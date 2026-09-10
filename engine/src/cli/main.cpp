// 命令行入口：跑批、自检、单局演示。属于「消费者②」，见 AGENTS.md 的目录结构说明。
//
// 这个程序是引擎的消费者，不是引擎的一部分 —— 引擎核心（src/core、src/ai）
// 不依赖它，Android 侧也不会用到它。

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ai/evaluate.h"
#include "ai/search.h"
#include "ai2048/ai2048.h"
#include "core/board.h"
#include "core/game.h"

namespace {

using ai2048::Direction;
using ai2048::Game;
using ai2048::SearchConfig;
using ai2048::SearchResult;

// ---------------------------------------------------------------------------
// 参数解析（手写，不引第三方库 —— 总共就这么几个选项）
// ---------------------------------------------------------------------------

struct Options {
  std::string seeds_path;
  std::uint64_t seed = 0;
  int limit = 0;  // bench 最多跑几局；0 = 全部
  int threads = 0;
  int every = 0;  // play 每多少步打印一次；0 = 只打印开头与结尾
  std::string tag;
  int depth = 6;
  int time_budget_ms = 0;
  int chance_limit = 0;
  bool use_tt = true;
};

[[nodiscard]] bool ParseOptions(int argc, char** argv, Options* options) {
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_next = (i + 1) < argc;

    auto take = [&](std::string* out) {
      if (!has_next) return false;
      *out = argv[++i];
      return true;
    };
    auto take_int = [&](int* out) {
      std::string raw;
      if (!take(&raw)) return false;
      try {
        *out = std::stoi(raw);
      } catch (...) {
        return false;
      }
      return true;
    };

    if (arg == "--seeds") {
      if (!take(&options->seeds_path)) return false;
    } else if (arg == "--seed") {
      std::string raw;
      if (!take(&raw)) return false;
      try {
        options->seed = std::stoull(raw);
      } catch (...) {
        return false;
      }
    } else if (arg == "--limit") {
      if (!take_int(&options->limit)) return false;
    } else if (arg == "--threads") {
      if (!take_int(&options->threads)) return false;
    } else if (arg == "--every") {
      if (!take_int(&options->every)) return false;
    } else if (arg == "--depth") {
      if (!take_int(&options->depth)) return false;
    } else if (arg == "--time") {
      if (!take_int(&options->time_budget_ms)) return false;
    } else if (arg == "--chance-limit") {
      if (!take_int(&options->chance_limit)) return false;
    } else if (arg == "--no-tt") {
      options->use_tt = false;
    } else if (arg == "--tag") {
      if (!take(&options->tag)) return false;
    } else {
      std::cerr << "未知选项: " << arg << "\n";
      return false;
    }
  }
  return true;
}

[[nodiscard]] SearchConfig MakeSearchConfig(const Options& options) {
  SearchConfig config;
  config.base_depth = options.depth;
  config.time_budget_ms = options.time_budget_ms;
  config.chance_sample_limit = options.chance_limit;
  return config;
}

// 每局一张置换表 —— **跨步复用**，不要每步新建。
// 早期实现每步新建一张 16MB 的表，单局 939 步白花 8.8 秒，
// 而搜索本身只要 0.15 秒。详见 search.h 里 TranspositionTable 的说明。
[[nodiscard]] std::size_t MakeTableCapacity(const Options& options) {
  return options.use_tt ? ai2048::TranspositionTable::kDefaultCapacity : 0;
}

// ---------------------------------------------------------------------------
// 种子集读取
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<std::vector<std::uint64_t>> ReadSeeds(const std::string& path,
                                                                  std::string* error) {
  std::ifstream input(path);
  if (!input) {
    *error = "无法打开种子集文件: " + path;
    return std::nullopt;
  }

  std::vector<std::uint64_t> seeds;
  std::string line;
  int line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    // 去掉行尾的 \r / 空格（本仓库强制 LF，但外部文件可能是 CRLF）
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line.front() == '#') continue;

    try {
      std::size_t consumed = 0;
      const std::uint64_t value = std::stoull(line, &consumed);
      if (consumed != line.size()) throw std::invalid_argument("trailing characters");
      seeds.push_back(value);
    } catch (...) {
      *error = "第 " + std::to_string(line_number) + " 行不是合法种子: " + line;
      return std::nullopt;
    }
  }

  if (seeds.empty()) {
    *error = "种子集是空的: " + path;
    return std::nullopt;
  }
  return seeds;
}

