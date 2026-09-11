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
#include <map>
#include <string>
#include <utility>
#include <vector>

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

/** (row, col) 是否与 (max_row, max_col) **上下左右**相邻（不含斜角）。 */
[[nodiscard]] bool IsAdjacent(int row, int col, int max_row, int max_col) {
  return (row == max_row && (col == max_col - 1 || col == max_col + 1)) ||
         (col == max_col && (row == max_row - 1 || row == max_row + 1));
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
  Game game(20240911, difficulty);
  for (int i = 0; i < trials; ++i) {
    game.SetBoardForTesting(board);
    const SpawnRecord record = game.SpawnRandomTile();
    if (record.exponent == 0) continue;  // 盘面满了：调用方会先断言
    counts[static_cast<std::size_t>(record.row * kBoardSize + record.col)]++;
  }

  std::vector<Sample> samples;
  for (int i = 0; i < kCellCount; ++i) {
    if (counts[static_cast<std::size_t>(i)] > 0) {
      samples.push_back({i, counts[static_cast<std::size_t>(i)]});
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
 * 最大块四邻全被占 → 两档的偏置都"完全不可用"。
 */
[[nodiscard]] std::uint64_t BoardMaxSurrounded() {
  return EncodeBoard({4, 2, 4, 0, 2, 3, 4, 0, 4, 2, 4, 0, 2, 4, 2, 0});
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
  const std::uint64_t corners = BoardCornersTaken();

  EXPECT_EQ(EmptyCount(center), 15) << dump("中央盘面（应只有 1 个块、15 空格）", center);
  EXPECT_EQ(EmptyNeighboursOfMax(center), 4) << dump("中央盘面（最大块四邻应全空）", center);

  // 围死盘面：1 列空 → 恰好 4 个空格；最大块四邻全被占
  EXPECT_EQ(EmptyCount(surrounded), 4) << dump("围死盘面（应为 4 空格：最右一列）", surrounded);
  EXPECT_EQ(EmptyNeighboursOfMax(surrounded), 0)
      << dump("围死盘面（最大块四邻必须全被占）", surrounded);

  EXPECT_EQ(EmptyCount(corners), 12) << dump("四角占用盘面（应为 12 空格）", corners);
  for (const int c : kCorners) {
    EXPECT_NE(GetExponent(corners, c), 0) << "角落 " << c << " 必须是占用状态";
  }
}

// ---------------------------------------------------------------------------
// easy：70% 落角
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, EasyLandsInCorner) {
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kEasy, BoardMaxInCenter(), kTrials);
  ASSERT_EQ(samples.size(), 15u) << "15 个空格都应被覆盖";

  int corner_hits = 0;
  for (const Sample& sample : samples) {
    if (IsCorner(sample.index)) corner_hits += sample.count;
  }
  const double rate = static_cast<double>(corner_hits) / kTrials;

  // 规格的准确含义：偏置分支 70%，未命中时走全盘均匀，那 30% 里仍有
  // 4/15 的概率落在角上 → 观测值应略高于 70%，但不能高到像"必定落角"。
  EXPECT_GT(rate, 0.69) << "落角率 " << rate << " 低于 70%，偏置没生效";
  EXPECT_LT(rate, 0.85) << "落角率 " << rate << " 过高，可能退化成了必定落角";
}

TEST(DifficultySpawn, EasyStillFillsNonCornerCells) {
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kEasy, BoardMaxInCenter(), kTrials);
  int non_corner = 0;
  for (const Sample& sample : samples) {
    if (!IsCorner(sample.index)) non_corner += sample.count;
  }
  const double rate = static_cast<double>(non_corner) / kTrials;
  // 30% 的随机分支 × 11/15 的落点 ≈ 22%，不能接近 0
  EXPECT_GT(rate, 0.15) << "非角落只占 " << rate << "，30% 的随机分支没生效";
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

TEST(DifficultySpawn, HardLandsNextToMaxTile) {
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kHard, BoardMaxInCenter(), kTrials);
  const MaxCell max_cell = FirstMaxCell(BoardMaxInCenter());
  ASSERT_TRUE(max_cell.found);

  int hits = 0;
  for (const Sample& sample : samples) {
    const int row = sample.index / kBoardSize;
    const int col = sample.index % kBoardSize;
    if (IsAdjacent(row, col, max_cell.row, max_cell.col)) hits += sample.count;
  }
  const double rate = static_cast<double>(hits) / kTrials;
  EXPECT_GT(rate, 0.79) << "贴最大块率 " << rate << " 低于 80%，偏置没生效";
  EXPECT_LT(rate, 0.95) << "贴最大块率 " << rate << " 过高，可能退化成了必定贴块";
}

