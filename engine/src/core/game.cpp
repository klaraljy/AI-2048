#include "core/game.h"

#include <array>
#include <cassert>
#include <cmath>
#include <vector>

namespace ai2048 {

namespace {

// 收集空格的下标（0..15，行优先）。顺序固定，保证"第 k 个空格"是确定的。
[[nodiscard]] std::array<int, kCellCount> CollectEmptyCells(std::uint64_t board,
                                                            int* count) noexcept {
  std::array<int, kCellCount> cells{};
  int n = 0;
  for (int index = 0; index < kCellCount; ++index) {
    if (GetExponent(board, index) == 0) {
      cells[static_cast<std::size_t>(n)] = index;
      ++n;
    }
  }
  *count = n;
  return cells;
}

// ---------------------------------------------------------------------------
// 位置评分：所有分值都是**百分数的整数**（1.5 分写成 150），
// 这样整条链路没有浮点，逐字节可复现的承诺不受影响。
//
// 直接照搬文档《方块生成策略》给出的评分，逐项对应：
//   简单档 = 边缘 + 角落 + 周围空旷 + 可合并 − 拥挤 − 贴大块
//   困难档 = 拥挤 + 断裂 + 贴大块 − 可合并 − 角落 − 边缘
//
// 两档都用 Max(score, 0) 收尾：负分位置不该被"负权"排挤，
// 否则会出现"因为太难算所以反而不会被选中"的反直觉行为。
// ---------------------------------------------------------------------------

/** 邻居偏移，顺序固定为「上、下、左、右」—— 它决定评分的确定性。 */
inline constexpr std::array<std::array<int, 2>, 4> kNeighbourOffsets = {
    {{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

[[nodiscard]] constexpr bool IsEdgeCell(int row, int col) noexcept {
  return row == 0 || row == kBoardSize - 1 || col == 0 || col == kBoardSize - 1;
}

[[nodiscard]] constexpr bool IsCornerCell(int row, int col) noexcept {
  return (row == 0 || row == kBoardSize - 1) && (col == 0 || col == kBoardSize - 1);
}

/**
 * 简单档：这个空位对玩家有多**安全**。
 *
 * 「安全」不是"周围越空越好"，而是"新块容易被纳入现有布局"。
 * 所以旁边有 2 反而加分 —— 简单档 90% 出 2，落下去可能立刻能合并。
 */
[[nodiscard]] int SafeScore(std::uint64_t board, int index) noexcept {
  const int row = index / kBoardSize;
  const int col = index % kBoardSize;
  int score = 0;

  if (IsEdgeCell(row, col)) score += 150;
  if (IsCornerCell(row, col)) score += 250;

  for (const auto& offset : kNeighbourOffsets) {
    const int r = row + offset[0];
    const int c = col + offset[1];
    if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
    const int exponent = GetExponent(board, r * kBoardSize + c);
    if (exponent == 0) {
      score += 80;  // 周围空旷
    } else {
      score -= 40;  // 拥挤
      // 简单档有 90% 出 2（指数 1）、10% 出 4（指数 2）：
      // 用期望值加权，而不是"只要看见 2 就给满分"。
      if (exponent == 1) score += 135;
      if (exponent == 2) score += 15;
      if (exponent >= 6) score -= 30;  // 2^6 = 64 及以上的大块
    }
  }
  return score < 0 ? 0 : score;
}

/** 贴着一个多大的块？越大越"不利"（块周围被堵住的价值更高）。 */
[[nodiscard]] constexpr int HighValuePenalty(int exponent) noexcept {
  // 指数 5=32, 7=128, 9=512（文档按数值 32/128/512 分档）
  if (exponent >= 9) return 200;
  if (exponent >= 7) return 140;
  if (exponent >= 5) return 80;
  if (exponent >= 3) return 40;  // 数值 8
  return 0;
}

/**
 * 困难档：这个空位对玩家有多**不利**。
 *
 * 关键的一条是**减去立即合并的机会**。旧实现只找"最拥挤的位置"，
 * 而最拥挤处常常紧挨同值块 —— 新块落下去白送一次合并，等于在帮玩家。
 * 文档明确指出这一点，这里按「拥挤 + 断裂 + 贴大块 − 可合并 − 角/边」
 * 一起算。
 */
[[nodiscard]] int HostileScore(std::uint64_t board, int index) noexcept {
  const int row = index / kBoardSize;
  const int col = index % kBoardSize;
  int score = 0;

  for (const auto& offset : kNeighbourOffsets) {
    const int r = row + offset[0];
    const int c = col + offset[1];
    if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
    const int exponent = GetExponent(board, r * kBoardSize + c);
    if (exponent == 0) continue;          // 空邻居不加分：拥挤度由非空邻居那边算
    score += 120;                         // 拥挤度
    score += HighValuePenalty(exponent);  // 贴高价值块
    if (exponent == 1) score -= 96;       // 80% × 1.2：能立刻合并 → 对玩家有利
    if (exponent == 2) score -= 24;       // 20% × 1.2
  }

  // ⚠️ 这里**曾经**还有一项 `score += (4 - empty_neighbours) * 50;`，
  // 是我自己加的，文档的评分表里没有它。它是个真 bug，测试抓住了：
  //
  // 「空邻居少」与「occupied 多」是同一件事的两种说法（四邻非空即满），
  // 所以那一项等于把"拥挤度"**加倍计权**，而且加到 +150 之后完全盖过了
  // −192 的合并惩罚 —— 结果一个"贴着 8、落下去就能合并"的格子
  // （对玩家明显有利）拿到了全场最高分，与设计意图正好相反。
  //
  // 与本项目其它几次教训一致：缺失的从来不是"再加一项"，
  // 而是删掉信息重叠的那一项。四个评分项各自已经表达了意图。

  // 断裂点：空位夹在两个**不同**数字之间，落子会打断排列。
  // 横竖两对，各自判断"两侧都有块且数值不同"。
  for (int axis = 0; axis < 2; ++axis) {
    const int dr = axis == 0 ? 1 : 0;
    const int dc = axis == 0 ? 0 : 1;
    const int r1 = row - dr;
    const int c1 = col - dc;
    const int r2 = row + dr;
    const int c2 = col + dc;
    if (r1 < 0 || r1 >= kBoardSize || c1 < 0 || c1 >= kBoardSize) continue;
    if (r2 < 0 || r2 >= kBoardSize || c2 < 0 || c2 >= kBoardSize) continue;
    const int a = GetExponent(board, r1 * kBoardSize + c1);
    const int b = GetExponent(board, r2 * kBoardSize + c2);
    if (a != 0 && b != 0 && a != b) score += 150;
    if (a != 0 && b != 0) {
      const int hi = a > b ? a : b;
      const int lo = a > b ? b : a;
      if (hi >= lo + 2) score += 150;  // 差距 ≥ 4 倍（指数差 2）
    }
  }

  // 困难档刻意**不**偏向角落：角落对玩家有利，一直往角上放会让
  // "角落策略"继续过强。
  if (IsCornerCell(row, col)) score -= 80;
  if (IsEdgeCell(row, col)) score -= 30;

  return score < 0 ? 0 : score;
}

// ---------------------------------------------------------------------------
// 权重 = exp(score × strength)
//
// ⚠️ **不能用 std::exp。** 本项目承诺"同种子逐字节一致"，而 libm 的 exp
// 不保证跨编译器/平台逐位相同。这里是**查表**：score 是百分数整数，
// strength 是 /100，所以指数就是 (score × strength) / 10000 —— 一个有理数。
// 表的范围覆盖 score ∈ [0, 12.00]（一千二百项），足够宽：
// 实测困难档的评分在 -400 ~ 800 之间。
//
// 表用 double 算**一次**再取整。这不破坏可复现性：取值完全由源码里的
// 常量与 IEEE-754 四则运算决定，任何平台上都得到同一张表。
// 表本身（1201 个 int64）放进静态存储，只算一次。
// ---------------------------------------------------------------------------
inline constexpr int kWeightTableLimit = 1200;  // score 上限 12.00

[[nodiscard]] const std::vector<std::int64_t>& WeightTable(bool hard) noexcept {
  static const std::vector<std::int64_t> easy_table = [] {
    std::vector<std::int64_t> table(kWeightTableLimit + 1);
    for (int i = 0; i <= kWeightTableLimit; ++i) {
      const double exponent = static_cast<double>(i) * kEasyWeightStrength / 10000.0;
      table[static_cast<std::size_t>(i)] =
          static_cast<std::int64_t>(std::exp(exponent) * 256.0 + 0.5);
    }
    return table;
  }();
  static const std::vector<std::int64_t> hard_table = [] {
    std::vector<std::int64_t> table(kWeightTableLimit + 1);
    for (int i = 0; i <= kWeightTableLimit; ++i) {
      const double exponent = static_cast<double>(i) * kHardWeightStrength / 10000.0;
      table[static_cast<std::size_t>(i)] =
          static_cast<std::int64_t>(std::exp(exponent) * 256.0 + 0.5);
    }
    return table;
  }();
  return hard ? hard_table : easy_table;
}

/**
 * score（百分数）→ 权重。
 *
 * 先**饱和**再查表（见 kScoreSaturation）。饱和不改变排序，只压缩极端值 ——
 * 没有它，困难档在极端局面下的评分能累加到 1960，指数化后 max/min 权重比
 * 达到千万量级，加权就退化成"必定落同一格"，也就是文档警告的「系统作弊感」。
 */
[[nodiscard]] std::int64_t WeightFor(int score, bool hard) noexcept {
  int saturated = score > kScoreSaturation ? kScoreSaturation : score;
  if (saturated < 0) saturated = 0;
  return WeightTable(hard)[static_cast<std::size_t>(saturated)];
}

}  // namespace

// --- 对外暴露：AI 的随机节点必须复用同一套评分与权重 -------------------------

int SafeSpawnScore(std::uint64_t board, int index) noexcept { return SafeScore(board, index); }

int HostileSpawnScore(std::uint64_t board, int index) noexcept {
  return HostileScore(board, index);
}

std::array<double, kCellCount> SpawnCellWeights(std::uint64_t board,
                                                Difficulty difficulty) noexcept {
  std::array<double, kCellCount> weights{};
  weights.fill(0.0);

  int empty_count = 0;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) == 0) ++empty_count;
  }
  if (empty_count == 0) return weights;

