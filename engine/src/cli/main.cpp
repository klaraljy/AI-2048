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
#include "core/console.h"
#include "core/game.h"
#include "net/json.h"

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
  bool symmetry_keys = false;
  std::string move_spec;                // move 子命令：16 个指数
  std::string move_direction = "left";  // move 子命令：方向名
  bool move_stdin = false;              // move 子命令：从 stdin 批量读
  // 难度改变新方块的**位置分布**，因此改变规则。默认 normal = 标准 2048。
  // 跑批结果必须带难度标记：分数不跨难度可比。
  ai2048::Difficulty difficulty = ai2048::Difficulty::kNormal;
  ai2048::Weights weight_overrides;
};

// 解析 "key=value,key=value,..."。键用短名，便于命令行书写。
[[nodiscard]] bool ParseWeightOverrides(const std::string& spec, ai2048::Weights* weights);

// 方向名 -> 枚举。无法识别返回 nullopt。
[[nodiscard]] std::optional<Direction> DirectionFromName(const std::string& name) {
  if (name == "up") return Direction::kUp;
  if (name == "down") return Direction::kDown;
  if (name == "left") return Direction::kLeft;
  if (name == "right") return Direction::kRight;
  return std::nullopt;
}

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
    } else if (arg == "--symmetry") {
      options->symmetry_keys = true;
    } else if (arg == "--difficulty") {
      std::string name;
      if (!take(&name)) return false;
      if (name == "normal") {
        options->difficulty = ai2048::Difficulty::kNormal;
      } else if (name == "easy") {
        options->difficulty = ai2048::Difficulty::kEasy;
      } else if (name == "hard") {
        options->difficulty = ai2048::Difficulty::kHard;
      } else {
        std::cerr << "无法识别 --difficulty: " << name << "（可选 normal / easy / hard）\n";
        return false;
      }
    } else if (arg == "--board") {
      if (!take(&options->move_spec)) return false;
    } else if (arg == "--dir") {
      if (!take(&options->move_direction)) return false;
    } else if (arg == "--stdin") {
      options->move_stdin = true;
    } else if (arg == "--weights") {
      // 形如 empty=270,empty_late=700,mono=47,smooth=32,merge=18,corner=2200,snake=0.35,maxtile=12
      // 用于自动调参：每次用一整套权重跑一批对局，比较分数。
      std::string spec;
      if (!take(&spec)) return false;
      if (!ParseWeightOverrides(spec, &options->weight_overrides)) {
        std::cerr << "无法解析 --weights: " << spec << "\n";
        return false;
      }
    } else if (arg == "--tag") {
      if (!take(&options->tag)) return false;
    } else {
      std::cerr << "未知选项: " << arg << "\n";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 权重覆盖（自动调参用）
// ---------------------------------------------------------------------------

// 解析 "key=value,key=value,..."。键用短名，便于命令行书写。
[[nodiscard]] bool ParseWeightOverrides(const std::string& spec, ai2048::Weights* weights) {
  std::size_t pos = 0;
  while (pos < spec.size()) {
    const std::size_t comma = spec.find(',', pos);
    const std::string token =
        spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = (comma == std::string::npos) ? spec.size() : comma + 1;
    if (token.empty()) continue;

    const std::size_t eq = token.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = token.substr(0, eq);
    float value = 0.0F;
    try {
      value = std::stof(token.substr(eq + 1));
    } catch (...) {
      return false;
    }

    if (key == "empty") {
      weights->empty = value;
    } else if (key == "empty_late") {
      weights->empty_late = value;
    } else if (key == "mono") {
      weights->monotonicity = value;
    } else if (key == "smooth") {
      weights->smoothness = value;
    } else if (key == "merge") {
      weights->merge = value;
    } else if (key == "corner") {
      weights->corner = value;
    } else if (key == "snake") {
      weights->snake = value;
    } else if (key == "maxtile") {
      weights->max_tile = value;
    } else if (key == "cc") {
      weights->corner_control = value;
    } else if (key == "edge") {
      weights->edge_support = value;
    } else if (key == "grad") {
      weights->gradient = value;
    } else if (key == "bias") {
      weights->anchor_bias = value;
    } else {
      std::cerr << "未知权重键: " << key << "\n";
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
  config.weights = options.weight_overrides;
  // AI 的**世界模型**必须与实际游戏的生成规则一致：
  // 不传的话 AI 一律按全盘均匀评估，hard 档下会低估
  // "新块贴着自己最大块出现"的风险，走子偏乐观。
  config.difficulty = options.difficulty;
  return config;
}

// 每局一张置换表 —— **跨步复用**，不要每步新建。
// 早期实现每步新建一张 16MB 的表，单局 939 步白花 8.8 秒，
// 而搜索本身只要 0.15 秒。详见 search.h 里 TranspositionTable 的说明。
[[nodiscard]] std::size_t MakeTableCapacity(const Options& options) {
  return options.use_tt ? ai2048::TranspositionTable::kDefaultCapacity : 0;
}

[[nodiscard]] bool MakeUseSymmetryKeys(const Options& options) {
  return options.use_tt && options.symmetry_keys;
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
                                      std::size_t table_capacity, bool symmetry_keys,
                                      ai2048::Difficulty difficulty = ai2048::Difficulty::kNormal) {
  const auto begin = std::chrono::steady_clock::now();

  Game game(seed, difficulty);
  std::optional<Direction> last_move;
  // 每局一张表，局内跨步复用。表内容只由棋盘与深度决定，不依赖搜索历史，
  // 所以复用不影响结果 —— 但每局重新构造一份，保证同种子的两次运行完全一致。
  ai2048::TranspositionTable table(table_capacity, symmetry_keys);

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
  const bool symmetry_keys = MakeUseSymmetryKeys(options);
  int failures = 0;
  for (const std::uint64_t seed : *seeds) {
    const PlayOutcome first = PlayOneGame(seed, config, table_capacity, symmetry_keys);
    const PlayOutcome second = PlayOneGame(seed, config, table_capacity, symmetry_keys);
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
  const bool symmetry_keys = MakeUseSymmetryKeys(options);
  const auto started = std::chrono::steady_clock::now();

  auto worker = [&]() {
    while (true) {
      const std::size_t index = next.fetch_add(1);
      if (index >= seeds->size()) return;

      // 每局用独立的配置副本（含独立的置换表），互不干扰。
      // 每局的结果只由 (*seeds)[index] 决定，与它跑在哪个线程无关 ——
      // 这是"并行不改变结果"的必要条件。
      outcomes[index] = PlayOneGame((*seeds)[index], base_config, table_capacity, symmetry_keys,
                                    options.difficulty);

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
  // 难度改变生成位置 = 改变规则，所以必须打出来。
  // 不打印的话，两份不同难度的跑批结果看起来完全一样，会被误当成可比。
  std::cout << "难度       : " << ai2048::DifficultyName(options.difficulty);
  if (options.difficulty != ai2048::Difficulty::kNormal) {
    std::cout << "（**非标准规则**，分数不可与 normal 比较）";
  }
  std::cout << "\n";
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
  ai2048::TranspositionTable table(MakeTableCapacity(options), MakeUseSymmetryKeys(options));

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

// ---------------------------------------------------------------------------
// move：对给定棋盘执行一次走子，输出结果行
//
// 供前端做**逐行穷举对拍**：前端本地规则引擎必须与引擎逐位一致。
// 只跑若干整局是不够的 —— 随机对局会漏掉罕见的方向/合并组合
// （实测就漏掉过一个"下移把棋盘清空"的 bug）。
//
// 输入：16 个指数（0 = 空），行优先。
// 输出：16 个指数 + 得分。
// ---------------------------------------------------------------------------

int RunMove(const Options& options) {
  // 两种模式：
  //   --board <16 个指数> --dir <方向>   单次
  //   --stdin                            批量：每行 "<16 个指数> <方向>"
  // 批量模式是为穷举对拍准备的 —— 逐次起进程太慢（26 万次要几分钟）。
  if (options.move_stdin) {
    std::string line;
    while (std::getline(std::cin, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.empty() || line.front() == '#') continue;

      const std::size_t space = line.rfind(' ');
      if (space == std::string::npos) {
        std::cerr << "格式错误（应为 \"<16 个指数> <方向>\"）: " << line << "\n";
        return 1;
      }
      const std::string board_spec = line.substr(0, space);
      const std::string dir_name = line.substr(space + 1);

      const std::optional<Direction> direction = DirectionFromName(dir_name);
      if (!direction.has_value()) {
        std::cerr << "未知方向: " << dir_name << "\n";
        return 1;
      }

      std::array<int, ai2048::kCellCount> exponents{};
      std::size_t pos = 0;
      bool ok = true;
      for (int i = 0; i < ai2048::kCellCount; ++i) {
        const std::size_t comma = board_spec.find(',', pos);
        const std::string token =
            board_spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? board_spec.size() : comma + 1;
        try {
          exponents[static_cast<std::size_t>(i)] = std::stoi(token);
        } catch (...) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        std::cerr << "无法解析棋盘: " << board_spec << "\n";
        return 1;
      }

      const ai2048::MoveResult result =
          ai2048::ApplyMove(ai2048::EncodeBoard(exponents), *direction);
      const std::array<int, ai2048::kCellCount> out = ai2048::DecodeBoard(result.board);
      for (int i = 0; i < ai2048::kCellCount; ++i) {
        std::cout << out[static_cast<std::size_t>(i)] << ',';
      }
      std::cout << result.score_gained << "\n";
    }
    return 0;
  }

  if (options.move_spec.empty()) {
    std::cerr << "move 需要 --board <16 个指数> --dir <方向>，或用 --stdin 批量\n";
    return 1;
  }

  std::array<int, ai2048::kCellCount> exponents{};
  {
    std::size_t pos = 0;
    for (int i = 0; i < ai2048::kCellCount; ++i) {
      const std::size_t comma = options.move_spec.find(',', pos);
      const std::string token = options.move_spec.substr(
          pos, comma == std::string::npos ? std::string::npos : comma - pos);
      pos = (comma == std::string::npos) ? options.move_spec.size() : comma + 1;
      try {
        exponents[static_cast<std::size_t>(i)] = std::stoi(token);
      } catch (...) {
        std::cerr << "无法解析第 " << i << " 个格子: " << token << "\n";
        return 1;
      }
    }
  }

  const std::optional<Direction> direction = DirectionFromName(options.move_direction);
  if (!direction.has_value()) {
    std::cerr << "未知方向: " << options.move_direction << "（可用 up/down/left/right）\n";
    return 1;
  }

  const ai2048::MoveResult result = ai2048::ApplyMove(ai2048::EncodeBoard(exponents), *direction);

  const std::array<int, ai2048::kCellCount> out = ai2048::DecodeBoard(result.board);
  for (int i = 0; i < ai2048::kCellCount; ++i) {
    std::cout << out[static_cast<std::size_t>(i)] << (i + 1 == ai2048::kCellCount ? '\n' : ',');
  }
  std::cout << result.score_gained << "\n";
  return 0;
}

// ---------------------------------------------------------------------------
// trace：把一局的最终状态打一行出来，供前端做规则对拍
//
// 前端有一份降级用的本地规则引擎（web/js/game.js）。两者**必须完全一致** ——
// 否则"引擎在跑"和"引擎连不上"会得到不同的对局，那比不能玩更糟。
//
// 走子策略用**固定顺序的第一个合法方向**（不用 AI），这样对拍只考验规则本身，
// 不受搜索实现影响。
// ---------------------------------------------------------------------------

int RunTrace(const Options& options) {
  if (options.seeds_path.empty()) {
    std::cerr << "trace 需要 --seeds <文件>\n";
    return 1;
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

  constexpr std::array<Direction, 4> kOrder = {Direction::kLeft, Direction::kDown,
                                               Direction::kRight, Direction::kUp};
  // 无参数 = 标准 2048。传 --difficulty 才能对拍另两档 ——
  // 前端实现了三档，所以三档都必须能逐位对拍，不能只验 normal。
  const ai2048::Difficulty difficulty = options.difficulty;
  for (const std::uint64_t seed : *seeds) {
    Game game(seed, difficulty);
    while (!game.game_over()) {
      bool advanced = false;
      for (const Direction direction : kOrder) {
        if (game.Step(direction).moved) {
          advanced = true;
          break;
        }
      }
      if (!advanced) break;
    }
    // 与 Game::Serialize() 同格式，但压成一行便于逐行对拍
    std::string state = game.Serialize();
    std::string flat;
    flat.reserve(state.size());
    for (const char ch : state) {
      flat += (ch == '\n') ? '|' : ch;
    }
    std::cout << flat << "\n";
  }
  return 0;
}

// ---------------------------------------------------------------------------
// json：把 stdin 的每一行当作一个 JSON 解析，输出规范化结果
//
// 供与标准实现（Node 的 JSON.parse）**逐例对拍**。
// 自己写的解析器只有跟一个独立实现比过才可信 —— 自己跟自己对只能证明稳定。
// ---------------------------------------------------------------------------

// 把字符串按 JSON 规则转义（含引号）。与 Node 的 JSON.stringify 对齐。
[[nodiscard]] std::string EscapeJsonString(const std::string& text) {
  std::string out = "\"";
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          out += buffer;
        } else {
          out += static_cast<char>(ch);
        }
        break;
    }
  }
  out += '"';
  return out;
}

int RunJsonCheck() {
  // 规范化形式：对象键按升序、字符串转义、数字用 17 位有效数字。
  // 这些规则与 Node 侧的生成脚本严格对应，否则对拍会因格式差异误报。
  auto canonical = [](const ai2048::net::json::Value& value, auto&& self) -> std::string {
    using ai2048::net::json::Type;
    switch (value.type()) {
      case Type::kNull:
        return "null";
      case Type::kBool:
        return value.AsBool() ? "true" : "false";
      case Type::kNumber: {
        // 与 Node 侧严格对应的规范化：1 位整数部分 + 16 位小数 + 十进制指数，
        // 去掉尾随零。这就是 %.16e 的格式。
        // 必须先统一格式，否则会把 "1" 与 "1.0000000000000000" 这种
        // **格式**差异误报成解析差异。
        const double number = value.AsNumber();
        if (!std::isfinite(number)) return "nonfinite";
        if (number == 0.0 && std::signbit(number)) return "-0.0000000000000000e+0";

        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.16e", number);
        std::string text(buffer);

        const std::size_t epos = text.find('e');
        std::string mantissa = text.substr(0, epos);
        const int exponent = std::atoi(text.c_str() + epos + 1);

        while (mantissa.size() > 2 && mantissa.back() == '0') mantissa.pop_back();
        if (!mantissa.empty() && mantissa.back() == '.') mantissa.pop_back();

        return mantissa + "e" + std::to_string(exponent);
      }
      case Type::kString:
        return EscapeJsonString(value.AsString());
      case Type::kArray: {
        std::string out = "[";
        const auto& array = value.AsArray();
        for (std::size_t i = 0; i < array.size(); ++i) {
          if (i != 0) out += ",";
          out += self(array[i], self);
        }
        out += "]";
        return out;
      }
      case Type::kObject: {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, member] : value.AsObject()) {  // std::map 已按 key 升序
          if (!first) out += ",";
          first = false;
          out += EscapeJsonString(key);
          out += ":";
          out += self(member, self);
        }
        out += "}";
        return out;
      }
    }
    return "null";
  };

  std::string line;
  while (std::getline(std::cin, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) continue;

    const ai2048::net::json::ParseResult result = ai2048::net::json::Parse(line);
    if (!result.ok) {
      std::cout << "ERR\n";
      continue;
    }
    std::cout << "OK " << canonical(result.value, canonical) << "\n";
  }
  return 0;
}

void PrintUsage() {
  std::cout
      << "ai2048-cli " << ai2048::VersionString() << " (ruleset " << ai2048::RulesetVersion()
      << ")\n"
      << "通用选项:\n"
      << "  --depth N         基础搜索深度，默认 6（偶数更自然）\n"
      << "  --time N          每步时间预算（毫秒）。0 = 不限时（完全确定，可复现）\n"
      << "  --chance-limit N  chance 节点采样上限。0 = 枚举全部空格\n"
      << "  --no-tt           关闭置换表\n"
      << "  --symmetry        置换表用 8 重对称键（有正确性代价，见 search.h）\n"
      << "子命令:\n"
      << "  version\n"
      << "  play      [--seed N] [--every N]              让 AI 跑一局并打印\n"
      << "  move      --board <16 个指数> --dir <方向>    对给定棋盘走一步，输出结果\n"
      << "  move      --stdin                            批量模式，每行 \"<16 个指数> <方向>\"\n"
      << "  trace     --seeds <文件> [--limit N]          每局输出一行状态，供前端规则对拍\n"
      << "  selfcheck --seeds <文件>                      同种子重跑两次，校验逐字节一致\n"
      << "  bench     --seeds <文件> [--limit N] [--threads N] [--tag T]\n";
}

}  // namespace

int main(int argc, char** argv) {
  // 双击运行时控制台默认是 GBK，中文会显示成乱码。必须在任何输出之前切到 UTF-8。
  // 输出被重定向时这个调用是空操作，字节保持原样（跑批与对拍都按 UTF-8 读）。
  ai2048::EnableUtf8Console();

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
  if (command == "move") return RunMove(options);
  if (command == "trace") return RunTrace(options);
  if (command == "json") return RunJsonCheck();
  if (command == "selfcheck") return RunSelfCheck(options);
  if (command == "bench") return RunBench(options);

  std::cerr << "未知子命令: " << command << "\n\n";
  PrintUsage();
  return 1;
}
