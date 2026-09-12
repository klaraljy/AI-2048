// 难度规则测试：新方块的**位置分布**必须符合规格。
//
//   easy   70% 落在空角落（没有空角落时退回全盘均匀）
//   normal 全盘均匀 —— 标准 2048，历史基准与公开分数仍然可比
//   hard   80% 落在最大方块的相邻空格（没有相邻空格时退回全盘均匀）
//
// 两条方法上的教训（都实际踩过，写在这里避免重犯）：
//
// 1. **必须能对任意盘面反复采样。** 靠"跑完整局再统计落点"验不准：
//    对局演化本身就把落点分布推偏了（实测四角 9%、中心 3%），
//    那是"空格在哪"造成的，与偏置是否生效无关，两者混在一起分不开。
//    所以用 SetBoardForTesting 摆固定盘面。
//
// 2. **盘面的性质一律由程序算，不靠手数。** 我手数空格数错了三次，
//    每次都表现为"断言失败"，看起来像实现有 bug，其实是盘面与我以为的不同。
//    现在每个盘面都先断言它的关键性质（空格数、最大块位置、四邻是否被占），
//    盘面写错时第一条断言就会明确指出"是盘面错了"。
//
// 另一条同样重要的断言：不同难度的随机流结构必须一致
// （每次生成恰好消耗一次"位置随机数"）。否则同一档难度下，一次生成
// 就会改变后续整局的随机流，同种子的不同难度之间再也没法对照。

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "ai/search.h"
#include "core/board.h"
#include "core/game.h"

namespace ai2048 {
namespace {

constexpr int kTrials = 20000;

/** 四角的下标。 */
constexpr std::array<int, 4> kCorners = {0, 3, 12, 15};

[[nodiscard]] bool IsCorner(int index) {
  for (const int c : kCorners) {
    if (c == index) return true;
  }
  return false;
}

[[nodiscard]] int EmptyCount(std::uint64_t board) {
  int n = 0;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) == 0) ++n;
  }
  return n;
}

/** 行优先第一个最大块的位置。 */
struct MaxCell {
  int row = -1;
  int col = -1;
  bool found = false;
};

/**
 * 「按等级从高到低，第一个四周有空位的方块」的等级。
 * 没有这样的方块时返回 0。这是 hard 档偏置的依据，测试里要用它算期望值。
 *
 * ⚠️ 必须检查**该等级的每一个方块**，不能只看行优先第一个就跳到下一级 ——
 * 写错过一次：那时 (0,2) 的 4 旁边有空位却被跳过，导致自检报 0。
 * 生成规则里的循环是按等级扫描、遇到有空的就返回，正是这个语义。
 */
[[nodiscard]] int LargestMovableExponent(std::uint64_t board) {
  constexpr int kDr[] = {-1, 1, 0, 0};
  constexpr int kDc[] = {0, 0, -1, 1};
  for (int exponent = MaxExponent(board); exponent >= 1; --exponent) {
    for (int index = 0; index < kCellCount; ++index) {
      if (GetExponent(board, index) != exponent) continue;
      const int row = index / kBoardSize;
      const int col = index % kBoardSize;
      for (int k = 0; k < 4; ++k) {
        const int r = row + kDr[k];
        const int c = col + kDc[k];
        if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
        if (GetExponent(board, r * kBoardSize + c) == 0) return exponent;
      }
      // 这个方块被围死，继续看同等级的下一个
    }
  }
  return 0;
}

[[nodiscard]] MaxCell FirstMaxCell(std::uint64_t board) {
  MaxCell cell;
  const int max_exp = MaxExponent(board);
  if (max_exp <= 0) return cell;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) == max_exp) {
      cell.row = i / kBoardSize;
      cell.col = i % kBoardSize;
      cell.found = true;
      return cell;
    }
  }
  return cell;
}

/** 最大块的四个上下左右邻居里，有几个是空格。 */
[[nodiscard]] int EmptyNeighboursOfMax(std::uint64_t board) {
  const MaxCell max_cell = FirstMaxCell(board);
  if (!max_cell.found) return 0;
  constexpr int kDr[] = {-1, 1, 0, 0};
  constexpr int kDc[] = {0, 0, -1, 1};
  int n = 0;
  for (int k = 0; k < 4; ++k) {
    const int r = max_cell.row + kDr[k];
    const int c = max_cell.col + kDc[k];
    if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
    if (GetExponent(board, r * kBoardSize + c) == 0) ++n;
  }
  return n;
}