// ---------------------------------------------------------------------------
// 用 AI 打一局
//
// 传入的 config 基深度会被逐局拷贝 —— 搜索数据（节点数等）不参与对局逻辑，
// 所以并行跑批时每局各用一份配置，互不干扰。
// ---------------------------------------------------------------------------

struct PlayOutcome {
  std::string final_state;
  std::uint64_t score = 0;
  std::uint32_t steps = 0;
  int max_exponent = 0;
  bool overflow = false;
  double microseconds = 0.0;
  std::uint64_t nodes = 0;
  double search_ms = 0.0;
  double step_ms = 0.0;
  int reached_depth = 0;
  bool timed_out = false;
};

[[nodiscard]] PlayOutcome PlayOneGame(std::uint64_t seed, const SearchConfig& config,
                                      std::size_t table_capacity) {
  const auto begin = std::chrono::steady_clock::now();

  Game game(seed);
  std::optional<Direction> last_move;
  // 每局一张表，局内跨步复用。表内容只由棋盘与深度决定，不依赖搜索历史，
  // 所以复用不影响结果 —— 但每局重新构造一份，保证同种子的两次运行完全一致。
  ai2048::TranspositionTable table(table_capacity);

  std::uint64_t nodes = 0;
  double search_ms = 0.0;
  int reached_depth = 0;
  bool timed_out = false;
  double step_ms = 0.0;

  while (!game.game_over()) {
    const SearchResult decision = ai2048::SearchBestMove(game.board(), config, &table, last_move);
    if (!decision.move.has_value()) break;

    nodes += decision.stats.nodes;
    search_ms += decision.stats.elapsed_ms;
    reached_depth = std::max(reached_depth, decision.stats.reached_depth);
    timed_out = timed_out || decision.stats.timed_out;

    const auto step_begin = std::chrono::steady_clock::now();
    const ai2048::StepResult step = game.Step(*decision.move);
    step_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - step_begin)
            .count();
    if (!step.moved) break;  // 搜索给出的方向不合法 —— 不该发生，防御性处理
    last_move = decision.move;
  }

  const auto end = std::chrono::steady_clock::now();

  PlayOutcome outcome;
  outcome.final_state = game.Serialize();
  outcome.score = game.score();
  outcome.steps = game.step_count();
  outcome.max_exponent = game.max_exponent();
  outcome.overflow = game.saw_overflow();
  outcome.microseconds = std::chrono::duration<double, std::micro>(end - begin).count();
  outcome.nodes = nodes;
  outcome.search_ms = search_ms;
  outcome.step_ms = step_ms;
  outcome.reached_depth = reached_depth;
  outcome.timed_out = timed_out;
  return outcome;
}

// ---------------------------------------------------------------------------
// selfcheck：同种子重跑两次，校验逐字节一致
//
// 注意：**不限时**时搜索是完全确定的。加了 --time 之后会引入机器相关的
// 深度差异，那时 selfcheck 可能失败 —— 这是预期行为，不是 bug。
// ---------------------------------------------------------------------------

