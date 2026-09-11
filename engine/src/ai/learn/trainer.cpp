#include "ai/learn/trainer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "core/game.h"
#include "core/rng.h"

namespace ai2048::learn {

namespace {

constexpr std::array<Direction, 4> kDirections = {Direction::kUp, Direction::kDown,
                                                  Direction::kLeft, Direction::kRight};

/** 把 core::Difficulty 的数字转成枚举。trainer.h 用 int 是为了不暴露 core 头。 */
[[nodiscard]] Difficulty ToDifficulty(int value) noexcept {
  switch (value) {
    case 0:
      return Difficulty::kEasy;
    case 2:
      return Difficulty::kHard;
    case 1:
    default:
      return Difficulty::kNormal;
  }
}

/** 走子结果里"合并得到的分数"，转成归一化后的奖励。 */
[[nodiscard]] double Reward(const MoveResult& move) noexcept {
  return static_cast<double>(move.score_gained) / ValueNetwork::kScoreScale;
}

/**
 * 把 [0, count) 切成 threads 份，每份交给一个线程。
 *
 * 切法是**按下标连续分段**，不是抢任务队列 —— 每段的结果只取决于下标范围，
 * 与哪个线程先启动无关，所以并行结果与串行逐位一致（见 TrainConfig::threads）。
 */
template <typename Worker>
void RunParallel(int count, int threads, const Worker& worker) {
  if (count <= 0) return;
  const int usable = std::max(1, std::min(threads, count));
  if (usable == 1) {
    for (int i = 0; i < count; ++i) worker(i);
    return;
  }

  std::vector<std::thread> pool;
  pool.reserve(static_cast<std::size_t>(usable));
  for (int t = 0; t < usable; ++t) {
    const int begin = count * t / usable;
    const int end = count * (t + 1) / usable;
    pool.emplace_back([&worker, begin, end] {
      for (int i = begin; i < end; ++i) worker(i);
    });
  }
  for (std::thread& thread : pool) thread.join();
}

/** threads <= 0 时用满硬件线程。 */
[[nodiscard]] int ResolveThreads(int requested) noexcept {
  if (requested > 0) return requested;
  const unsigned int hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : static_cast<int>(hardware);
}

/** 一局自对弈收集到的轨迹：走过的方向 + 每步拿到的奖励。 */
struct GameTrajectory {
  std::uint64_t seed = 0;
  std::vector<Direction> directions;
  /**
   * 每步的奖励，总长比 directions 多一个前置的 0。
   *
   * 之所以把奖励和方向分开存、而不是存一步的二元组，是为了在重放时
   * 严格复刻串行版的顺序（"先算 target 再更新 previous"）。
   * 顺序写错会让 TD 目标的奖励错位一步，分数会莫名其妙地停在低位。
   */
  std::vector<double> rewards;
  int score = 0;
};

/**
 * 收集一局的轨迹（只读网络，可并行）。
 *
 * 这里的走子选择逻辑与串行版**逐字对应**，改动必须两边同步 ——
 * 它决定了样本从哪个盘面分布里来，等价于改了训练目标。
 *
 * ## 为什么探索用**自己的** Rng
 *
 * 探索决策原本用的是 game 内部的随机流。但 Game 不暴露 rng()，
 * 而给 core/rng.h 或 Game 加访问器会动到"同种子逐字节一致"的契约
 * （selfcheck 与 replay 都挂在那上面）—— 训练没资格改那个契约。
 *
 * 所以这里另起一个 Rng，种子由局号派生。它只影响"训练时走哪一步"，
 * 不影响任何一局的棋盘演化：走子序列是**记录**下来的，重放时原样执行。
 * 换个种子只会换一批样本，不会让同一份轨迹重放出不一样的结果。
 */
void CollectGame(const ValueNetwork& network, const TrainConfig& config, Difficulty difficulty,
                 std::uint64_t seed, double epsilon, GameTrajectory* out) {
  Game game(seed, difficulty);
  // 与 game 的随机流刻意分开：加一个黄金比常数，避免两条流相关性。
  Rng policy_rng(seed ^ 0x9E37'79B9'7F4A'7C15ULL);

  out->seed = seed;
  out->directions.clear();
  out->rewards.assign(1, 0.0);  // 第 0 步没有"上一步的奖励"
  out->score = 0;

  for (int step = 0; step < config.max_steps_per_game && !game.game_over(); ++step) {
    // ε-greedy：以 ε 随机走，否则按当前网络挑"走完之后"价值最高的方向。
    //
    // 网络自己的策略是必须的：纯随机走子的盘面分布太窄（100 步就死），
    // 网络学不到"好盘面值多少"，贪心评估也就永远停在 1,000 分。
    std::array<Direction, 4> legal{};
    int legal_count = 0;
    for (const Direction direction : kDirections) {
      if (ApplyMove(game.board(), direction).moved) {
        legal[static_cast<std::size_t>(legal_count)] = direction;
        ++legal_count;
      }
    }
    if (legal_count == 0) break;

    Direction chosen = legal[0];
    if (policy_rng.Chance(static_cast<std::uint64_t>(epsilon * 10000.0), 10000)) {
      chosen = legal[static_cast<std::size_t>(
          policy_rng.NextBounded(static_cast<std::uint64_t>(legal_count)))];
    } else {
      double best_value = -std::numeric_limits<double>::infinity();
      for (int i = 0; i < legal_count; ++i) {
        const MoveResult candidate = ApplyMove(game.board(), legal[static_cast<std::size_t>(i)]);
        const double value = network.Evaluate(candidate.board);
        if (value > best_value) {
          best_value = value;
          chosen = legal[static_cast<std::size_t>(i)];
        }
      }
    }

    const MoveResult move = ApplyMove(game.board(), chosen);
    out->directions.push_back(chosen);
    out->rewards.push_back(Reward(move));
    static_cast<void>(game.Step(chosen));
  }

  out->score = static_cast<int>(game.score());
}

/**
 * 重放一局，把 TD 更新写进网络。
 *
 *
 * 重放而不是"收集时顺手更新"，是因为更新必须发生在**一批全部收集完之后**：
 *
 * 收集阶段多个线程同时在跑，谁都不能改权重。重放一遍的代价是每步多一次
 *
 * 评估，但换来的是没有锁、没有竞态、结果与串行版一致。
 *
 * ## n-step 的环形缓冲
 *
 * 位置 p 的
 * n-step 目标是
 *
 *     target(p) = r_p + γ·r_{p+1} + … + γⁿ⁻¹·r_{p+n-1} + γⁿ·V_{p+n}
 *
 * 其中
 * r_p 是**走完第 p 步**得到的奖励（奖励滞后于状态一步）。
 * 要算它必须同时拿到两样东西：窗口内 n
 * 个奖励，以及窗口末端之后那一步的
 * 价值 V_{p+n}。两者都在第 p+n
 * 步执行完之后才齐备，所以缓冲要留
 * **n+1 个槽位**：最后一个槽位专供"用于自举的那一步"。
 *
 * ##
 * 这里踩过的坑（必须记住）
 *
 * 第一版只留了 n 个槽位，于是自举项拿到了 V_p 自己而不是 V_{p+n} ——

 * * 等价于把 `r + γ·V_{t+1}` 错写成 `r + γ·V_t`，**差一步**。
 * 它不崩、不报错，只是让 TD
 * 目标系统性偏移：100k 局只有 891 分，
 *
 * 而正确实现同规模能到几千分。凡是"训练分数莫名其妙趴着不动"，
 * 先回来检查自举的是哪一步的价值。

 * *
 * ## 两种更新
 *
 * - 主循环：位置 p 之前，先更新 p-n（它已经等够了 n+1 步）
 * -
 * 局终：剩下的位置 p ∈ [tail_start, total-1] 的窗口伸出了轨迹之外，
 *
 * 此时"之后再也拿不到分"，自举项为 0，只累加剩下的真实奖励
 *
 * 两段共用同一个 update_at，靠参数 n
 * 与 last_reward 区分，
 * 避免写出两套各自带差一错误的循环。
 */
// 形参刻意是**非 const 指针**：这是全流程里唯一允许写权重的地方。
// 收集阶段拿到的是 const 引用，编译器替我们守住"收集时不许改网络"这条线。
void ApplyGame(ValueNetwork* network, const GameTrajectory& trajectory, double learning_rate,
               int difficulty_value, int nstep, double discount) {
  Game game(trajectory.seed, ToDifficulty(difficulty_value));

  const std::size_t horizon = static_cast<std::size_t>(std::max(1, nstep));
  const std::size_t slots = horizon + 1;  // 末尾多一格给"用于自举的那一步"
  std::vector<ValueNetwork::Trace> value_at(slots);
  std::vector<double> reward_at(slots, 0.0);

  const std::size_t total = trajectory.directions.size();
  const std::size_t tail_start = total >= horizon ? total - horizon : 0;

  /**
   * 更新位置 p。
   * @param n          回报的步数（局终的尾巴用剩余步数）
   * @param
   * last_reward 窗口内最后一个奖励的下标（是奖励下标，不是位置）
   * @param bootstrap
   * 是否加自举项（局终的尾巴不加）
   */
  auto update_at = [&](std::size_t p, std::size_t n, std::size_t last_reward, bool bootstrap) {
    const std::size_t slot = p % slots;
    double target = 0.0;
    double gamma = 1.0;
    const std::size_t first_reward = p + 1;  // 奖励滞后状态一步
    for (std::size_t r = first_reward; r <= last_reward; ++r) {
      target += gamma * reward_at[r % slots];
      gamma *= discount;
    }
    if (bootstrap) {
      target += gamma * value_at[(p + n) % slots].value;
    }
    const ValueNetwork::Trace& trace = value_at[slot];
    network->ApplyGradient(trace, target - trace.value, learning_rate);
  };

  for (std::size_t i = 0; i < total; ++i) {
    const Direction chosen = trajectory.directions[i];
    const MoveResult move = ApplyMove(game.board(), chosen);
    static_cast<void>(game.Step(chosen));

    value_at[i % slots] = network->EvaluateWithTrace(move.board, game.game_over());
    reward_at[(i + 1) % slots] = trajectory.rewards[i + 1];

    // 第 i 步做完，位置 i-n 的窗口（含自举用的 V_i）才齐备。
    if (i >= horizon) {
      update_at(i - horizon, horizon, i, /*bootstrap=*/true);
    }
  }

  // 局终尾巴：窗口伸出轨迹之外，自举项为 0。
  for (std::size_t p = tail_start; p < total; ++p) {
    const std::size_t n = total - p;
    update_at(p, n, total, /*bootstrap=*/false);
  }
}

}  // namespace

EvalResult EvaluateGreedy(const ValueNetwork& network, int games, std::uint64_t seed,
                          int difficulty, int max_steps, int threads) {
  if (games <= 0) return EvalResult{};

  const Difficulty diff = ToDifficulty(difficulty);
  std::vector<int> scores(static_cast<std::size_t>(games), 0);
  std::vector<std::uint64_t> max_tiles(static_cast<std::size_t>(games), 0);

  // 局 i 的种子固定为 seed + i*7919，与串行版一致；
  // 切分只影响谁算哪一段，不影响任何一局的内容。
  RunParallel(games, ResolveThreads(threads), [&](int i) {
    Game game(seed + static_cast<std::uint64_t>(i) * 7919, diff);

    for (int step = 0; step < max_steps && !game.game_over(); ++step) {
      // 贪心：挑"走完之后"价值最高的方向。
      // 这里评估的是 afterstate（走完、还没生成新方块），与训练目标一致 ——
      // 用完整盘面会引入"新方块落在哪"的随机性，与训练时的目标不一致。
      Direction best_direction = Direction::kUp;
      double best_value = -std::numeric_limits<double>::infinity();
      bool found = false;

      for (const Direction direction : kDirections) {
        const MoveResult move = ApplyMove(game.board(), direction);
        if (!move.moved) continue;
        const double value = network.Evaluate(move.board);
        if (!found || value > best_value) {
          best_value = value;
          best_direction = direction;
          found = true;
        }
      }
      if (!found) break;
      static_cast<void>(game.Step(best_direction));
    }

    scores[static_cast<std::size_t>(i)] = static_cast<int>(game.score());
    max_tiles[static_cast<std::size_t>(i)] = game.max_tile();
  });

  EvalResult result;
  long long total = 0;
  for (int i = 0; i < games; ++i) {
    const int score = scores[static_cast<std::size_t>(i)];
    total += score;
    result.best_score = std::max(result.best_score, score);
    const std::uint64_t max_tile = max_tiles[static_cast<std::size_t>(i)];
    if (max_tile >= 256) ++result.reach_256;
    if (max_tile >= 512) ++result.reach_512;
    if (max_tile >= 1024) ++result.reach_1024;
  }
  result.mean_score = static_cast<double>(total) / static_cast<double>(games);
  return result;
}

void Train(ValueNetwork* network, const TrainConfig& config,
           const std::function<void(const TrainLog&)>& log) {
  const Difficulty difficulty = ToDifficulty(config.difficulty);
  const int threads = ResolveThreads(config.threads);
  const int batch_games = std::max(1, config.batch_games);

  long long train_score_total = 0;
  int train_games = 0;

  const auto started = std::chrono::steady_clock::now();

  std::vector<GameTrajectory> batch;
  batch.reserve(static_cast<std::size_t>(batch_games));

  for (int batch_begin = 0; batch_begin < config.games; batch_begin += batch_games) {
    const int batch_end = std::min(batch_begin + batch_games, config.games);
    const int batch_size = batch_end - batch_begin;

    // 一批之内的 ε 用批首的进度算，批内所有局共享同一个 ε。
    // 逐局算 ε 会让 ε 依赖局号，不利于并行切分；
    // 批粒度上的台阶（512 局）远小于 ε 的衰减速率，对结果无实质影响。
    //
    // 两种衰减方式：
    //   指数（默认）：在 epsilon_decay_games 局时降到 epsilon_end，之后保持。
    //     与总局数解耦，所以不同规模的跑批可以直接横向对比。
    //   线性（epsilon_decay_games == 0）：旧行为，按 config.games 线性插值。
    const double epsilon = [&] {
      const double start = config.epsilon_start;
      const double end = config.epsilon_end;
      const double decay = static_cast<double>(config.epsilon_decay_games);
      if (decay <= 0.0) {  // 旧行为：按总局数线性衰减
        const double progress =
            static_cast<double>(batch_begin) / static_cast<double>(config.games);
        return start + (end - start) * progress;
      }
      if (batch_begin >= config.epsilon_decay_games) return end;
      const double ratio = static_cast<double>(batch_begin) / decay;
      return start * std::pow(end / start, ratio);
    }();

    batch.assign(static_cast<std::size_t>(batch_size), GameTrajectory{});

    // 阶段一：并行收集。所有线程只读 network，可以放心并发。
    RunParallel(batch_size, threads, [&](int i) {
      // 种子必须由局号唯一决定，不能取 rng.NextU64() ——
      // 那样种子的消费顺序就成了共享状态，线程数一变结果就变。
      const std::uint64_t seed_of_game = config.seed + static_cast<std::uint64_t>(batch_begin + i);
      CollectGame(*network, config, difficulty, seed_of_game, epsilon,
                  &batch[static_cast<std::size_t>(i)]);
    });

    // 阶段二：按局号顺序串行写梯度。这一段是单线程的，但不占大头：
    // 收集时每步要做 ~4 次评估，重放时每步只要 2 次。
    for (int i = 0; i < batch_size; ++i) {
      const GameTrajectory& trajectory = batch[static_cast<std::size_t>(i)];
      ApplyGame(network, trajectory, config.learning_rate, config.difficulty, config.nstep,
                config.discount);
      train_score_total += trajectory.score;
      ++train_games;
    }

    const int done = batch_end;
    const bool is_last = done >= config.games;
    if (config.eval_every > 0 && (done % config.eval_every < batch_size || is_last)) {
      TrainLog entry;
      entry.games_done = done;
      entry.mean_train_score =
          static_cast<double>(train_score_total) / static_cast<double>(train_games);
      entry.eval = EvaluateGreedy(*network, config.eval_games, config.seed + 999983,
                                  config.difficulty, config.max_steps_per_game, threads);

      const double now =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      if (log) log(entry);
      (void)now;
    }
  }
}

}  // namespace ai2048::learn