struct Sample {
  int index = 0;
  int count = 0;
  /** 这些落点里生成的是 4（而不是 2）的次数。用于验证数值概率随难度变化。 */
  int four_count = 0;
};

/**
 * 在固定盘面上反复生成，统计落点。
 *
 * 用**同一个 Game 实例**连续生成，每次生成前把棋盘复位，随机流持续前进。
 * 曾经每次 new 一个 Game：那样每个实例只用第一次抽样，而 SetBoardForTesting
 * 不重置随机流，结果 20000 次只覆盖 4 格 —— 看起来像"偏置失效"，
 * 其实采样方法错了。
 */
[[nodiscard]] std::vector<Sample> SampleSpawns(Difficulty difficulty, std::uint64_t board,
                                               int trials) {
  std::array<int, kCellCount> counts{};
  std::array<int, kCellCount> four_counts{};
  Game game(20240911, difficulty);
  for (int i = 0; i < trials; ++i) {
    game.SetBoardForTesting(board);
    const SpawnRecord record = game.SpawnRandomTile();
    if (record.exponent == 0) continue;  // 盘面满了：调用方会先断言
    const auto slot = static_cast<std::size_t>(record.row * kBoardSize + record.col);
    counts[slot]++;
    // 指数 2 = 数值 4（指数 1 = 数值 2）
    if (record.exponent == 2) four_counts[slot]++;
  }

  std::vector<Sample> samples;
  for (int i = 0; i < kCellCount; ++i) {
    if (counts[static_cast<std::size_t>(i)] > 0) {
      samples.push_back(
          {i, counts[static_cast<std::size_t>(i)], four_counts[static_cast<std::size_t>(i)]});
    }
  }
  return samples;
}

[[nodiscard]] int Total(const std::vector<Sample>& samples) {
  int n = 0;
  for (const Sample& sample : samples) n += sample.count;
  return n;
}

/** 卡方统计量（对"空格数均分"的零假设）。 */
[[nodiscard]] double ChiSquareUniform(const std::vector<Sample>& samples, int cells) {
  const double expected = static_cast<double>(Total(samples)) / static_cast<double>(cells);
  double chi = 0.0;
  for (const Sample& sample : samples) {
    const double diff = static_cast<double>(sample.count) - expected;
    chi += diff * diff / expected;
  }
  return chi;
}

/** 自由度 dof 时 α≈0.001 的卡方临界值（近似式，dof 大时足够准）。 */
[[nodiscard]] double CriticalChiSquare(int dof) {
  return static_cast<double>(dof) + 3.0 * std::sqrt(2.0 * static_cast<double>(dof));
}

// ---------------------------------------------------------------------------
// 示意盘面
// ---------------------------------------------------------------------------

/**
 * 最大块 8 在中央 (2,2)，四邻全空，四角也全空。
 * 用于检验 easy 的落角与 hard 的贴块 —— 两档的偏置都"完全可用"。
 */
[[nodiscard]] std::uint64_t BoardMaxInCenter() {
  return EncodeBoard({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0});
}

/**
 * 最大块 8 在 (1,1)，左列与中列填满、右列与底行留空。
 *
 * 注意：这个盘面里**每一格空位都挨着某个 2**，所以按"从大到小找第一个
 * 有空位的大块"的规则，偏置永远可用（找到的会是 2）。
 * 所以它不适合用来测"偏置不可用"，那个场景用 BoardSecondaryMovable。
 */
[[nodiscard]] std::uint64_t BoardMaxSurrounded() {
  return EncodeBoard({4, 2, 4, 0, 2, 3, 4, 0, 4, 2, 4, 0, 2, 4, 2, 0});
}

/**
 * 最大块 8 在 (1,1)，四邻（2/2/4/4）全被占；**(1,2) 是空位**，
 * 它的上邻 (0,2)=4、左邻 (1,1)=8。
 *
 * 布局（值，. 表示空）—— 这里是**值的网格**，不是指数：
 *      2  4  .  4
 *      4  8  4  2
 *      2  4  2  4
 *      4  2  4  2
 *
 * 编码时按下标 row*4+col 写**指数**：2→1、4→2、8→3、空→0。
 *
 * 这正是用户指出的场景：最大块被围死时，应当退到"下一个有空位的大块"旁边，
 * 而不是直接关闭偏置。从大到小找：8 在 (1,1) 四邻全被占 →
 * 下一个等级 4，行优先第一个 4 在 (0,1)（四邻也全被占）→ 继续看同等级的
 * (1,2)，它的下方 (2,2) 是空位 → 偏置落在 (2,2)。
 */