int RunSelfCheck(const Options& options) {
  if (options.seeds_path.empty()) {
    std::cerr << "selfcheck 需要 --seeds <文件>\n";
    return 1;
  }

  std::string error;
  const auto seeds = ReadSeeds(options.seeds_path, &error);
  if (!seeds.has_value()) {
    std::cerr << error << "\n";
    return 1;
  }

  std::cout << "selfcheck: 种子集 " << options.seeds_path << "，共 " << seeds->size() << " 局"
            << "，深度 " << options.depth;
  if (options.time_budget_ms > 0) {
    std::cout << "，时间预算 " << options.time_budget_ms << "ms（会引入机器相关差异）";
  }
  std::cout << "\n";

  const SearchConfig config = MakeSearchConfig(options);
  const std::size_t table_capacity = MakeTableCapacity(options);
  int failures = 0;
  for (const std::uint64_t seed : *seeds) {
    const PlayOutcome first = PlayOneGame(seed, config, table_capacity);
    const PlayOutcome second = PlayOneGame(seed, config, table_capacity);
    if (first.final_state != second.final_state) {
      ++failures;
      std::cerr << "  ✗ seed " << seed << " 两次运行结果不一致\n"
                << "    第一次: " << first.final_state << "\n"
                << "    第二次: " << second.final_state << "\n";
      if (failures >= 5) {
        std::cerr << "  （已经 5 个不一致，提前停止）\n";
        break;
      }
    }
  }

  if (failures != 0) {
    std::cerr << "selfcheck 失败: " << failures << " 局不可复现\n";
    if (options.time_budget_ms > 0) {
      std::cerr << "提示: 使用了 --time，超时会按机器速度截断搜索深度。\n"
                << "      要验证确定性请去掉 --time。\n";
    }
    return 1;
  }
  std::cout << "selfcheck 通过: " << seeds->size() << " 局全部可复现（逐字节一致）\n";
  return 0;
}

// ---------------------------------------------------------------------------
// bench
// ---------------------------------------------------------------------------

struct Summary {
  int games = 0;
  double mean_score = 0.0;
  std::uint64_t min_score = 0;
  std::uint64_t max_score = 0;
  double mean_steps = 0.0;
  double mean_seconds = 0.0;
  double mean_nodes = 0.0;
  double mean_search_ms = 0.0;
  double mean_step_ms = 0.0;
  int max_reached_depth = 0;
  int timed_out_games = 0;
  std::array<int, 18> reached{};  // 到达 2^k 的局数，k = 1..17
  int overflow_games = 0;
};

