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
  // ⚠️ 这里原来有一句 `score < 0 ? 0 : score`，与困难档那处的缺陷同源：
  // 一个被 4 个大块围住的正中枢格会被夹回 0，与"平庸但无害"的格子同权 ——
  // 而它其实是最该被避开的位置。现在允许负分（见 kScoreFloor）。
  return score;
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
 * 某一行 / 某一列有几个空格。
 *
 * 困难档"填满行/列"的判据要用它 —— 与格子数值无关，只看满不满。
 */
[[nodiscard]] int RowEmptyCount(std::uint64_t board, int row) noexcept {
  int n = 0;
  for (int c = 0; c < kBoardSize; ++c) {
    if (GetExponent(board, row * kBoardSize + c) == 0) ++n;
  }
  return n;
}

[[nodiscard]] int ColEmptyCount(std::uint64_t board, int col) noexcept {
  int n = 0;
  for (int r = 0; r < kBoardSize; ++r) {
    if (GetExponent(board, r * kBoardSize + col) == 0) ++n;
  }
  return n;
}

/**
 * 困难档：这个空位对玩家有多**不利**。
 *
 * ⚠️ **主导项是「封最大块的路」，其它项只是微调。** 这是 2026-09-13 的第二次
 * 修订，起因是用户实测反馈："生成在空白位置的概率还是比较高，我想要的是
 * 大概率生成在有空闲位置的最大方块旁边 —— 比如一行里面有三个，你就要大概率
 * 生成在第四个把这一行封死。"
 *
 * 上一版的问题（实测，2 万次采样/盘面）：
 *   - 判据是「贴着**任意** 32+ 的块」（+80/+140/+200），不是「贴着最大的块」；
 *   - 于是"拥挤度 +120 × 多个邻居"和"断裂点 +150"都能盖过它；
 *   - 结果**最大块所在的行/列完全没有被优待**：盘面实测占 42.8%，
 *     而均匀基期是 42.9% —— 等于没有偏置。
 *
 * 现在改成两层结构：先看"是否与最大块同行/同列"（决定性），
 * 再在层内用拥挤度、断裂点、可合并惩罚微调。这样"封路位置"必然排在前面。
 */
[[nodiscard]] int HostileScore(std::uint64_t board, int index) noexcept {
  const int row = index / kBoardSize;
  const int col = index % kBoardSize;
  int score = 0;

  // ---------------------------------------------------------------------------
  // 主导项：**把行/列填满**（用户的明确要求）
  // ---------------------------------------------------------------------------
  // 用户原话（2026-09-13 第三次修订）：
  //
  //   「假如，空、32、8、4，你就高概率生成在空位，然后导致封路。
  //     封路和方块大小数字不同没有关系，就是要使这一行填满，
  //     应该主要是填满而不是封路。」
  //
  // 所以判据是「这一行/列**已经有几块**」，**与格子的数值完全无关**，
  // 也不是我先前理解的"两侧数字不同"。机理：
  //
  //   一行只要还有空位，它就能被继续滑动、也能与相邻行交换牌；
  //   填满之后这一行就**刚性**了 —— 玩家只能靠移动整盘来重新腾挪。
  //   填得越满的一行，再填一格的边际伤害越大。
  //
  // 之前几版都在看"数值"（贴大块、异值接缝），方向偏了：
  // 实测用户给的形态 `空,32,8,4` 拿不到高概率。
  const int row_tiles = kBoardSize - RowEmptyCount(board, row);
  const int col_tiles = kBoardSize - ColEmptyCount(board, col);
  const int line_tiles = row_tiles > col_tiles ? row_tiles : col_tiles;

  // 3 块的行/列填第 4 格 → 1400；完全空的行列 → 只有 60。
  static_assert(kBoardSize == 4, "下面的分档按 4x4 硬编码，改尺寸要同步改");
  static constexpr int kFillBonus[kBoardSize + 1] = {0, 60, 900, 1200, 1400};
  score += kFillBonus[line_tiles];

  // 这个空格两侧都有块（填上就连成一条）→ 略微再加，表示"补的是缺口"
  // 而不是行尾的延伸。数值仍不参与。
  if (row_tiles >= 2 && col > 0 && col + 1 < kBoardSize &&
      GetExponent(board, row * kBoardSize + col - 1) != 0 &&
      GetExponent(board, row * kBoardSize + col + 1) != 0) {
    score += 120;
  }
  if (col_tiles >= 2 && row > 0 && row + 1 < kBoardSize &&
      GetExponent(board, (row - 1) * kBoardSize + col) != 0 &&
      GetExponent(board, (row + 1) * kBoardSize + col) != 0) {
    score += 120;
  }

  // 贴最大块 / 在最大块的行列上：次要加分（第二策略）。
  int max_exponent = 0;
  int max_index = -1;
  for (int i = 0; i < kCellCount; ++i) {
    const int e = GetExponent(board, i);
    if (e > max_exponent) {
      max_exponent = e;
      max_index = i;
    }
  }
  if (max_index >= 0) {
    const int mr = max_index / kBoardSize;
    const int mc = max_index % kBoardSize;
    const int dr = row > mr ? row - mr : mr - row;
    const int dc = col > mc ? col - mc : mc - col;
    if (dr + dc == 1)
      score += 300;  // 正贴着最大块
    else if (dr == 0 || dc == 0)
      score += 120;  // 在最大块的行/列上
  }

  // ---------------------------------------------------------------------------
  // 微调：拥挤度与「可立即合并」
  // ---------------------------------------------------------------------------
  for (const auto& offset : kNeighbourOffsets) {
    const int r = row + offset[0];
    const int c = col + offset[1];
    if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
    const int exponent = GetExponent(board, r * kBoardSize + c);
    if (exponent == 0) continue;
    score += 60;                      // 拥挤度
    if (exponent == 1) score -= 120;  // 邻格是 2：落子白送一次合并 → 对玩家有利
    if (exponent == 2) score -= 40;   // 邻格是 4
  }

  // ⚠️ 这里**曾经**有一个「断裂点」循环（两侧都有块且数值不同 → +100/+100）。
  // 它与"填满行/列"的意图**信息重叠**：一行的空位被填满时，两侧自然就是块，
  // 留着等于对同一件事加倍计权，而且只给 +100、与主导项（+900~1400）量级不搭。
  // 前端已经删掉，C++ 这里漏删了 —— 结果两边分数差 200/400，
  // 前端与引擎的生成分布静默分叉。信息重叠的项要删掉，不是叠加
  // （这条教训本项目已经踩过四次，这是第四次）。

  // 困难档刻意**不**偏向角落：角落对玩家有利，一直往角上放会让
  // "角落策略"继续过强。
  //
  // ⚠️ 这两条惩罚以前**完全无效** —— 函数末尾有一句 `score < 0 ? 0 : score`，
  // 而一个空角落格通常没有任何加分、本身就得 0 分，减去 80 之后又被夹回 0。
  // 于是"不往角落放"这条设计意图从未生效：实测角落格与平庸 0 分格的落点
  // 份额完全一样。现在允许负分（见 kScoreFloor）。
  if (IsCornerCell(row, col)) score -= 80;
  if (IsEdgeCell(row, col)) score -= 30;

  return score;
}