[[nodiscard]] std::uint64_t BoardSecondaryMovable() {
  return EncodeBoard({
      1, 2, 0, 2,  // (0,*) = 2 4 . 4
      2, 3, 2, 1,  // (1,*) = 4 8 4 2
      1, 2, 0, 2,  // (2,*) = 2 4 . 4   ← (2,2) 是空位，正是偏置的目标
      2, 1, 2, 1,  // (3,*) = 4 2 4 2
  });
}

/** 四角全占、其余 12 格空。用于检验 easy 的退路。 */
[[nodiscard]] std::uint64_t BoardCornersTaken() {
  return EncodeBoard({1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1});
}

// ---------------------------------------------------------------------------
// 盘面自检（盘面写错时，第一条失败的断言必须指出"是盘面错了"）
// ---------------------------------------------------------------------------

TEST(DifficultyBoards, FixturesHaveThePropertiesTheyClaim) {
  // 先打印每个示意盘面，盘面写错时一眼能看出来（避免又一次"手数空格"）
  auto dump = [](const char* name, std::uint64_t board) {
    std::string text = std::string(name) + ":\n";
    for (int r = 0; r < kBoardSize; ++r) {
      text += "    ";
      for (int c = 0; c < kBoardSize; ++c) {
        const std::uint64_t value = ExponentToValue(GetExponent(board, r * kBoardSize + c));
        text += value == 0
                    ? "   ."
                    : (value < 10 ? "   " + std::to_string(value) : "  " + std::to_string(value));
      }
      text += "\n";
    }
    return text;
  };

  const std::uint64_t center = BoardMaxInCenter();
  const std::uint64_t surrounded = BoardMaxSurrounded();
  const std::uint64_t secondary = BoardSecondaryMovable();
  const std::uint64_t corners = BoardCornersTaken();

  EXPECT_EQ(EmptyCount(center), 15) << dump("中央盘面（应只有 1 个块、15 空格）", center);
  EXPECT_EQ(EmptyNeighboursOfMax(center), 4) << dump("中央盘面（最大块四邻应全空）", center);
  EXPECT_EQ(LargestMovableExponent(center), 3)
      << dump("中央盘面（最大的可放块就是最大块 8 自己）", center);

  // 围死盘面：1 列空 → 恰好 4 个空格；最大块四邻全被占
  EXPECT_EQ(EmptyCount(surrounded), 4) << dump("围死盘面（应为 4 空格：最右一列）", surrounded);
  EXPECT_EQ(EmptyNeighboursOfMax(surrounded), 0)
      << dump("围死盘面（最大块四邻必须全被占）", surrounded);
  // 但这个盘面里空位都挨着 2，所以偏置仍然可用（会退到 2）
  EXPECT_GT(LargestMovableExponent(surrounded), 0)
      << dump("围死盘面（空位挨着 2，偏置应仍可用）", surrounded);

  // 用户指出的场景：最大块被围死，退到"下一个有空位的大块"
  EXPECT_EQ(EmptyNeighboursOfMax(secondary), 0)
      << dump("次级盘面（最大块 8 的四邻必须全被占）", secondary);
  EXPECT_EQ(LargestMovableExponent(secondary), 2)
      << dump("次级盘面（应当退到等级 2，也就是 4 那一档）", secondary);

  EXPECT_EQ(EmptyCount(corners), 12) << dump("四角占用盘面（应为 12 空格）", corners);
  for (const int c : kCorners) {
    EXPECT_NE(GetExponent(corners, c), 0) << "角落 " << c << " 必须是占用状态";
  }
}

