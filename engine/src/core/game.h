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
// 注意：这是**规则**，不随难度变化。难度只改**位置**（见 Difficulty），
// 不改取值概率 —— 参考原型的 nearMaxSpawnProbability 会连取值一起改，
// 那会让同一档难度里"游戏的随机性"也变，没法归因。
inline constexpr std::uint64_t kFourSpawnNumerator = 1;
inline constexpr std::uint64_t kSpawnDenominator = 10;

// 开局放几个方块。
inline constexpr int kInitialTiles = 2;

// ---------------------------------------------------------------------------
// 难度：改变**新方块出现的位置分布**
//
// 这是一次**规则的主动扩展**，不是实现细节。原设计曾明确禁止难度影响规则，
// 理由是"分数会与公开基准不可比、人和 AI 玩的不是同一个游戏"。那个理由依然
// 成立，所以配套做了三件事来兜住它：
//   1. 默认是 kNormal（= 标准 2048，全盘均匀），历史分数与基准仍然可比；
//   2. 分数**不跨难度比较**，跑批与界面都必须带上难度标记；
//   3. tests/spawn.test.mjs 只对 kNormal 断言"全盘均匀"，
//      对另外两档断言"确实偏离"，避免把偏置当成 bug 又改回去。
//
// 与参考实现的差别（重要）：它只有"优先塞角落"，而角落**对玩家有利**，
// 所以它的每一档都比标准 2048 更容易（实测落角率 77.5% / 36.25% / 28.75%，
// 连最难的一档都高于标准的 25%）。它那个 nearMaxSpawnProbability 名字看着像
// "往最大块附近生成"，其实**只改取值不改位置**，是死代码。
// 本实现按明确规格来做，其中 kHard 是真正的"往最大块附近堆"——
// 那会**削弱**玩家（大块周围被小方块堵住），是货真价实的难度。
// ---------------------------------------------------------------------------
enum class Difficulty : std::uint8_t {
  kEasy = 0,    // 70% 概率生成在空角落
  kNormal = 1,  // 全盘均匀（标准 2048）
  kHard = 2,    // 80% 概率生成在最大方块的相邻空格
};

// 各档的偏置概率（分子/分母）。
inline constexpr std::uint64_t kEasyCornerNumerator = 7;
inline constexpr std::uint64_t kHardNearMaxNumerator = 8;
inline constexpr std::uint64_t kDifficultyDenominator = 10;

[[nodiscard]] const char* DifficultyName(Difficulty difficulty) noexcept;

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
  // 默认 kNormal：不传难度的调用方（跑批、自检、历史基准）拿到的是标准 2048，
  // 保证既有结果与公开基准继续可比。
  explicit Game(std::uint64_t seed, Difficulty difficulty = Difficulty::kNormal) noexcept;

  // 走一步。方向不合法时返回 moved == false，且**不消耗任何随机数** ——
  // 否则"玩家按了几下无效方向键"就会改变后续结果，replay 立刻失效。
  [[nodiscard]] StepResult Step(Direction direction) noexcept;

  [[nodiscard]] Difficulty difficulty() const noexcept { return difficulty_; }
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

  // 生成一个新方块并返回落点。棋盘已满时返回的记录 exponent == 0。
  //
  // 公开是为了**可测试**：难度偏置（70% 落角 / 80% 贴最大块）只有能对
  // 任意盘面反复采样才验得准。靠"跑完整局再统计"会混入对局演化带来的
  // 分布偏移，根本分不清是偏置生效还是棋盘的形状使然 —— 这一点实测踩过。
  [[nodiscard]] SpawnRecord SpawnRandomTile() noexcept;

  // 直接摆一个盘面。**仅供测试与工具**（生成分布验证、复现某个局面）。
  // 会清零分数与步数，但保留种子与随机流。
  void SetBoardForTesting(std::uint64_t board) noexcept;

  // 规范化的状态字符串：参与确定性的逐字节比较，不要随意改动字段或格式。
  [[nodiscard]] std::string Serialize() const noexcept;

 private:
  std::uint64_t seed_ = 0;
  Rng rng_;
  Difficulty difficulty_ = Difficulty::kNormal;
  std::uint64_t board_ = 0;
  std::uint64_t score_ = 0;
  std::uint32_t step_count_ = 0;
  bool game_over_ = false;
  bool reached_2048_ = false;
  bool saw_overflow_ = false;
};

}  // namespace ai2048

#endif  // AI2048_CORE_GAME_H_