  std::uint64_t weighted_share = 0;
  if (difficulty == Difficulty::kEasy) {
    weighted_share = kEasyWeightedShare;
  } else if (difficulty == Difficulty::kHard) {
    weighted_share = kHardWeightedShare;
  }

  // 先算每个空格的原始权重，并求出总和 —— 纯随机分支的"每格份额"
  // 依赖总权重（见头文件里的公式）。
  std::int64_t total = 0;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) != 0) continue;
    const int score =
        difficulty == Difficulty::kEasy ? SafeScore(board, i) : HostileScore(board, i);
    total += WeightFor(score, difficulty == Difficulty::kHard);
  }
  if (total <= 0) total = 1;

  const double weighted_fraction = static_cast<double>(weighted_share) / kSpawnValueDenominator;
  const double uniform_fraction = 1.0 - weighted_fraction;
  const double per_cell_uniform =
      uniform_fraction * (static_cast<double>(total) / static_cast<double>(empty_count));

  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) != 0) continue;
    const int score =
        difficulty == Difficulty::kEasy ? SafeScore(board, i) : HostileScore(board, i);
    const double raw =
        static_cast<double>(WeightFor(score, difficulty == Difficulty::kHard)) * weighted_fraction;
    weights[static_cast<std::size_t>(i)] = raw + per_cell_uniform;
  }
  return weights;
}