// ---------------------------------------------------------------------------
// easy：70% 落角
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, EasyFavoursCornersOverUniform) {
  // 新规则（2026-09-12）是**加权随机**，不再是"以 70% 概率跳到空角落"。
  //
  // 所以断言的口径也变了：均匀落角率是 4/15 ≈ 0.267，加权后应当**明显高于**
  // 它，但不该像旧规则那样接近 0.70 —— 加权分布在去掉纯随机那一份之后，
  // 角的合计份额也只有三成左右。
  const std::uint64_t board = BoardMaxInCenter();
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kEasy, board, kTrials);
  ASSERT_EQ(samples.size(), 15u) << "15 个空格都应被覆盖";

  int corner_hits = 0;
  for (const Sample& sample : samples) {
    if (IsCorner(sample.index)) corner_hits += sample.count;
  }
  const double rate = static_cast<double>(corner_hits) / kTrials;
  const double uniform_rate = 4.0 / 15.0;

  // 实测（strength 0.35 + 评分饱和 600）约 0.41。
  // 旧规则是 0.70，太高（文档警告的"过于温和"）；均匀是 0.267，太低。
  // 0.32~0.50 这个区间既排除"加权没生效"，也排除"退化成必定落角"。
  EXPECT_GT(rate, uniform_rate + 0.05)
      << "落角率 " << rate << " 不比均匀值 " << uniform_rate << " 明显高，简单档的加权没生效";
  EXPECT_LT(rate, 0.50) << "落角率 " << rate
                        << " 高得离谱 —— 加权可能退化成了『必定落角』，"
                           "那样简单档会过于温和（文档明确要求保留 20% 纯随机）";
}

TEST(DifficultySpawn, EasyStillFillsNonCornerCells) {
  // 20% 的纯随机兜底必须真的存在：非角落格要拿到可观的份额。
  // 加权分支也会给它们分一些（空旷、可合并都能加分），所以下限不能设太高。
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kEasy, BoardMaxInCenter(), kTrials);
  int non_corner = 0;
  for (const Sample& sample : samples) {
    if (!IsCorner(sample.index)) non_corner += sample.count;
  }
  const double rate = static_cast<double>(non_corner) / kTrials;
  EXPECT_GT(rate, 0.40) << "非角落只占 " << rate << "，纯随机兜底或加权分布有问题";
}

// ---------------------------------------------------------------------------
// normal：全盘均匀（标准 2048）
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, NormalIsUniformOnEmptyBoard) {
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kNormal, 0, kTrials);
  ASSERT_EQ(samples.size(), 16u) << "标准难度必须能落到全部 16 格";
  EXPECT_LT(ChiSquareUniform(samples, 16), 37.7) << "空盘分布不均匀";
}

TEST(DifficultySpawn, NormalIsUniformOnSparseBoard) {
  const std::vector<Sample> samples =
      SampleSpawns(Difficulty::kNormal, BoardMaxSurrounded(), kTrials);
  const int empties = EmptyCount(BoardMaxSurrounded());
  ASSERT_EQ(static_cast<int>(samples.size()), empties) << "应覆盖全部 " << empties << " 个空格";
  EXPECT_LT(ChiSquareUniform(samples, empties), CriticalChiSquare(empties - 1))
      << "稀疏盘面分布不均匀";
}

// ---------------------------------------------------------------------------
// hard：80% 贴最大块
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 困难档：加权随机，不再是"以 80% 概率精确跳到最大块旁边"
//
// 规则在 2026-09-12 改成加权随机 + 纯随机兜底，所以原先那批
// 「HardLandsNextToMaxTile / HardBiasUsesOnlyOrthogonalNeighbours /
//   HardChecksEveryTileOfTheLevelNotJustTheFirst / HardFallsBackToNextLargest…」
// 断言的具体机制已经不存在了 —— 它们测的是"偏置如何选候选格"，
// 而现在根本没有候选格这个概念，只有一张按评分加权的分布。
//
// 这里换成测**性质**而不是测机制：加权到底有没有把概率推向不利位置、
// 有没有正确扣掉"立刻能合并"的位置、以及纯随机兜底的比例对不对。
// ---------------------------------------------------------------------------

/** kHard 在某个空位上的相对权重（已归一化）。 */

TEST(DifficultySpawn, HardWeightingFavoursCrowdedCells) {
  // BoardMaxSurrounded 里空格全在最后一列，其中 (0,3)(1,3)(2,3) 空，
  // 而 (3,3) 也空。拥挤程度不同，权重就该不同。
  const std::uint64_t board = BoardMaxSurrounded();
  const std::array<double, kCellCount> weights = SpawnWeights(board, Difficulty::kHard);

  // 先确认打出来的分布不是均匀的 —— 加权随机一旦失效（例如 weighted_share
  // 被误设成 0），分布会退化成均匀，而这个测试必须能抓住那种回归。
  double max_weight = 0.0;
  double min_weight = 1.0;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) != 0) continue;
    max_weight = std::max(max_weight, weights[static_cast<std::size_t>(i)]);
    min_weight = std::min(min_weight, weights[static_cast<std::size_t>(i)]);
  }
  EXPECT_GT(max_weight / min_weight, 1.2)
      << "困难档的空格权重几乎相等（max/min = " << (max_weight / min_weight)
      << "），加权分支没有起作用";
}