// ---------------------------------------------------------------------------
// 权重 = exp(score × strength)
//
// ⚠️ **不能用 std::exp。** 本项目承诺"同种子逐字节一致"，而 libm 的 exp
// 不保证跨编译器/平台逐位相同。这里是**查表**：score 是百分数整数，
// strength 是 /100，所以指数就是 (score × strength) / 10000 —— 一个有理数。
//
// 表的下标从 kScoreFloor（负）起，覆盖 score ∈ [−4.00, +9.00]。
// **负分是必需的**：困难档的角落/边缘惩罚是负的，把它夹成 0 就等于没有惩罚
// （见 kScoreFloor 的说明）。负分对应的权重会下溢截断成 0 —— 这是**对的**，
// 那种格子就该几乎不出块。
//
// 表用 double 算**一次**再取整。这不破坏可复现性：取值完全由源码里的
// 常量与 IEEE-754 四则运算决定，任何平台上都得到同一张表。
// ---------------------------------------------------------------------------
inline constexpr int kWeightTableLimit = kScoreSaturation;

// 运行期可覆盖的 strength / 加权占比。
//
// **仅用于标定工具**（`tools/` 下的分析器与探针）：偏置强度这类参数只有把
// "不同取值下的实际分布"并排打出来才能选，而每试一个值都重编译一次太慢。
//
// ⚠️ 默认值就是编译期常量，**生产路径不调用 SetWeightParamsForTesting**，
// 所以确定性不受影响（同种子逐字节一致的承诺只依赖默认路径）。
int g_hard_weight_strength = kHardWeightStrength;
int g_easy_weight_strength = kEasyWeightStrength;
std::uint64_t g_easy_weighted_share = kEasyWeightedShare;
std::uint64_t g_hard_weighted_share = kHardWeightedShare;