int RunBench(const Options& options) {
  if (options.seeds_path.empty()) {
    std::cerr << "bench 需要 --seeds <文件>\n";
    return 1;
  }

  // 命令行参数在 Windows 上走的是 ANSI 代码页，不是 UTF-8 ——
  // 传中文 tag 会变成乱码。这里直接拒绝，避免乱码进入归档文件名。
  // 中文说明写在 docs/ 里，不用命令行传。
  for (const char ch : options.tag) {
    if (static_cast<unsigned char>(ch) >= 0x80) {
      std::cerr << "bench 的 --tag 只能用 ASCII 字符（收到非 ASCII 字节）。\n"
                << "原因：Windows 的命令行参数走 ANSI 代码页而不是 UTF-8，中文会变乱码，\n"
                << "而 tag 会被写进归档文件名。请用英文短标记，例如 M2-depth6-heuristics。\n";
      return 1;
    }
  }

  std::string error;
  auto seeds = ReadSeeds(options.seeds_path, &error);
  if (!seeds.has_value()) {
    std::cerr << error << "\n";
    return 1;
  }
  if (options.limit > 0 && static_cast<std::size_t>(options.limit) < seeds->size()) {
    seeds->resize(static_cast<std::size_t>(options.limit));
  }

  unsigned int threads = options.threads > 0 ? static_cast<unsigned int>(options.threads)
                                             : std::max(1u, std::thread::hardware_concurrency());
  threads = std::min<unsigned int>(threads, static_cast<unsigned int>(seeds->size()));

  std::vector<PlayOutcome> outcomes(seeds->size());
  std::mutex print_mutex;
  std::atomic<std::size_t> next{0};
  std::atomic<int> completed{0};

  const SearchConfig base_config = MakeSearchConfig(options);
  const std::size_t table_capacity = MakeTableCapacity(options);
  const auto started = std::chrono::steady_clock::now();

  auto worker = [&]() {
    while (true) {
      const std::size_t index = next.fetch_add(1);
      if (index >= seeds->size()) return;

      // 每局用独立的配置副本（含独立的置换表），互不干扰。
      // 每局的结果只由 (*seeds)[index] 决定，与它跑在哪个线程无关 ——
      // 这是"并行不改变结果"的必要条件。
      outcomes[index] = PlayOneGame((*seeds)[index], base_config, table_capacity);

      const int done = completed.fetch_add(1) + 1;
      if (done % 50 == 0 || static_cast<std::size_t>(done) == seeds->size()) {
        std::lock_guard<std::mutex> lock(print_mutex);
        std::cout << "\r  进度 " << done << "/" << seeds->size() << std::flush;
      }
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (unsigned int i = 0; i < threads; ++i) pool.emplace_back(worker);
  for (std::thread& thread : pool) thread.join();

  const double wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "\n";

  Summary summary;
  summary.games = static_cast<int>(outcomes.size());
  summary.min_score = outcomes.front().score;
  for (const PlayOutcome& entry : outcomes) {
    summary.mean_score += static_cast<double>(entry.score);
    summary.mean_steps += entry.steps;
    summary.mean_seconds += entry.microseconds / 1e6;
    summary.mean_nodes += static_cast<double>(entry.nodes);
    summary.mean_search_ms += entry.search_ms;
    summary.mean_step_ms += entry.step_ms;
    summary.max_reached_depth = std::max(summary.max_reached_depth, entry.reached_depth);
    if (entry.timed_out) ++summary.timed_out_games;
    summary.min_score = std::min(summary.min_score, entry.score);
    summary.max_score = std::max(summary.max_score, entry.score);
    if (entry.overflow) ++summary.overflow_games;
    for (int k = 1; k < static_cast<int>(summary.reached.size()); ++k) {
      if (entry.max_exponent >= k) ++summary.reached[static_cast<std::size_t>(k)];
    }
  }
  summary.mean_score /= summary.games;
  summary.mean_steps /= summary.games;
  summary.mean_seconds /= summary.games;
  summary.mean_nodes /= summary.games;
  summary.mean_search_ms /= summary.games;

  std::cout << "规则集版本 : " << ai2048::RulesetVersion() << "\n";
  if (!options.tag.empty()) std::cout << "标记       : " << options.tag << "\n";
  std::cout << "基础深度   : " << options.depth;
  if (options.time_budget_ms > 0) std::cout << "，时间预算 " << options.time_budget_ms << "ms";
  if (options.chance_limit > 0) std::cout << "，chance 采样上限 " << options.chance_limit;
  std::cout << "\n";
  std::cout << "局数       : " << summary.games << "\n";
  std::cout << "平均分     : " << static_cast<std::uint64_t>(std::llround(summary.mean_score))
            << "\n";
  std::cout << "分数区间   : " << summary.min_score << " .. " << summary.max_score << "\n";
  std::cout << "平均步数   : " << static_cast<std::uint64_t>(std::llround(summary.mean_steps))
            << "\n";
  std::cout << "平均每局   : " << summary.mean_seconds << " s（其中搜索 "
            << summary.mean_search_ms / 1000.0 << " s，走子 " << summary.mean_step_ms / 1000.0
            << " s，其它 "
            << (summary.mean_seconds - summary.mean_search_ms / 1000.0 -
                summary.mean_step_ms / 1000.0)
            << " s）\n";
  std::cout << "平均节点   : " << static_cast<std::uint64_t>(std::llround(summary.mean_nodes))
            << " / 局\n";
  std::cout << "最深达到   : " << summary.max_reached_depth << " 层\n";
  std::cout << "总耗时     : " << wall_seconds << " s（" << threads << " 线程）\n";
  if (summary.timed_out_games > 0) {
    std::cout << "超时局数   : " << summary.timed_out_games << "（结果与机器速度相关）\n";
  }
  if (summary.overflow_games > 0) {
    std::cout << "碰到上限   : " << summary.overflow_games
              << " 局在 32768 处发生饱和合并（见 kMaxExponent）\n";
  }
  std::cout << "到达率     :";
  for (int k = 7; k < static_cast<int>(summary.reached.size()); ++k) {
    const std::uint64_t tile = std::uint64_t{1} << k;
    const double percent = 100.0 * summary.reached[static_cast<std::size_t>(k)] / summary.games;
    std::cout << "  " << tile << "=" << static_cast<int>(std::llround(percent)) << "%";
  }
  std::cout << "\n";

  // TODO(里程碑 5): 把结果按 <种子集版本>-<tag>.json 归档到 docs/results/。
  // 归档时必须同时写入规则集版本与种子集版本，否则分数不可比。
  return 0;
}

// ---------------------------------------------------------------------------
// play：单局演示
// ---------------------------------------------------------------------------

int RunPlay(const Options& options) {
  const SearchConfig config = MakeSearchConfig(options);
  Game game(options.seed);
  std::optional<Direction> last_move;
  ai2048::TranspositionTable table(MakeTableCapacity(options));

  std::cout << "seed=" << game.seed() << "  规则集=" << ai2048::RulesetVersion()
            << "  基础深度=" << options.depth;
  if (options.time_budget_ms > 0) std::cout << "  时间预算=" << options.time_budget_ms << "ms";
  std::cout << "\n";
  std::cout << ai2048::ToString(game.board()) << "\n\n";

  while (!game.game_over()) {
    const SearchResult decision = ai2048::SearchBestMove(game.board(), config, &table, last_move);
    if (!decision.move.has_value()) break;

    const ai2048::StepResult step = game.Step(*decision.move);
    if (!step.moved) break;
    last_move = decision.move;

    const bool show =
        options.every > 0 && (game.step_count() % static_cast<std::uint32_t>(options.every) == 0);
    if (show) {
      std::cout << "第 " << game.step_count() << " 步 " << ai2048::DirectionName(*decision.move)
                << "  +" << step.score_gained << "  分数=" << game.score()
                << "  最大块=" << game.max_tile() << "  深度=" << decision.stats.reached_depth
                << "  节点=" << decision.stats.nodes << "  耗时=" << decision.stats.elapsed_ms
                << "ms\n";
      std::cout << ai2048::ToString(game.board()) << "\n";
      for (const auto& evaluation : decision.evaluations) {
        std::cout << "    " << ai2048::DirectionName(evaluation.direction)
                  << "  评分=" << evaluation.total_score << "\n";
      }
      std::cout << "\n";
    }
  }

  std::cout << "终局\n" << ai2048::ToString(game.board()) << "\n";
  std::cout << "步数=" << game.step_count() << "  分数=" << game.score()
            << "  最大块=" << game.max_tile() << "\n";
  return 0;
}

void PrintUsage() {
  std::cout << "ai2048-cli " << ai2048::VersionString() << " (ruleset " << ai2048::RulesetVersion()
            << ")\n"
            << "通用选项:\n"
            << "  --depth N         基础搜索深度，默认 6（偶数更自然）\n"
            << "  --time N          每步时间预算（毫秒）。0 = 不限时（完全确定，可复现）\n"
            << "  --chance-limit N  chance 节点采样上限。0 = 枚举全部空格\n"
            << "  --no-tt           关闭置换表\n"
            << "子命令:\n"
            << "  version\n"
            << "  play      [--seed N] [--every N]              让 AI 跑一局并打印\n"
            << "  selfcheck --seeds <文件>                      同种子重跑两次，校验逐字节一致\n"
            << "  bench     --seeds <文件> [--limit N] [--threads N] [--tag T]\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view command = (argc > 1) ? argv[1] : "";

  if (command.empty() || command == "-h" || command == "--help") {
    PrintUsage();
    return 0;
  }

  if (command == "version") {
    std::cout << "ai2048 " << ai2048::VersionString() << " (ruleset " << ai2048::RulesetVersion()
              << ")\n";
    return 0;
  }

  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    std::cerr << "\n";
    PrintUsage();
    return 1;
  }

  if (command == "play") return RunPlay(options);
  if (command == "selfcheck") return RunSelfCheck(options);
  if (command == "bench") return RunBench(options);

  std::cerr << "未知子命令: " << command << "\n\n";
  PrintUsage();
  return 1;
}