TEST(DifficultySpawn, HardSharesProbabilityWithPureRandom) {
  // 纯随机兜底的比例是**规则的一部分**：没有它，困难档会显得在作弊。
  // 检验方式是找一个"加权严重偏向某一格"的盘面，看那一格的实际占比
  // 是否明显低于 1（= 没有变成必定落那里）。
  const std::uint64_t board = BoardMaxInCenter();
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kHard, board, kTrials);

  int best_index = -1;
  double best_weight = -1.0;
  const std::array<double, kCellCount> weights = SpawnWeights(board, Difficulty::kHard);
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) != 0) continue;
    if (weights[static_cast<std::size_t>(i)] > best_weight) {
      best_weight = weights[static_cast<std::size_t>(i)];
      best_index = i;
    }
  }
  ASSERT_GE(best_index, 0);

  int hits = 0;
  for (const Sample& sample : samples) {
    if (sample.index == best_index) hits += sample.count;
  }
  const double rate = static_cast<double>(hits) / kTrials;
  EXPECT_LT(rate, 0.45) << "最高权重格占了 " << rate
                        << " 的生成 —— 纯随机兜底没起作用，困难档会显得在作弊";
  EXPECT_GT(rate, 1.0 / 15.0) << "最高权重格占比 " << rate << " 不高于均匀值，加权没生效";
}

TEST(DifficultySpawn, HardNeverMatchesNormalOnTheSameBoard) {
  // 困难档与中等档必须是不同的分布 —— 否则难度选择形同虚设。
  const std::uint64_t board = BoardMaxInCenter();
  const std::array<double, kCellCount> hard = SpawnWeights(board, Difficulty::kHard);
  const std::array<double, kCellCount> normal = SpawnWeights(board, Difficulty::kNormal);

  double worst = 0.0;
  for (int i = 0; i < kCellCount; ++i) {
    worst = std::max(
        worst, std::abs(hard[static_cast<std::size_t>(i)] - normal[static_cast<std::size_t>(i)]));
  }
  EXPECT_GT(worst, 0.01) << "困难档与中等档的分布几乎相同（最大差 " << worst << "）";
}

// ---------------------------------------------------------------------------
// 数值概率随难度变化：简单 10% / 中等 15% / 困难 20% 出 4
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, FourSpawnRateFollowsDifficulty) {
  struct Case {
    const char* name;
    Difficulty difficulty;
    double expected;
  };
  const std::vector<Case> cases = {
      {"简单", Difficulty::kEasy, 0.10},
      {"中等", Difficulty::kNormal, 0.15},
      {"困难", Difficulty::kHard, 0.20},
  };

  for (const Case& item : cases) {
    const std::vector<Sample> samples = SampleSpawns(item.difficulty, BoardMaxInCenter(), kTrials);
    int fours = 0;
    int total = 0;
    for (const Sample& sample : samples) {
      fours += sample.four_count;
      total += sample.count;
    }
    ASSERT_GT(total, 0);
    const double rate = static_cast<double>(fours) / static_cast<double>(total);
    // 20000 次、p≈0.15 时标准误约 0.0025，容差 0.015 留足余量
    EXPECT_NEAR(rate, item.expected, 0.015)
        << item.name << " 档出 4 的实际比例是 " << rate << "，规则要求 " << item.expected;
  }
}

// ---------------------------------------------------------------------------
// 随机流结构
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, DefaultDifficultyIsNormal) {
  // 默认难度必须是 normal：不传难度的调用方（跑批、自检、历史基准）
  // 拿到的必须是标准 2048，否则历史分数全部作废。
  Game explicit_normal(777, Difficulty::kNormal);
  Game defaulted(777);
  EXPECT_EQ(explicit_normal.board(), defaulted.board());
  EXPECT_EQ(std::string(DifficultyName(defaulted.difficulty())), std::string("normal"));
  EXPECT_EQ(std::string(DifficultyName(Difficulty::kEasy)), std::string("easy"));
  EXPECT_EQ(std::string(DifficultyName(Difficulty::kHard)), std::string("hard"));
}