[[nodiscard]] const std::vector<std::int64_t>& WeightTable(bool hard) noexcept {
  const auto build = [](std::int32_t strength) {
    std::vector<std::int64_t> table(static_cast<std::size_t>(kWeightTableLimit - kScoreFloor + 1));
    for (int score = kScoreFloor; score <= kWeightTableLimit; ++score) {
      const double exponent = static_cast<double>(score) * static_cast<double>(strength) / 10000.0;
      const double scaled = std::exp(exponent) * 256.0 + 0.5;
      table[static_cast<std::size_t>(score - kScoreFloor)] =
          scaled < 1.0 ? 0 : static_cast<std::int64_t>(scaled);
    }
    return table;
  };
  // 表按 strength 缓存：标定工具会换 strength，缓存键必须跟着变。
  static const std::vector<std::int64_t> easy_table = build(kEasyWeightStrength);
  static const std::vector<std::int64_t> hard_table = build(kHardWeightStrength);
  static std::vector<std::int64_t> easy_override;
  static std::vector<std::int64_t> hard_override;
  static int easy_override_strength = kEasyWeightStrength;
  static int hard_override_strength = kHardWeightStrength;

  if (hard) {
    if (g_hard_weight_strength != hard_override_strength) {
      hard_override = build(g_hard_weight_strength);
      hard_override_strength = g_hard_weight_strength;
    }
    if (!hard_override.empty()) return hard_override;
    return hard_table;
  }
  if (g_easy_weight_strength != easy_override_strength) {
    easy_override = build(g_easy_weight_strength);
    easy_override_strength = g_easy_weight_strength;
  }
  if (!easy_override.empty()) return easy_override;
  return easy_table;
}

/**
 * score（百分数）→ 权重。
 *
 * 先**饱和**再查表（见 kScoreSaturation）。饱和不改变排序，只压缩极端值 ——
 * 没有它，困难档在极端局面下的评分能累加到 1960，指数化后 max/min 权重比
 * 达到千万量级，加权就退化成"必定落同一格"，也就是文档警告的「系统作弊感」。
 */
[[nodiscard]] std::int64_t WeightFor(int score, bool hard) noexcept {
  // 上界饱和、下界截断。**不能把负分夹成 0** —— 那样困难档的角落/边缘惩罚
  // 就等于没有（见 kScoreFloor 的说明，这是实际踩过的缺陷）。
  int bounded = score > kScoreSaturation ? kScoreSaturation : score;
  if (bounded < kScoreFloor) bounded = kScoreFloor;
  return WeightTable(hard)[static_cast<std::size_t>(bounded - kScoreFloor)];
}

}  // namespace

// --- 对外暴露：AI 的随机节点必须复用同一套评分与权重 -------------------------

int SafeSpawnScore(std::uint64_t board, int index) noexcept { return SafeScore(board, index); }

int HostileSpawnScore(std::uint64_t board, int index) noexcept {
  return HostileScore(board, index);
}

int ExportScoreFloor() noexcept { return kScoreFloor; }
int ExportScoreSaturation() noexcept { return kScoreSaturation; }

std::vector<std::int64_t> ExportWeightTableForTesting(bool hard) { return WeightTable(hard); }

void SetWeightParamsForTesting(int strength, int weighted_share) noexcept {
  if (strength > 0) {
    g_hard_weight_strength = strength;
    g_easy_weight_strength = strength;
  }
  if (weighted_share >= 0 && weighted_share <= 1000) {
    g_hard_weighted_share = static_cast<std::uint64_t>(weighted_share);
    g_easy_weighted_share = static_cast<std::uint64_t>(weighted_share);
  }
}

int EffectiveHardWeightStrength() noexcept { return g_hard_weight_strength; }

int EffectiveHardWeightedShare() noexcept { return static_cast<int>(g_hard_weighted_share); }

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
    weighted_share = g_easy_weighted_share;
  } else if (difficulty == Difficulty::kHard) {
    weighted_share = g_hard_weighted_share;
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
    weighted_share = g_easy_weighted_share;
  } else if (difficulty_ == Difficulty::kHard) {
    weighted_share = g_hard_weighted_share;
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

  // ⚠️ **必须取一个新的随机数**，不能复用 roll_branch。
  //
  // 这里踩过一个很隐蔽的坑：原先写成 `roll_branch < four_threshold`，
  // 于是"生成 2 还是 4"由**位置偏置那一次随机数**决定。两个后果：
  //   1. 每次生成只消耗 2 个随机数而不是 3 个 → 整个随机流每次生成错位一格，
  //      前端与引擎再也对不上，而且同一种子在不同难度下分叉。
  //   2. 数值与位置**统计相关**：简单/困难档只在 roll_branch < 800/750 时
  //      才走加权分支，而出 4 的门槛只有 100~200 —— 也就是"走加权分支时
  //      几乎必然出 2"，出 4 的实际概率被严重压低且与位置耦合。三档的
  //      期望生成值（2.2/2.3/2.4）实际都没有达到。
  //
  // 这个 bug 是靠"给 Rng 加消耗计数器"发现的：构造 Game(1) 只消耗了 4 次
  // 而不是 6 次。**光读代码看不出来，必须做这种运行时计数。**
  const std::uint64_t roll_value = rng_.NextBounded(kSpawnValueDenominator);
  const int exponent = roll_value < four_threshold ? 2 : 1;

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
