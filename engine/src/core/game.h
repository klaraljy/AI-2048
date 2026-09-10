// 一局 2048 的状态机。
//
// 职责边界：
//   - board.cpp 只管"一次走子"（纯函数，无随机）
//   - 这里管"生成新方块、计分、终局判定、可复现的随机"
//
// 确定性承诺：同一个种子 + 同一串操作序列 -> 完全一致的棋盘、分数、
// 步数与随机数消耗。统计与 replay 都建立在这上面。

#ifndef AI2048_CORE_GAME_H_
#define AI2048_CORE_GAME_H_

#include <cstdint>
#include <string>
#include <vector>

#include "core/board.h"
#include "core/rng.h"

namespace ai2048 {

// 新方块是 4 的概率，其余出 2。标准 2048 的取值。
//
// 注意：这是**规则**，不是难度。参考原型把方块生成规则拿去做难度调节
// （简单难度 70% 把新块塞进四角），导致 AI 的分数与公开基准不可比，
// 也让人玩的规则和 AI 跑的规则不是同一个游戏。这里不做那件事 --
// 难度只能改非规则的东西（撤销次数、提示、让子步数）。
inline constexpr std::uint64_t kFourSpawnNumerator = 1;
inline constexpr std::uint64_t kSpawnDenominator = 10;

// 开局放几个方块。
inline constexpr int kInitialTiles = 2;

struct SpawnRecord {
  int row = 0;
  int col = 0;
  int exponent = 0;
};

struct StepResult {
  bool moved = false;              // false = 这个方向不合法，状态完全没有变化
  bool spawned = false;            // 是否生成了新方块
  SpawnRecord spawn;               // 仅当 spawned 为真时有效
  std::uint64_t score_gained = 0;  // 本次走子合并得到的分数
  std::vector<TileMove> moves;     // 方块轨迹，供前端动画
  bool overflow = false;           // 见 kMaxExponent
};

class Game {
 public:
  explicit Game(std::uint64_t seed) noexcept;

  // 走一步。方向不合法时返回 moved == false，且**不消耗任何随机数** ——
  // 否则"玩家按了几下无效方向键"就会改变后续结果，replay 立刻失效。
  [[nodiscard]] StepResult Step(Direction direction) noexcept;

  [[nodiscard]] std::uint64_t board() const noexcept { return board_; }
  [[nodiscard]] std::uint64_t score() const noexcept { return score_; }
  [[nodiscard]] std::uint32_t step_count() const noexcept { return step_count_; }
  [[nodiscard]] bool game_over() const noexcept { return game_over_; }
  [[nodiscard]] bool reached_2048() const noexcept { return reached_2048_; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
  [[nodiscard]] int max_exponent() const noexcept { return MaxExponent(board_); }
  [[nodiscard]] std::uint64_t max_tile() const noexcept { return ExponentToValue(max_exponent()); }

  // 本次走子是否触发过"上限处合并"。累计值，便于跑批统计有多少局碰到过这个边界。
  [[nodiscard]] bool saw_overflow() const noexcept { return saw_overflow_; }

  // 规范化的状态字符串：参与确定性的逐字节比较，不要随意改动字段或格式。
  [[nodiscard]] std::string Serialize() const noexcept;

 private:
  void SpawnRandomTile() noexcept;

  std::uint64_t seed_ = 0;
  Rng rng_;
  std::uint64_t board_ = 0;
  std::uint64_t score_ = 0;
  std::uint32_t step_count_ = 0;
  bool game_over_ = false;
  bool reached_2048_ = false;
  bool saw_overflow_ = false;
};

}  // namespace ai2048

#endif  // AI2048_CORE_GAME_H_