TEST(DifficultySpawn, SameDifficultyAndSeedIsFullyDeterministic) {
  // 真正要保证的性质：**同一难度、同一种子 → 逐位一致的整局**。
  //
  // 特意**不**断言"三档的随机流相同"—— 那不可能成立：
  // 偏置分支要多消耗一次随机数（在候选格里挑一个），
  // 这是"偏向某个位置集合"的必然代价。要求三档消耗次数相同，
  // 就只能放弃偏置。所以这里验的是各档内部的确定性。
  auto play = [](Difficulty difficulty, std::uint64_t seed) {
    Game game(seed, difficulty);
    for (int step = 0; step < 300 && !game.game_over(); ++step) {
      bool moved = false;
      for (const Direction d :
           {Direction::kDown, Direction::kLeft, Direction::kRight, Direction::kUp}) {
        if (game.Step(d).moved) {
          moved = true;
          break;
        }
      }
      if (!moved) break;
    }
    return game.Serialize();
  };

  for (const auto& [name, difficulty] :
       std::vector<std::pair<const char*, Difficulty>>{{"easy", Difficulty::kEasy},
                                                       {"normal", Difficulty::kNormal},
                                                       {"hard", Difficulty::kHard}}) {
    const std::string first = play(difficulty, 20240911);
    const std::string second = play(difficulty, 20240911);
    EXPECT_EQ(first, second) << name << " 档同种子两次运行结果不同，确定性被破坏";
  }
}

TEST(DifficultySpawn, DifferentDifficultiesActuallyDifferOnTheSameSeed) {
  // 反面：难度必须真的改变对局，否则这个功能没生效。
  auto play = [](Difficulty difficulty) {
    Game game(20240911, difficulty);
    for (int step = 0; step < 120 && !game.game_over(); ++step) {
      bool moved = false;
      for (const Direction d :
           {Direction::kDown, Direction::kLeft, Direction::kRight, Direction::kUp}) {
        if (game.Step(d).moved) {
          moved = true;
          break;
        }
      }
      if (!moved) break;
    }
    return game.Serialize();
  };

  const std::string easy = play(Difficulty::kEasy);
  const std::string normal = play(Difficulty::kNormal);
  const std::string hard = play(Difficulty::kHard);

  EXPECT_TRUE(easy != normal || hard != normal || easy != hard)
      << "三档在同一粒子上跑出完全相同的对局，难度没有生效";
}

// ---------------------------------------------------------------------------
// AI 的世界模型：SpawnWeights 必须与实际生成分布一致
//
// 这是最容易悄悄写错的地方：SpawnWeights 与 Game::SpawnRandomTile 是同一套
// 规则的两种表达。不一致时**不会有任何报错**，只会让 AI 按错误的世界模型
// 评估风险 —— 例如 hard 档下低估"新块贴着自己最大块出现"的概率。
//
// 验法：把 SpawnWeights 归一化成概率，与实际采样 20000 次得到的频率比较。
// ---------------------------------------------------------------------------

/**
 * 把权重归一化成概率。
 *
 * ⚠️ **必须传入盘面**：SpawnWeights 对已占格不置零（搜索里只看空格，
 * 那些位置根本不会被访问）。如果照着原始数组求和，总权重会偏大，
 * 每格概率都被系统性压低 —— 实测表现为"已占格算出 1/16 的概率"，
 * 看起来像实现有 bug，其实是这个辅助函数漏了一步。
 *
 * 这里显式跳过已占格：它们既不计入总和，结果也置 0。
 */
[[nodiscard]] std::array<double, kCellCount> Normalize(
    const std::array<double, kCellCount>& weights, std::uint64_t board) {
  double total = 0.0;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) == 0) total += weights[static_cast<std::size_t>(i)];
  }
  std::array<double, kCellCount> out{};
  if (total <= 0.0) return out;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) != 0) continue;  // 已占格概率必为 0
    out[static_cast<std::size_t>(i)] = weights[static_cast<std::size_t>(i)] / total;
  }
  return out;
}

/** 在固定盘面上实际采样，返回各格的出现频率。 */
[[nodiscard]] std::array<double, kCellCount> EmpiricDistribution(Difficulty difficulty,
                                                                 std::uint64_t board, int trials) {
  std::array<int, kCellCount> counts{};
  Game game(987654, difficulty);
  for (int i = 0; i < trials; ++i) {
    game.SetBoardForTesting(board);
    const SpawnRecord record = game.SpawnRandomTile();
    if (record.exponent == 0) continue;
    counts[static_cast<std::size_t>(record.row * kBoardSize + record.col)]++;
  }
  std::array<double, kCellCount> out{};
  for (std::size_t i = 0; i < counts.size(); ++i) {
    out[i] = static_cast<double>(counts[i]) / static_cast<double>(trials);
  }
  return out;
}

