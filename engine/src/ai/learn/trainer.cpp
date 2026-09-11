#include "ai/learn/trainer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

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

}  // namespace

EvalResult EvaluateGreedy(const ValueNetwork& network, int games, std::uint64_t seed,
                          int difficulty, int max_steps) {
  EvalResult result;
  const Difficulty diff = ToDifficulty(difficulty);
  long long total = 0;

  for (int i = 0; i < games; ++i) {
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

    const int score = static_cast<int>(game.score());
    total += score;
    result.best_score = std::max(result.best_score, score);
    const std::uint64_t max_tile = game.max_tile();
    if (max_tile >= 256) ++result.reach_256;
    if (max_tile >= 512) ++result.reach_512;
    if (max_tile >= 1024) ++result.reach_1024;
  }

  result.mean_score = games > 0 ? static_cast<double>(total) / static_cast<double>(games) : 0.0;
  return result;
}

void Train(ValueNetwork* network, const TrainConfig& config,
           const std::function<void(const TrainLog&)>& log) {
  Rng rng(config.seed);
  const Difficulty difficulty = ToDifficulty(config.difficulty);

  long long train_score_total = 0;
  int train_games = 0;

  const auto started = std::chrono::steady_clock::now();
  double last_eval_seconds = 0.0;

  for (int game_index = 0; game_index < config.games; ++game_index) {
    Game game(rng.NextU64() % 1000000007ULL, difficulty);

    // 上一对 (afterstate 的 trace)。TD(0) 只需要前一步的记录。
    bool has_previous = false;
    ValueNetwork::Trace previous;

    for (int step = 0; step < config.max_steps_per_game && !game.game_over(); ++step) {
      // ε-greedy：以 ε 随机走，否则按当前网络挑"走完之后"价值最高的方向。
      //
      // 网络自己的策略是必须的：纯随机走子的盘面分布太窄（100 步就死），
      // 网络学不到"好盘面值多少"，贪心评估也就永远停在 1,000 分。
      const double progress =
          config.games > 0 ? static_cast<double>(game_index) / static_cast<double>(config.games)
                           : 1.0;
      const double epsilon =
          config.epsilon_start + (config.epsilon_end - config.epsilon_start) * progress;

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
      // 用整数比较实现 ε-greedy，而不是给 Rng 加一个 NextDouble ——
      // core/rng.h 有"同种子逐字节一致"的承诺，新增接口会动到那份契约。
      // 训练不参与可复现性承诺，用 NextBounded 就够。
      const bool explore = rng.NextBounded(10000) < static_cast<std::uint64_t>(epsilon * 10000.0);
      if (explore) {
        chosen = legal[static_cast<std::size_t>(
            rng.NextBounded(static_cast<std::uint64_t>(legal_count)))];
      } else {
        double best_value = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < legal_count; ++i) {
          const MoveResult candidate = ApplyMove(game.board(), legal[static_cast<std::size_t>(i)]);
          const double value = network->Evaluate(candidate.board);
          if (value > best_value) {
            best_value = value;
            chosen = legal[static_cast<std::size_t>(i)];
          }
        }
      }

      const MoveResult move = ApplyMove(game.board(), chosen);
      const std::uint64_t afterstate = move.board;
      const double reward = Reward(move);

      const ValueNetwork::Trace current = network->EvaluateWithTrace(afterstate);

      if (has_previous) {
        // δ = (r + γ·V(afterstate_t+1)) − V(afterstate_t)
        const double target = reward + config.discount * current.value;
        const double delta = target - previous.value;
        network->ApplyGradient(previous, delta, config.learning_rate);
      }

      previous = current;
      has_previous = true;

      static_cast<void>(game.Step(chosen));
    }

    // 一局结束：最后一步的 afterstate 之后没有后续奖励了，
    // 用它把价值往"这一局就此结束"的方向收一下。
    if (has_previous) {
      network->ApplyGradient(previous, -previous.value, config.learning_rate * 0.5);
    }

    train_score_total += static_cast<long long>(game.score());
    ++train_games;

    const bool is_last = (game_index + 1) == config.games;
    if (config.eval_every > 0 && ((game_index + 1) % config.eval_every == 0 || is_last)) {
      TrainLog entry;
      entry.games_done = game_index + 1;
      entry.mean_train_score =
          static_cast<double>(train_score_total) / static_cast<double>(train_games);
      entry.eval = EvaluateGreedy(*network, config.eval_games, config.seed + 999983,
                                  config.difficulty, config.max_steps_per_game);

      const double now =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      if (log) log(entry);

      // 训练循环本身要能看出"时间花在哪" —— 评估占的比例过大时
      // 说明 eval_games 该调小，否则训练进度会被评估拖住。
      last_eval_seconds = now;
      (void)last_eval_seconds;
    }
  }
}

}  // namespace ai2048::learn
