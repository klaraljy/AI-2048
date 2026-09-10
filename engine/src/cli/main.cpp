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

#include "ai2048/ai2048.h"
#include "core/board.h"
#include "core/game.h"

namespace {

using ai2048::Direction;
using ai2048::Game;

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
// 确定性对局
//
// 走子策略是**刻意平凡的**：按固定顺序取第一个合法方向。
// 里程碑 1 要证明的是"引擎确定"，不是"AI 强"。这个策略的存在意义是让一局
// 能跑到终局，从而验证走子/合并/生成/终局判定整条链路。
// 真正的 AI 属于里程碑 2，会以策略对象的形式接进来。
// ---------------------------------------------------------------------------

inline constexpr std::array<Direction, 4> kPolicyOrder = {Direction::kLeft, Direction::kDown,
                                                          Direction::kRight, Direction::kUp};

struct PlayOutcome {
  std::string final_state;
  std::uint64_t score = 0;
  std::uint32_t steps = 0;
  int max_exponent = 0;
  bool overflow = false;
  double microseconds = 0.0;
};

[[nodiscard]] PlayOutcome PlayOneGame(std::uint64_t seed) {
  const auto begin = std::chrono::steady_clock::now();

  Game game(seed);
  while (!game.game_over()) {
    const std::optional<Direction> direction = ai2048::FindAnyLegalMove(game.board());
    if (!direction.has_value()) break;
    // 里程碑 1 只关心终局结果与确定性，不看单步细节。
    static_cast<void>(game.Step(*direction));
  }

  const auto end = std::chrono::steady_clock::now();

  PlayOutcome outcome;
  outcome.final_state = game.Serialize();
  outcome.score = game.score();
  outcome.steps = game.step_count();
  outcome.max_exponent = game.max_exponent();
  outcome.overflow = game.saw_overflow();
  outcome.microseconds = std::chrono::duration<double, std::micro>(end - begin).count();
  return outcome;
}

// ---------------------------------------------------------------------------
// selfcheck：里程碑 1 的验收命令
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

  std::cout << "selfcheck: 种子集 " << options.seeds_path << "，共 " << seeds->size() << " 局\n";

  int failures = 0;
  for (const std::uint64_t seed : *seeds) {
    const PlayOutcome first = PlayOneGame(seed);
    const PlayOutcome second = PlayOneGame(seed);
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
  double mean_milliseconds = 0.0;
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

  const auto started = std::chrono::steady_clock::now();

  auto worker = [&]() {
    while (true) {
      const std::size_t index = next.fetch_add(1);
      if (index >= seeds->size()) return;

      // 每局的结果只由 (*seeds)[index] 决定，与它跑在哪个线程无关 ——
      // 这是"并行不改变结果"的必要条件，也是 selfcheck 能覆盖到的部分。
      outcomes[index] = PlayOneGame((*seeds)[index]);

      const std::size_t done = index + 1;
      if (done % 100 == 0 || done == seeds->size()) {
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
    summary.mean_milliseconds += entry.microseconds / 1000.0;
    summary.min_score = std::min(summary.min_score, entry.score);
    summary.max_score = std::max(summary.max_score, entry.score);
    if (entry.overflow) ++summary.overflow_games;
    for (int k = 1; k < static_cast<int>(summary.reached.size()); ++k) {
      if (entry.max_exponent >= k) ++summary.reached[static_cast<std::size_t>(k)];
    }
  }
  summary.mean_score /= summary.games;
  summary.mean_steps /= summary.games;
  summary.mean_milliseconds /= summary.games;

  std::cout << "规则集版本 : " << ai2048::RulesetVersion() << "\n";
  if (!options.tag.empty()) std::cout << "标记       : " << options.tag << "\n";
  std::cout << "局数       : " << summary.games << "\n";
  std::cout << "平均分     : " << static_cast<std::uint64_t>(std::llround(summary.mean_score))
            << "\n";
  std::cout << "分数区间   : " << summary.min_score << " .. " << summary.max_score << "\n";
  std::cout << "平均步数   : " << static_cast<std::uint64_t>(std::llround(summary.mean_steps))
            << "\n";
  std::cout << "平均每局   : "
            << static_cast<std::uint64_t>(std::llround(summary.mean_milliseconds)) << " ms\n";
  std::cout << "总耗时     : " << wall_seconds << " s（" << threads << " 线程）\n";
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
// play：单局演示，用于里程碑 1 的当场演示
// ---------------------------------------------------------------------------

int RunPlay(const Options& options) {
  Game game(options.seed);
  std::cout << "seed=" << game.seed() << "  规则集=" << ai2048::RulesetVersion() << "\n";
  std::cout << ai2048::ToString(game.board()) << "\n\n";

  while (!game.game_over()) {
    const std::optional<Direction> direction = ai2048::FindAnyLegalMove(game.board());
    if (!direction.has_value()) break;

    const ai2048::StepResult step = game.Step(*direction);
    if (!step.moved) break;

    const bool show =
        options.every > 0 && (game.step_count() % static_cast<std::uint32_t>(options.every) == 0);
    if (show) {
      std::cout << "第 " << game.step_count() << " 步 " << ai2048::DirectionName(*direction)
                << "  +" << step.score_gained << "  分数=" << game.score()
                << "  最大块=" << game.max_tile() << "  轨迹=" << step.moves.size() << " 块\n";
      std::cout << ai2048::ToString(game.board()) << "\n\n";
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
            << "用法:\n"
            << "  ai2048-cli version\n"
            << "  ai2048-cli play      [--seed N] [--every N]      跑一局并打印，用于演示\n"
            << "  ai2048-cli selfcheck --seeds <文件>              同种子重跑两次，校验逐字节一致\n"
            << "  ai2048-cli bench     --seeds <文件> [--limit N] [--threads N] [--tag T]\n";
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