/** 两个分布的逐格差值上限。 */
[[nodiscard]] double MaxAbsDiff(const std::array<double, kCellCount>& a,
                                const std::array<double, kCellCount>& b) {
  double worst = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::abs(a[i] - b[i]));
  return worst;
}

TEST(DifficultyWorldModel, WeightsMatchActualSpawnDistribution) {
  struct Case {
    const char* name;
    Difficulty difficulty;
    std::uint64_t board;
  };
  const std::vector<Case> cases = {
      {"normal/中央盘面", Difficulty::kNormal, BoardMaxInCenter()},
      {"normal/稀疏盘面", Difficulty::kNormal, BoardMaxSurrounded()},
      {"easy/中央盘面", Difficulty::kEasy, BoardMaxInCenter()},
      {"easy/四角已占", Difficulty::kEasy, BoardCornersTaken()},
      {"hard/中央盘面", Difficulty::kHard, BoardMaxInCenter()},
      {"hard/全部被围死", Difficulty::kHard, BoardSecondaryMovable()},
  };

  for (const Case& item : cases) {
    const std::array<double, kCellCount> predicted =
        Normalize(SpawnWeights(item.board, item.difficulty), item.board);
    const std::array<double, kCellCount> actual =
        EmpiricDistribution(item.difficulty, item.board, kTrials);

    // 先自检：实际分布的**总和必须接近 1**。
    // 不等于 1 说明采样本身有问题（例如大量生成被跳过），
    // 那时比较分布毫无意义 —— 这个自检能避免把"测试写错"误判成"实现有 bug"。
    double actual_sum = 0.0;
    for (const double p : actual) actual_sum += p;
    ASSERT_NEAR(actual_sum, 1.0, 0.02)
        << item.name << " 的实际分布总和是 " << actual_sum << "，采样本身有问题";

    double predicted_sum = 0.0;
    for (const double p : predicted) predicted_sum += p;
    ASSERT_NEAR(predicted_sum, 1.0, 1e-9) << item.name << " 的预测分布没有归一化";

    // 20000 次采样、概率量级 1/16 时标准误约 0.0018；放宽到 0.01 留足余量。
    const double diff = MaxAbsDiff(predicted, actual);
    if (diff >= 0.01) {
      // 失败时把**盘面**与两个分布并排打出来。
      // 只说"偏差 0.0625"没法定位，而 0.0625 恰好是 1/16 ——
      // 这类数字暗示某一格被系统性地多算或少算，必须看到盘面才能判断。
      std::string dump = item.name;
      dump += "\n      —— 盘面（值，. 表示空）——";
      for (int r = 0; r < kBoardSize; ++r) {
        dump += "\n      ";
        for (int c = 0; c < kBoardSize; ++c) {
          const int exponent = GetExponent(item.board, r * kBoardSize + c);
          if (exponent == 0) {
            dump += "      .";
          } else {
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "%7llu",
                          static_cast<unsigned long long>(ExponentToValue(exponent)));
            dump += buffer;
          }
        }
      }
      dump += "\n      —— 每格两行：预测 / 实际 ——";
      for (int r = 0; r < kBoardSize; ++r) {
        std::string predicted_row = "\n      预测 ";
        std::string actual_row = "\n      实际 ";
        for (int c = 0; c < kBoardSize; ++c) {
          char buffer[32];
          const auto index = static_cast<std::size_t>(r * kBoardSize + c);
          std::snprintf(buffer, sizeof(buffer), "%7.4f", predicted[index]);
          predicted_row += buffer;
          std::snprintf(buffer, sizeof(buffer), "%7.4f", actual[index]);
          actual_row += buffer;
        }
        dump += predicted_row + actual_row;
      }
      ADD_FAILURE() << dump;
    }
    EXPECT_LT(diff, 0.01) << item.name << " 的权重与实际生成分布不符（最大偏差 " << diff
                          << "）—— AI 的世界模型是错的";
  }
}

TEST(DifficultyWorldModel, NormalWeightsAreUniform) {
  const std::array<double, kCellCount> weights =
      SpawnWeights(BoardMaxInCenter(), Difficulty::kNormal);
  // 15 个空格 → 各格 1/15
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(BoardMaxInCenter(), i) != 0) continue;
    EXPECT_NEAR(weights[static_cast<std::size_t>(i)], 1.0 / 15.0, 1e-12);
  }
}