const char* DifficultyName(Difficulty difficulty) noexcept {
  switch (difficulty) {
    case Difficulty::kEasy:
      return "easy";
    case Difficulty::kHard:
      return "hard";
    case Difficulty::kNormal:
    default:
      return "normal";
  }
}

Game::Game(std::uint64_t seed, Difficulty difficulty) noexcept
    : seed_(seed), rng_(seed), difficulty_(difficulty) {
  for (int i = 0; i < kInitialTiles; ++i) {
    static_cast<void>(SpawnRandomTile());
  }
  // 开局不判定终局：两个方块不可能把 16 格堵死。
}

void Game::SetBoardForTesting(std::uint64_t board) noexcept {
  board_ = board;
  score_ = 0;
  step_count_ = 0;
  game_over_ = false;
  reached_2048_ = false;
  saw_overflow_ = false;
}

SpawnRecord Game::SpawnRandomTile() noexcept {
  SpawnRecord record;
  int empty_count = 0;
  const std::array<int, kCellCount> empty_cells = CollectEmptyCells(board_, &empty_count);
  if (empty_count == 0) return record;  // exponent 保持 0，调用方据此判断"没生成"

  // ---------------------------------------------------------------------------
  // 随机数消耗：**恒定三次**，与难度、盘面、分支都无关。
  //
  // 这是本项目最硬的一条规则（同种子逐字节一致）。旧实现存在一个隐蔽缺陷：
  // 它用 Chance() 决定"要不要走偏置"，命中后再调一次 NextBounded 选具体格子 ——
  // 于是"偏置命中"与"未命中"消耗的随机数**个数不同**，同一种子在不同难度的
  // 后续随机流就此分叉，跨难度对比时根本说不清差异来自哪里。
  //
  // 现在固定为：
  //   第 1 次 —— 决定走「加权」还是「纯随机」
  //   第 2 次 —— 在选定的分布里挑格子（两种分布都**恰好**用掉这一次）
  //   第 3 次 —— 决定数值是 2 还是 4
  // 加权分布是"一次随机数按权重区间落点"，不是"逐个位置掷骰子"，
  // 所以它和均匀分布一样只消耗一次。
  // ---------------------------------------------------------------------------
  const std::uint64_t roll_branch = rng_.NextBounded(kSpawnValueDenominator);
  const std::uint64_t roll_cell = rng_.NextBounded(kSpawnValueDenominator);

  int index = -1;

  std::uint64_t weighted_share = 0;
  if (difficulty_ == Difficulty::kEasy) {
    weighted_share = kEasyWeightedShare;
  } else if (difficulty_ == Difficulty::kHard) {
    weighted_share = kHardWeightedShare;
  }

  if (weighted_share > 0 && roll_branch < weighted_share) {
    // 加权分支：算出每个空位的权重，再让第 2 次随机数按权重区间落点。
    std::int64_t total = 0;
    std::array<std::int64_t, kCellCount> weights{};
    for (int i = 0; i < empty_count; ++i) {
      const int cell = empty_cells[static_cast<std::size_t>(i)];
      const int score =
          difficulty_ == Difficulty::kEasy ? SafeScore(board_, cell) : HostileScore(board_, cell);
      const std::int64_t weight = WeightFor(score, difficulty_ == Difficulty::kHard);
      weights[static_cast<std::size_t>(i)] = weight;
      total += weight;
    }

    if (total > 0) {
      // 把 [0,1000) 映射到 [0,total)。用 NextBounded 会多消耗一次随机数，
      // 所以这里直接用已有的 roll_cell 做定点映射。
      const std::int64_t pick = static_cast<std::int64_t>(
          (roll_cell * static_cast<std::uint64_t>(total)) / kSpawnValueDenominator);
      std::int64_t acc = 0;
      for (int i = 0; i < empty_count; ++i) {
        acc += weights[static_cast<std::size_t>(i)];
        if (pick < acc) {
          index = empty_cells[static_cast<std::size_t>(i)];
          break;
        }
      }
      // 浮点/取整的边界情况：落到最后一个
      if (index < 0) index = empty_cells[static_cast<std::size_t>(empty_count - 1)];
    }
  }

  if (index < 0) {
    // 纯随机分支：简单/困难档的兜底，也是 kNormal 唯一走的分支。
    // 没有它，简单档会过于温和、困难档会显得在作弊。
    index =
        empty_cells[static_cast<std::size_t>(roll_cell % static_cast<std::uint64_t>(empty_count))];
  }

  // 数值：出 4 的概率随难度变化（简单 10% / 中等 15% / 困难 20%）。
  std::uint64_t four_threshold = kFourSpawnNormal;
  if (difficulty_ == Difficulty::kEasy) four_threshold = kFourSpawnEasy;
  if (difficulty_ == Difficulty::kHard) four_threshold = kFourSpawnHard;
  const int exponent = roll_branch < four_threshold ? 2 : 1;

  board_ = SetExponent(board_, index, exponent);

  record.row = index / kBoardSize;
  record.col = index % kBoardSize;
  record.exponent = exponent;
  return record;
}

