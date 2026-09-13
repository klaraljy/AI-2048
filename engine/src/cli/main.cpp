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
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ai/evaluate.h"
#include "ai/learn/search_bridge.h"
#include "ai/learn/trainer.h"
#include "ai/learn/value_network.h"
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
  /**
   * 自适应深度的**上限**。0 = 用 SearchConfig 的默认值。
   *
   * 需要这个开关，是因为默认的 max_depth 正好等于 base_depth，于是
   * "方向少时加深"的加成会被完全夹掉 —— 等于自适应只减不增。
   * 放开上限意味着**深搜那一步可能很贵**，所以它必须能被单独对拍，
   * 而不是我凭"文档说该更深"就替用户决定。
   */
  int max_depth = 0;
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
  // 置换表条目数；0 表示用默认容量。用于对照"表越大是否越好"。
  int tt_entries = 0;
  // train 子命令：TD 学习的超参数
  int train_games = 200;
  int train_eval_every = 25;
  int train_eval_games = 12;
  double train_lr = 0.002;
  /**
   * 训练线程数。0 = 用满硬件线程，1 = 单线程。
   *
   * **默认 1 是实测结论，不是保守。** 本机（Ryzen 5 5500U，6 核 12 线程）
   * 跑纯浮点负载的并行扩展性：
   *
   *     线程数  1      2      4      6      8      12
   *     耗时    94.6   104.7  162.6  209.9  244.2  253.2  ms
   *
   * **线程越多越慢**，12 线程比单线程慢 2.7 倍。高优先级重测结果相同，
   * 所以不是被别的进程挤掉，而是这台笔记本的功耗/频率管理：
   * 单核能冲高频，多核并发立刻掉到基频；SMT 那 6 个逻辑核还要跟物理核
   * 抢执行单元。实测 12 线程训练只快 1.05 倍（噪声级别）。
   *
   * 并行代码留着（它是对的，见 TrainConfig::threads 的确定性说明，
   * 已用权重哈希验证过任意线程数逐位一致），但默认关掉 ——
   * 在拿到"目标机器上并行确实更快"的测量之前，不该让默认值假装有收益。
   */
  int train_threads = 1;
  /** n-step 回报步数，1 = TD(0)。见 TrainConfig::nstep。 */
  int train_nstep = 1;
  /** ε 衰减到 epsilon_end 所需的局数（0 = 按总局数线性衰减的旧行为）。 */
  int train_decay_games = 300000;
  /** 折扣 γ。 */
  double train_discount = 0.98;
  /** 每批收集的局数（见 TrainConfig::batch_games）。 */
  int train_batch = 512;
  std::string out_path;
  /**
   * 已训练权重的路径。这是"用学习出来的评估替代手写启发式"的开关。
   *
   * 为空 = 手写启发式（历史行为，所有既有基准都是那个配置下测的）。
   */
  std::string net_file;
  /** 网络布局：rows / mixed（默认）/ six1 / six2 / six4 / serp。 */
  std::string net_layout = "mixed";
  ai2048::Weights weight_overrides;

  // --- compare 子命令：A/B 两套配置的**配对**对拍 ---------------------------
  //
  // 为什么要有这个命令：用两个独立跑批的**均值**比大小，在 100 局规模上
  // 标准误约 1500 分（局标准差约 15000），3~4% 的差异只有 0.7~1.1σ，
  // 根本分不出来 —— 这正是"长时间小提升"的陷阱。
  //
  // 配对就不一样：让 A、B 跑**同一批种子**，同一局里生成序列完全相同，
  // 只有 AI 决策不同，于是局间方差被消掉，剩下的是"同一局里 B 比 A 多拿多少"。
  // 逐局差值的标准误通常只有独立比较的几分之一，百局量级就能看出 5% 的差异。
  // --- compare 子命令：A/B 两套配置的**配对**对拍 ---------------------------
  //
  // 为什么要有这个子命令：用两个独立跑批的**均值**比大小，在 100 局规模上
  // 标准误约 1500 分（局标准差约 15000），3~4% 的差异只有 0.7~1.1σ，
  // 根本分不出来 —— 这正是"长时间小提升"的陷阱。
  //
  // 配对就不一样：让 A、B 跑**同一批种子**，同一局里生成序列完全相同，
  // 只有 AI 决策不同，于是局间方差被消掉，剩下的是"同一局里 B 比 A 多拿多少"。
  // 逐局差值的标准误通常只有独立比较的几分之一。
  /** B 组的基础深度（树层数）。0 = 与 A 组相同。 */
  int depth_b = 0;
  /** B 组的时间预算；-1 = 与 A 组相同（0 是合法值，代表不限时）。 */
  int time_budget_b = -1;
  bool has_weight_b = false;
  ai2048::Weights weight_overrides_b;
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
    } else if (arg == "--max-depth") {
      if (!take_int(&options->max_depth)) return false;
    } else if (arg == "--time") {
      if (!take_int(&options->time_budget_ms)) return false;
    } else if (arg == "--chance-limit") {
      if (!take_int(&options->chance_limit)) return false;
    } else if (arg == "--no-tt") {
      options->use_tt = false;
    } else if (arg == "--tt-entries") {
      if (!take_int(&options->tt_entries)) return false;
    } else if (arg == "--games") {
      if (!take_int(&options->train_games)) return false;
    } else if (arg == "--eval-every") {
      if (!take_int(&options->train_eval_every)) return false;
    } else if (arg == "--eval-games") {
      if (!take_int(&options->train_eval_games)) return false;
    } else if (arg == "--threads") {
      if (!take_int(&options->train_threads)) return false;
    } else if (arg == "--batch") {
      if (!take_int(&options->train_batch)) return false;
    } else if (arg == "--nstep") {
      if (!take_int(&options->train_nstep)) return false;
    } else if (arg == "--decay-games") {
      if (!take_int(&options->train_decay_games)) return false;
    } else if (arg == "--discount") {
      std::string raw;
      if (!take(&raw)) return false;
      try {
        options->train_discount = std::stod(raw);
      } catch (...) {
        return false;
      }
    } else if (arg == "--lr") {
      std::string raw;
      if (!take(&raw)) return false;
      try {
        options->train_lr = std::stod(raw);
      } catch (...) {
        return false;
      }
    } else if (arg == "--out") {
      if (!take(&options->out_path)) return false;
    } else if (arg == "--net-file") {
      if (!take(&options->net_file)) return false;
    } else if (arg == "--net") {
      if (!take(&options->net_layout)) return false;
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
      // 形如
      // empty=400,empty_late=700,mono=250,smooth=32,merge=30,corner=2200,snake=15,maxtile=0,cc=100
      // 用于自动调参：每次用一整套权重跑一批对局，比较分数。
      std::string spec;
      if (!take(&spec)) return false;
      if (!ParseWeightOverrides(spec, &options->weight_overrides)) {
        std::cerr << "无法解析 --weights: " << spec << "\n";
        return false;
      }
    } else if (arg == "--tag") {
      if (!take(&options->tag)) return false;
    } else if (arg == "--depth-b") {
      if (!take_int(&options->depth_b)) return false;
    } else if (arg == "--time-b") {
      if (!take_int(&options->time_budget_b)) return false;
    } else if (arg == "--weights-b") {
      std::string spec;
      if (!take(&spec)) return false;
      if (!ParseWeightOverrides(spec, &options->weight_overrides_b)) {
        std::cerr << "无法解析 --weights-b: " << spec << "\n";
        return false;
      }
      options->has_weight_b = true;
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
    } else if (key == "mergeval") {
      // 按结果牌值加权的合并潜力。原始量级比 merge 大得多
      // （一次 512 合并就是 1024），所以权重该比 merge 小几个数量级。
      weights->merge_value = value;
    } else if (key == "corner") {
      weights->corner = value;
    } else if (key == "snake") {
      weights->snake = value;
    } else if (key == "snakerank") {
      // 位置排名蛇形分（指数衰减）。默认 0 = 关闭，见 evaluate.h 的说明。
      weights->snake_rank = value;
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
    } else if (key == "mob") {
      // 可移动方向数（平方后乘此权重）。默认 0，见 evaluate.h。
      weights->mobility = value;
    } else if (key == "island") {
      // 孤立块惩罚。默认 0，且**应为负值**。
      weights->islands = value;
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
  if (options.max_depth > 0) {
    config.max_depth = options.max_depth;
    // 上限不能低于基础深度，否则 clamp 会把基础深度也一起压下去。
    config.max_depth = std::max(config.max_depth, config.base_depth);
  }
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
  if (!options.use_tt) return 0;
  if (options.tt_entries > 0) {
    return static_cast<std::size_t>(options.tt_entries);
  }
  return ai2048::TranspositionTable::kDefaultCapacity;
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
// train：用自我对弈训练 n-tuple 价值网络（TD 学习）
//
// 这是"换评估函数"路线的第一个阶段：先用最小的网络验证**能不能学**，
// 学得动再往上加 tuple 数量与长度。
//
// 判断"有没有在学"的唯一硬指标是**贪心评估分**（eval），不是训练期的随机走子分：
// 随机走子分只反映盘面分布，与网络好坏无关。
// ---------------------------------------------------------------------------
int RunTrain(const Options& options) {
  using ai2048::learn::TrainConfig;
  using ai2048::learn::TrainLog;
  using ai2048::learn::ValueNetwork;

  // 网络布局由 --net 选，默认用 C2 的混合 4-tuple。
  // C1 的纯按行布局表达能力不足（只能学到约 2,300 分），保留它只为对照。
  // serp 是 mixed + 真蛇形前缀（见 ValueNetwork::SerpentineTuples）。
  std::vector<ai2048::learn::Tuple> tuples;
  if (options.net_layout == "rows") {
    tuples = ValueNetwork::RowTuples();
  } else if (options.net_layout == "six1") {
    tuples = ValueNetwork::WithSixTuples(1);
  } else if (options.net_layout == "six2") {
    tuples = ValueNetwork::WithSixTuples(2);
  } else if (options.net_layout == "six4") {
    tuples = ValueNetwork::WithSixTuples(4);
  } else if (options.net_layout == "serp") {
    // mixed + 12 个真蛇形前缀。抓的是**换行处的相邻关系** ——
    // 那是横 tuple 和竖 tuple 都看不到的结构。见 SerpentineTuples 的说明。
    tuples = ValueNetwork::SerpentineTuples();
  } else {
    tuples = ValueNetwork::MixedTuples();
  }
  ValueNetwork network(std::move(tuples));

  TrainConfig config;
  config.games = options.train_games;
  config.learning_rate = options.train_lr;
  config.discount = options.train_discount;
  config.nstep = options.train_nstep;
  config.epsilon_decay_games = options.train_decay_games;
  config.seed = options.seed != 0 ? options.seed : 12345;
  config.difficulty = options.difficulty == ai2048::Difficulty::kEasy   ? 0
                      : options.difficulty == ai2048::Difficulty::kHard ? 2
                                                                        : 1;
  config.eval_every = options.train_eval_every;
  config.eval_games = options.train_eval_games;
  config.threads = options.train_threads;
  config.batch_games = options.train_batch;

  // 打印**实际**会用到的线程数，而不是命令行传的值：
  // threads=0 的含义是"用满硬件线程"，报告成 0 会让人以为没并行。
  const int active_threads =
      config.threads > 0 ? config.threads
                         : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

  std::cout << "TD 学习训练（n-tuple 价值网络）\n";
  std::cout << "  布局       : " << options.net_layout << "（" << network.tuple_count()
            << " 个 tuple）\n";
  std::cout << "  参数数量   : " << network.parameter_count() << "（约 "
            << static_cast<int>(network.parameter_megabytes() + 0.5) << " MB）\n";
  std::cout << "  局数       : " << config.games << "\n";
  std::cout << "  学习率     : " << config.learning_rate << "，折扣 " << config.discount << "\n";
  std::cout << "  TD 目标    : " << config.nstep << "-step 回报"
            << (config.nstep == 1 ? "（TD(0)）" : "") << "\n";
  std::cout << "  评估频率   : 每 " << config.eval_every << " 局（" << config.eval_games
            << " 局贪心）\n";
  std::cout << "  策略       : ε-greedy 自对弈，ε " << config.epsilon_start << " → "
            << config.epsilon_end;
  if (config.epsilon_decay_games > 0) {
    std::cout << "（指数衰减，" << config.epsilon_decay_games << " 局到低位）\n";
  } else {
    std::cout << "（按总局数线性衰减）\n";
  }
  std::cout << "  并行       : " << active_threads << " 线程，每批 " << config.batch_games
            << " 局（结果与单线程逐位一致）\n";
  std::cout << "\n";
  std::cout << "  局数    评估均分   评估最高   256    512   1024   训练均分\n";
  std::cout << "  ------  ---------  ---------  -----  -----  -----  ---------\n";
  std::cout.flush();

  const auto started = std::chrono::steady_clock::now();

  ai2048::learn::Train(&network, config, [&](const TrainLog& entry) {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("  %6d  %9.0f  %9d  %5d  %5d  %5d  %9.0f   (%.0fs)\n", entry.games_done,
                entry.eval.mean_score, entry.eval.best_score, entry.eval.reach_256,
                entry.eval.reach_512, entry.eval.reach_1024, entry.mean_train_score, seconds);
    std::cout.flush();
  });

  const double total_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "\n训练耗时 " << total_seconds << " s\n";

  if (!options.out_path.empty()) {
    std::string error;
    if (!network.Save(options.out_path, &error)) {
      std::cerr << "保存权重失败：" << error << "\n";
      return 1;
    }
    std::cout << "权重已保存到 " << options.out_path << "\n";
  }
  return 0;
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
  auto seeds = ReadSeeds(options.seeds_path, &error);
  if (!seeds.has_value()) {
    std::cerr << error << "\n";
    return 1;
  }
  // ⚠️ `--limit` 曾经在这里**被静默忽略** —— 它跑的是种子集里的全部局数。
  // 后果是一次"我想快速验 6 局"的调用实际跑了 200 局（每个种子跑两趟），
  // 从几分钟变成将近一小时，而且看起来像卡死。
  // 凡是接受 `--seeds` 的子命令都必须尊重 `--limit`（bench / compare 都遵守）。
  if (options.limit > 0 && static_cast<std::size_t>(options.limit) < seeds->size()) {
    seeds->resize(static_cast<std::size_t>(options.limit));
  }

  std::cout << "selfcheck: 种子集 " << options.seeds_path << "，共 " << seeds->size() << " 局"
            << "（每局跑两趟，合计 " << seeds->size() * 2 << " 局）"
            << "，深度 " << options.depth;
  if (options.time_budget_ms > 0) {
    std::cout << "，时间预算 " << options.time_budget_ms << "ms（会引入机器相关差异）";
  }
  std::cout << "\n";

  const SearchConfig config = MakeSearchConfig(options);
  const std::size_t table_capacity = MakeTableCapacity(options);
  const bool symmetry_keys = MakeUseSymmetryKeys(options);
  int failures = 0;
  std::size_t checked = 0;
  for (const std::uint64_t seed : *seeds) {
    const PlayOutcome first = PlayOneGame(seed, config, table_capacity, symmetry_keys);
    const PlayOutcome second = PlayOneGame(seed, config, table_capacity, symmetry_keys);
    ++checked;
    // 进度必须打出来：这个子命令每局跑两趟，全量种子集要接近一小时，
    // 没有输出的话会被误当成卡死（实际发生过）。
    if (checked % 10 == 0 || checked == seeds->size()) {
      std::cout << "\r  进度 " << checked << "/" << seeds->size() << std::flush;
    }
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
    std::cerr << "\nselfcheck 失败: " << failures << " 局不可复现\n";
    if (options.time_budget_ms > 0) {
      std::cerr << "提示: 使用了 --time，超时会按机器速度截断搜索深度。\n"
                << "      要验证确定性请去掉 --time。\n";
    }
    return 1;
  }
  std::cout << "\nselfcheck 通过: " << seeds->size() << " 局全部可复现（逐字节一致）\n";
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

  SearchConfig base_config = MakeSearchConfig(options);
  std::string net_error;
  auto loaded = ai2048::learn::LoadNetworkFromFile(options.net_file, &net_error);
  if (!loaded.has_value()) {
    std::cerr << "加载权重失败：" << net_error << "\n";
    return 1;
  }
  const std::shared_ptr<ai2048::learn::ValueNetwork> network = *loaded;
  ai2048::learn::AttachNetwork(network, &base_config);
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
// compare：A/B 两套配置的**配对**对拍
// ---------------------------------------------------------------------------
//
// 存在理由：用两次独立跑批的**均值**比较，100 局的标准误约 1500 分
// （局标准差约 15000），3~4% 的差异只有 0.7~1.1σ，测不出来。
// 配对比较让两套配置跑**同一批种子**，同一局里生成序列完全相同，
// 只有 AI 的决策不同，于是局间方差被消掉，只留下"同一局里 B 比 A 多拿多少"。
//
// 必须同时看两个量：
//   - 平均差值：效应有多大
//   - 差值的标准误：这个效应有多可信（|mean|/SE 就是配对 t 统计量）
// 只看前者就是"长时间小提升"的陷阱。
// 另外必须跑一次 **A 对 A 的空转**（--weights-b 与 --weights 相同）：
// 空转若报出显著差异，说明工具本身有问题，后面的结论一律不可信。
int RunCompare(const Options& options) {
  std::string error;
  auto seeds = ReadSeeds(options.seeds_path, &error);
  if (!seeds.has_value()) {
    std::cerr << error << "\n";
    return 1;
  }
  if (options.limit > 0 && static_cast<std::size_t>(options.limit) < seeds->size()) {
    seeds->resize(static_cast<std::size_t>(options.limit));
  }
  if (seeds->empty()) {
    std::cerr << "种子集为空\n";
    return 1;
  }

  // A 组 = 常规选项；B 组 = 在此基础上只改被显式指定的那一项。
  SearchConfig config_a = MakeSearchConfig(options);
  SearchConfig config_b = config_a;
  if (options.depth_b > 0) config_b.base_depth = options.depth_b;
  if (options.time_budget_b >= 0) config_b.time_budget_ms = options.time_budget_b;
  if (options.has_weight_b) config_b.weights = options.weight_overrides_b;

  std::string net_error;
  auto loaded = ai2048::learn::LoadNetworkFromFile(options.net_file, &net_error);
  if (!loaded.has_value()) {
    std::cerr << "加载权重失败：" << net_error << "\n";
    return 1;
  }
  const std::shared_ptr<ai2048::learn::ValueNetwork> network = *loaded;
  ai2048::learn::AttachNetwork(network, &config_a);
  ai2048::learn::AttachNetwork(network, &config_b);

  const std::size_t table_capacity = MakeTableCapacity(options);
  const bool symmetry_keys = MakeUseSymmetryKeys(options);
  const auto started = std::chrono::steady_clock::now();

  std::size_t a_wins = 0;
  std::size_t b_wins = 0;
  std::size_t ties = 0;
  std::size_t a_2048 = 0;
  std::size_t b_2048 = 0;
  double sum_delta = 0.0;     // Σ(B - A)
  double sum_delta_sq = 0.0;  // Σ(B - A)²
  double a_total = 0.0;
  double b_total = 0.0;

  for (std::size_t i = 0; i < seeds->size(); ++i) {
    const std::uint64_t seed = (*seeds)[i];
    const PlayOutcome a =
        PlayOneGame(seed, config_a, table_capacity, symmetry_keys, options.difficulty);
    const PlayOutcome b =
        PlayOneGame(seed, config_b, table_capacity, symmetry_keys, options.difficulty);
    const double delta = static_cast<double>(b.score) - static_cast<double>(a.score);
    sum_delta += delta;
    sum_delta_sq += delta * delta;
    a_total += static_cast<double>(a.score);
    b_total += static_cast<double>(b.score);
    if (delta > 0.0) {
      ++b_wins;
    } else if (delta < 0.0) {
      ++a_wins;
    } else {
      ++ties;
    }
    if (a.max_exponent >= 11) ++a_2048;
    if (b.max_exponent >= 11) ++b_2048;

    const std::size_t done = i + 1;
    if (done % 10 == 0 || done == seeds->size()) {
      std::cout << "\r  配对进度 " << done << "/" << seeds->size() << std::flush;
    }
  }

  const double wall_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::cout << "\n";

  const double games = static_cast<double>(seeds->size());
  const double mean_delta = sum_delta / games;
  const double mean_a = a_total / games;
  const double mean_b = b_total / games;
  // 样本方差（无偏）。差值为常数时方差为 0，SE 也为 0 —— 这时任何非零差值
  // 都"无限显著"，但那是退化情形，下面单独处理，避免除以 0。
  const double variance =
      games > 1.0 ? (sum_delta_sq - games * mean_delta * mean_delta) / (games - 1.0) : 0.0;
  const double std_err = std::sqrt(std::max(0.0, variance) / games);
  const double t_stat = std_err > 0.0 ? mean_delta / std_err : 0.0;

  // 符号检验（只看胜负，不看幅度）。大样本下用正态近似。
  // 忽略平局：平局不提供方向信息。
  const double decisive = static_cast<double>(a_wins + b_wins);
  double sign_p = 1.0;
  if (decisive > 0.0) {
    const double z =
        std::abs(static_cast<double>(b_wins) - decisive / 2.0) / std::sqrt(decisive * 0.25);
    sign_p = std::erfc(z / std::sqrt(2.0));
  }

  // 要在同一显著水平下分辨出 5% 的效应，还需要多少局？
  // n ≈ 16 × (σ_delta / μ_A)² 是"5% 效应、α=0.05、power=0.8"的两样本估计。
  const double rel_se = mean_a > 0.0 ? std_err / mean_a : 0.0;
  const double need_for_5pct = rel_se > 0.0 ? 16.0 / (rel_se * rel_se) : 0.0;

  std::cout << "规则集版本 : " << ai2048::RulesetVersion() << "\n";
  std::cout << "难度       : " << ai2048::DifficultyName(options.difficulty) << "\n";
  std::cout << "种子集     : " << options.seeds_path << "（前 " << seeds->size() << " 局）\n";
  std::cout << "A 组       : 深度 " << config_a.base_depth;
  if (config_a.time_budget_ms > 0) std::cout << "，预算 " << config_a.time_budget_ms << "ms";
  std::cout << "\n";
  std::cout << "B 组       : 深度 " << config_b.base_depth;
  if (config_b.time_budget_ms > 0) std::cout << "，预算 " << config_b.time_budget_ms << "ms";
  std::cout << "\n\n";
  std::cout << "A 平均分   : " << static_cast<std::uint64_t>(std::llround(mean_a)) << "\n";
  std::cout << "B 平均分   : " << static_cast<std::uint64_t>(std::llround(mean_b)) << "\n";
  std::cout << "平均差值   : " << (mean_delta >= 0 ? "+" : "")
            << static_cast<std::int64_t>(std::llround(mean_delta)) << "  (B - A)\n";
  std::cout << "相对变化   : " << (mean_a > 0.0 ? 100.0 * mean_delta / mean_a : 0.0) << " %\n";
  std::cout << "差值标准差 : " << std::sqrt(std::max(0.0, variance)) << "\n";
  std::cout << "差值标准误 : " << std_err << "\n";
  std::cout << "配对 t     : " << t_stat << "   （|t| > 2 约等于 95% 置信）\n";
  std::cout << "胜负局     : B 胜 " << b_wins << " / A 胜 " << a_wins << " / 平 " << ties
            << "，符号检验 p = " << sign_p << "\n";
  std::cout << "2048 到达  : A " << a_2048 << "/" << seeds->size() << "，B " << b_2048 << "/"
            << seeds->size() << "\n";

  std::cout << "\n结论       : ";
  if (variance == 0.0 && mean_delta == 0.0) {
    std::cout << "两组**逐局完全相同** —— 这次改动没有影响决策（可用作管线自检）。\n";
  } else if (std::abs(t_stat) < 2.0) {
    std::cout << "**分辨不出差异**（|t| < 2）。不能说 B 更好，也不能说更差。\n";
    if (need_for_5pct > games) {
      std::cout << "           当前精度只能分辨约 " << (100.0 * 2.0 * std_err / mean_a)
                << "% 以上的差异；要分辨 5% 需要约 "
                << static_cast<std::uint64_t>(std::ceil(need_for_5pct)) << " 局。\n";
    }
  } else if (t_stat > 0.0) {
    std::cout << "B 显著更好（t = " << t_stat << "）。\n";
  } else {
    std::cout << "A 显著更好（t = " << t_stat << "）。\n";
  }
  std::cout << "总耗时     : " << wall_seconds << " s（单线程，A+B 各 " << seeds->size()
            << " 局）\n";
  return 0;
}

// ---------------------------------------------------------------------------
// play：单局演示
// ---------------------------------------------------------------------------

int RunPlay(const Options& options) {
  SearchConfig config = MakeSearchConfig(options);
  std::string net_error;
  auto loaded = ai2048::learn::LoadNetworkFromFile(options.net_file, &net_error);
  if (!loaded.has_value()) {
    std::cerr << "加载权重失败：" << net_error << "\n";
    return 1;
  }
  const std::shared_ptr<ai2048::learn::ValueNetwork> network = *loaded;
  ai2048::learn::AttachNetwork(network, &config);
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
      << "                    单位是**树层数**：1 步前瞻 = 2 层。公开基准说的 depth 8\n"
      << "                    是 8 步 = 本项目的 16 层，别把两者的数字直接比。\n"
      << "  --max-depth N     自适应深度的上限（默认 = 基础深度，即加成被夹掉）\n"
      << "  --time N          每步时间预算（毫秒）。0 = 不限时（完全确定，可复现）\n"
      << "  --chance-limit N  chance 节点采样上限。0 = 枚举全部空格\n"
      << "  --net-file F      用训练好的 n-tuple 权重做叶子评估（替代手写启发式）\n"
      << "  --no-tt           关闭置换表\n"
      << "  --symmetry        置换表用 8 重对称键（有正确性代价，见 search.h）\n"
      << "子命令:\n"
      << "  version\n"
      << "  play      [--seed N] [--every N]              让 AI 跑一局并打印\n"
      << "  move      --board <16 个指数> --dir <方向>    对给定棋盘走一步，输出结果\n"
      << "  move      --stdin                            批量模式，每行 \"<16 个指数> <方向>\"\n"
      << "  trace     --seeds <文件> [--limit N]          每局输出一行状态，供前端规则对拍\n"
      << "  selfcheck --seeds <文件>                      同种子重跑两次，校验逐字节一致\n"
      << "  bench     --seeds <文件> [--limit N] [--threads N] [--tag T]\n"
      << "  compare   --seeds <文件> [--limit N] [--weights-b S] [--depth-b N] [--time-b N]\n"
      << "            配对对拍：A/B 跑同一批种子，输出逐局差值的均值与标准误。\n"
      << "            用均值比较分不出 3~4% 的差异（100 局 SE≈1500 分），配对可以。\n"
      << "            先跑一次 A 对 A（--weights-b 与 --weights 相同）做空转自检：\n"
      << "            空转若报显著，工具本身有问题，后续结论一律不可信。\n"
      << "  train     [--net L] [--games N] [--lr F] [--nstep N] [--discount F]\n"
      << "            [--eval-every N] [--eval-games N] [--threads N] [--batch N] [--out 文件]\n"
      << "            TD 学习训练 n-tuple 价值网络。L 取 rows/mixed/six1/six2/six4\n"
      << "            --nstep 是 n-step 回报步数（1 = TD(0)，3~5 通常更好）\n"
      << "            --threads 默认 1；本机多线程反而更慢（见 main.cpp 的实测表）\n";
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
  if (command == "compare") return RunCompare(options);
  if (command == "train") return RunTrain(options);

  std::cerr << "未知子命令: " << command << "\n\n";
  PrintUsage();
  return 1;
}