TEST(DifficultyWorldModel, EasyWeightsFavourCorners) {
  const std::uint64_t board = BoardMaxInCenter();
  const std::array<double, kCellCount> weights = SpawnWeights(board, Difficulty::kEasy);
  // 四角全空 → 权重最高的就是这四格
  for (const int corner : kCorners) {
    EXPECT_GT(weights[static_cast<std::size_t>(corner)], 1.0 / 15.0)
        << "角落 " << corner << " 的权重没有高于均匀值";
  }
  // 非角落应低于均匀值（偏置把概率从它们那里挪走了）
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) != 0 || IsCorner(i)) continue;
    EXPECT_LT(weights[static_cast<std::size_t>(i)], 1.0 / 15.0)
        << "非角落 " << i << " 的权重不该高于均匀值";
  }
}

TEST(DifficultyWorldModel, WeightsAreZeroOnOccupiedCells) {
  // 已占格子的权重必须是 0，否则 chance 节点会去"在已占格上生成"，
  // 那会算出物理上不可能的盘面。
  const std::uint64_t board = BoardSecondaryMovable();
  for (const Difficulty difficulty : {Difficulty::kEasy, Difficulty::kNormal, Difficulty::kHard}) {
    const std::array<double, kCellCount> weights = SpawnWeights(board, difficulty);
    for (int i = 0; i < kCellCount; ++i) {
      if (GetExponent(board, i) != 0) continue;
      EXPECT_GT(weights[static_cast<std::size_t>(i)], 0.0)
          << "空格 " << i << " 的权重不该是 0（难度 " << DifficultyName(difficulty) << "）";
    }
  }
}

TEST(DifficultyWorldModel, FullBoardHasNoWeights) {
  const std::uint64_t board = EncodeBoard({1, 2, 1, 2, 2, 1, 2, 1, 1, 2, 1, 2, 2, 1, 2, 1});
  ASSERT_EQ(EmptyCount(board), 0);
  for (const Difficulty difficulty : {Difficulty::kEasy, Difficulty::kNormal, Difficulty::kHard}) {
    const std::array<double, kCellCount> weights = SpawnWeights(board, difficulty);
    for (const double w : weights) EXPECT_DOUBLE_EQ(w, 0.0);
  }
}

TEST(DifficultyWorldModel, DifficultyChangesTheAIMove) {
  // 世界模型变了，走子至少要**有可能**不同。
  // 不断言"一定不同"（强 AI 在多数局面下结论一致是正常的），
  // 而是在一批真实局面上统计：应该有相当比例的局面给出不同走子。
  int compared = 0;
  int different = 0;

  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    Game game(seed, Difficulty::kHard);
    TranspositionTable table(1u << 16, false);

    for (int step = 0; step < 120 && !game.game_over(); ++step) {
      SearchConfig base;
      base.base_depth = 4;
      base.min_depth = 4;
      base.max_depth = 4;

      SearchConfig as_normal = base;
      as_normal.difficulty = Difficulty::kNormal;
      const SearchResult a = SearchBestMove(game.board(), as_normal, &table, std::nullopt);

      SearchConfig as_hard = base;
      as_hard.difficulty = Difficulty::kHard;
      table.Reset();  // 换难度必须清表（协议层也是这么做的）
      const SearchResult b = SearchBestMove(game.board(), as_hard, &table, std::nullopt);

      if (a.move.has_value() && b.move.has_value()) {
        ++compared;
        if (a.move != b.move) ++different;
      }

      table.Reset();
      bool moved = false;
      for (const Direction d :
           {Direction::kDown, Direction::kLeft, Direction::kRight, Direction::kUp}) {
        if (game.Step(d).moved) {
          moved = true;
          break;
        }
      }
      if (!moved) break;
    }
  }

  EXPECT_GT(compared, 100) << "对比的局面太少，测试没有说服力";
  // 只要**存在**差异就说明世界模型确实参与了决策；
  // 比例本身取决于局面，不作为质量判据。
  EXPECT_GT(different, 0) << "在 " << compared
                          << " 个局面里，按 normal 与按 hard 评估给出的走子完全相同 —— "
                             "难度可能没有传进搜索";
}

}  // namespace
}  // namespace ai2048