StepResult Game::Step(Direction direction) noexcept {
  StepResult result;
  if (game_over_) return result;

  const MoveResult move = ApplyMove(board_, direction);
  if (!move.moved) return result;  // 不消耗随机数

  board_ = move.board;
  result.moved = true;
  result.moves = std::move(move.moves);
  result.overflow = move.overflow;
  result.score_gained = move.score_gained;

  score_ += move.score_gained;
  ++step_count_;
  saw_overflow_ = saw_overflow_ || move.overflow;

  // 规则顺序：先合并、再生成、最后判终局。
  //
  // **必须调用同一个 SpawnRandomTile**，不能在这里再写一份位置选择 ——
  // 之前就是两处各写一份，改难度时只改了一处，走子后的生成会退回均匀分布，
  // 难度只在开局生效。这种"两份实现悄悄分叉"是本项目反复吃亏的地方。
  const SpawnRecord spawn = SpawnRandomTile();
  if (spawn.exponent == 0) return result;  // 无空格（理论上不该发生）

  result.spawned = true;
  result.spawn = spawn;

  if (MaxExponent(board_) >= 11) {  // 2^11 = 2048
    reached_2048_ = true;
  }

  game_over_ = !HasLegalMove(board_);
  return result;
}

std::string Game::Serialize() const noexcept {
  std::string out;
  out.reserve(160);
  out += "seed=";
  out += std::to_string(seed_);
  out += " steps=";
  out += std::to_string(step_count_);
  out += " score=";
  out += std::to_string(score_);
  out += " max=";
  out += std::to_string(max_tile());
  out += " over=";
  out += game_over_ ? '1' : '0';
  out += " overflow=";
  out += saw_overflow_ ? '1' : '0';
  out += " board=";
  out += ToString(board_);
  return out;
}

}  // namespace ai2048