TEST(DifficultySpawn, HardBiasUsesOnlyOrthogonalNeighbours) {
  // 若把斜角也算作"附近"，落角率会异常升高（四角里的斜角更多）。
  // 这里用"对角四格"的占比做反证：它们不该被偏置照顾。
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kHard, BoardMaxInCenter(), kTrials);
  const MaxCell max_cell = FirstMaxCell(BoardMaxInCenter());

  int diagonal = 0;
  for (const Sample& sample : samples) {
    const int row = sample.index / kBoardSize;
    const int col = sample.index % kBoardSize;
    const bool is_diagonal =
        (std::abs(row - max_cell.row) == 1 && std::abs(col - max_cell.col) == 1);
    if (is_diagonal) diagonal += sample.count;
  }
  // 均匀时对角 4 格占 4/16 = 25%；偏置只照顾正邻，所以对角应低于均匀
  const double rate = static_cast<double>(diagonal) / kTrials;
  EXPECT_LT(rate, 0.20) << "对角格占 " << rate << "，偏置可能把斜角也算进去了";
}

// ---------------------------------------------------------------------------
// 偏置不可用时的退路
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, EasyFallsBackWhenAllCornersOccupied) {
  const std::vector<Sample> samples = SampleSpawns(Difficulty::kEasy, BoardCornersTaken(), kTrials);
  ASSERT_EQ(samples.size(), 12u) << "四角已占，落点应覆盖其余 12 格";
  for (const Sample& sample : samples) {
    EXPECT_FALSE(IsCorner(sample.index)) << "角落已占，不该再落角";
  }
  EXPECT_LT(ChiSquareUniform(samples, 12), CriticalChiSquare(11)) << "退路应当是均匀的";
}

TEST(DifficultySpawn, HardFallsBackWhenMaxTileIsSurrounded) {
  const std::uint64_t board = BoardMaxSurrounded();
  ASSERT_EQ(EmptyNeighboursOfMax(board), 0) << "这是盘面自检：最大块四邻必须全被占";

  const std::vector<Sample> samples = SampleSpawns(Difficulty::kHard, board, kTrials);
  const int empties = EmptyCount(board);
  ASSERT_EQ(static_cast<int>(samples.size()), empties) << "应覆盖全部 " << empties << " 个空格";
  EXPECT_LT(ChiSquareUniform(samples, empties), CriticalChiSquare(empties - 1))
      << "退路应当是均匀的";
}

TEST(DifficultySpawn, HardMatchesNormalWhenBiasIsUnavailable) {
  // 偏置关闭时，hard 与 normal 必须给出**同分布**。
  // 若实现把"附近占用格"也当候选，两者就会分叉，这里抓它。
  const std::uint64_t board = BoardMaxSurrounded();
  const std::vector<Sample> hard = SampleSpawns(Difficulty::kHard, board, kTrials);
  const std::vector<Sample> normal = SampleSpawns(Difficulty::kNormal, board, kTrials);

  std::map<int, int> hard_map;
  for (const Sample& sample : hard) hard_map[sample.index] = sample.count;
  std::map<int, int> normal_map;
  for (const Sample& sample : normal) normal_map[sample.index] = sample.count;

  EXPECT_EQ(hard_map.size(), normal_map.size()) << "覆盖的格子数应一致";
  const double sigma = std::sqrt(static_cast<double>(kTrials) / 4.0);
  for (const auto& [index, count] : normal_map) {
    const auto it = hard_map.find(index);
    ASSERT_NE(it, hard_map.end()) << "hard 少了格子 " << index;
    EXPECT_LT(std::abs(static_cast<double>(it->second - count)), 6.0 * sigma)
        << "格子 " << index << " 在 hard 与 normal 下差异过大，偏置可能没关闭";
  }
}

// ---------------------------------------------------------------------------
// 取值概率不随难度变化（规格只改位置）
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, TileValuesStayAt90PercentTwos) {
  const std::uint64_t board = BoardMaxInCenter();
  for (const auto& [name, difficulty] :
       std::vector<std::pair<const char*, Difficulty>>{{"easy", Difficulty::kEasy},
                                                       {"normal", Difficulty::kNormal},
                                                       {"hard", Difficulty::kHard}}) {
    Game game(31337, difficulty);
    int fours = 0;
    for (int i = 0; i < kTrials; ++i) {
      game.SetBoardForTesting(board);
      if (game.SpawnRandomTile().exponent == 2) ++fours;
    }
    const double rate = static_cast<double>(fours) / kTrials;
    EXPECT_NEAR(rate, 0.10, 0.015) << name << " 档的 4 出现率应仍是 10%";
  }
}

// ---------------------------------------------------------------------------
// 随机流结构
// ---------------------------------------------------------------------------

TEST(DifficultySpawn, NormalMatchesLegacyBehaviour) {
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

}  // namespace
}  // namespace ai2048
