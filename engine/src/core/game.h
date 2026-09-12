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

// ---------------------------------------------------------------------------
// 新方块的**数值**：出 4 的概率随难度变化
//
//     简单 10%   中等 15%   困难 20%
//
// ⚠️ 这是 2026-09-12 的**规则修订**，推翻了先前"难度只改位置、不改取值概率"
// 的决定。推翻的理由：那条决定的依据是"取值一起变会让随机性也变、没法归因"，
// 但三档的**期望生成值**只差 2.2 / 2.3 / 2.4（差 9%），而归因问题由
// "分数不跨难度比较 + 跑批必须带难度标记"这两条已经兜住了。
// 权衡下来，"中等档比原版略难、困难档更难"对可玩性的价值更高。
//
// 数值都写成 /1000，配合 Rng::NextBounded(1000) 比较 —— 整数比较，
// 不引入浮点，逐字节可复现的承诺不受影响。
// ---------------------------------------------------------------------------
inline constexpr std::uint64_t kSpawnValueDenominator = 1000;
inline constexpr std::uint64_t kFourSpawnEasy = 100;    // 10%
inline constexpr std::uint64_t kFourSpawnNormal = 150;  // 15%
inline constexpr std::uint64_t kFourSpawnHard = 200;    // 20%

// 开局放几个方块。
inline constexpr int kInitialTiles = 2;

// ---------------------------------------------------------------------------
// 难度：改变**新方块的位置分布**与**数值分布**
//
// 位置采用「加权随机 + 纯随机兜底」，而不是确定性的"n% 走偏置分支"：
//
//   简单：80% 按安全分加权，20% 均匀随机
//   中等：100% 均匀随机（标准 2048）
//   困难：75% 按不利分加权，25% 均匀随机
//
// 为什么改成加权而不是布尔偏置（这是本次修订的核心）：
//
//  1. **确定性偏置会让 AI 变得可预测且不公平。** 旧实现是"以 80% 概率
//     跳到最大块旁边"，那 80% 一旦触发就**精确落在最难受的位置**，
//     玩家的感受是"系统在针对我"，而不是"我运气不好"。
//  2. **困难档的旧规则会帮玩家合并。** 最拥挤的位置往往紧挨着同值块，
//     新块落下去反而送一次合并。文档明确指出这一点，并给出正确做法：
//     同时考虑「拥挤度 + 断裂程度 + 靠近高价值块 − 立即合并机会」。
//  3. 加权随机保留了尾部概率，所以**极端情况仍会发生但不总发生**，
//     三档的难度差异表现为"分布不同"而不是"有无偏置"。
//
// 纯随机兜底的比例是**规则的一部分**：没有它，简单档会变得过于温和、
// 困难档会显得作弊。
//
// ⚠️ 分数不跨难度比较；跑批与界面必须带难度标记。改动本节的任何数值
// 都会让全部历史分数作废，必须同步提升 RulesetVersion()。
// ---------------------------------------------------------------------------
enum class Difficulty : std::uint8_t {
  kEasy = 0,    // 80% 偏向安全空位（边/角/空旷/可合并）+ 20% 均匀
  kNormal = 1,  // 全盘均匀（标准 2048）
  kHard = 2,    // 75% 偏向不利空位（拥挤/断裂/贴大块/难合并）+ 25% 均匀
};

// 加权与纯随机的比例（/1000）。
inline constexpr std::uint64_t kEasyWeightedShare = 800;
inline constexpr std::uint64_t kHardWeightedShare = 750;

// 权重 = exp(score × strength) 的放大强度（/100）。
//
// ⚠️ 这两个值**不是**文档给的 0.70 / 0.85，而是实测标定出来的。原因见
// kScoreSaturation 的说明：文档的 strength 是按"评分量级 ~5"设计的，
// 而文档自己给的评分常数（拥挤 +120、断裂 +150、贴大块 +200…）加总起来
// 量级到了 19.6，两者不匹配。
//
// 实测（20 万随机空格，strength 0.85 时困难档的 max/min 权重比达到 1700 万倍）
// 会让加权完全压倒 25% 的纯随机兜底 —— 表现为"困难档几乎总落在同一格"，
// 也就是文档自己警告的「系统作弊感」。
inline constexpr std::int32_t kEasyWeightStrength = 35;  // 0.35
inline constexpr std::int32_t kHardWeightStrength = 35;  // 0.35

/**
 * 评分参与指数运算前的**饱和上限**（百分数）。
 *
 * 为什么需要它：`exp(score × strength)` 只有在 score 有界时才是个可控的旋钮。
 * 而困难档的评分是若干惩罚项之和，极端局面下会累加到 1960（19.6 分），
 * 于是 `exp(19.6 × 0.85)` 直接把分布压成一个点。
 *
 * 饱和之后 strength 才能像文档预期的那样工作：0.35 配上 600 的上限，
 * max/min 权重比约 8 倍 —— 足以让"更不利的位置"稳定地更常出现，
 * 又不会被单格垄断。
 *
 * ⚠️ 饱和**不改变排序**，只压缩极端值 —— 所以"哪个位置更不利"的判断不受影响。
 */
inline constexpr int kScoreSaturation = 600;

[[nodiscard]] const char* DifficultyName(Difficulty difficulty) noexcept;

/**
 * 某个**空**格在简单档下的"安全分"（百分数整数，越大越安全）。
 *
 * 暴露出来是为了让 AI 的随机节点能用**与生成规则完全相同**的一套评分。
 * 各写一份必然分叉 —— 本项目已经因为"两份实现悄悄分叉"吃过亏
 * （走子后的生成退回均匀分布，难度只在开局生效）。
 */
[[nodiscard]] int SafeSpawnScore(std::uint64_t board, int index) noexcept;

/** 某个**空**格在困难档下的"不利分"（百分数整数，越大越不利）。 */
[[nodiscard]] int HostileSpawnScore(std::uint64_t board, int index) noexcept;

/**
 * 空格的**相对生成权重**，未归一化。
 *
 * 位置分布 = 加权分支(w) + 纯随机分支(1−w)，其中
 *     简单 w = 0.80，中等 w = 0，困难 w = 0.75
 * 加权分支内部按 exp(score × strength) 分配。
 *
 * 所以第 i 个空格的相对权重是
 *     w × WeightFor(score_i) + (1−w) × (总权重 / 空格数)
 * 调用方自行归一化即可。**注意**：这里不叠乘数值概率
 * （0.9/0.85/0.8 出 2），因为数值与位置相互独立，而 AI 的期望值计算里
 * 0.9+0.1=1 的系数会整体约掉。要按数值分开算时再各自乘。
 */
[[nodiscard]] std::array<double, kCellCount> SpawnCellWeights(std::uint64_t board,
                                                              Difficulty difficulty) noexcept;

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
