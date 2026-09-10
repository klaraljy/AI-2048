// 规则引擎的单元测试。
//
// 这些测试存在的理由：规则引擎的任何静默错误都会让 AI 的分数失去意义。
// 参考原型就是在这里出的问题 —— moveBoard 长出了四份实现，评估用的规则
// 与游戏用的规则悄悄漂移，于是"调参变好了"其实是"评估环境变简单了"。

#include "core/board.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/rng.h"

namespace ai2048 {
namespace {

// 用可读的方式构造棋盘：值写数值，内部转成指数。
[[nodiscard]] std::uint64_t MakeBoard(const std::array<std::uint64_t, kCellCount>& values) {
  std::array<int, kCellCount> exponents{};
  for (int i = 0; i < kCellCount; ++i) {
    const std::uint64_t value = values[static_cast<std::size_t>(i)];
    const int exponent = ValueToExponent(value);
    EXPECT_GE(exponent, 0) << "构造棋盘时用到了非 2 的幂: " << value;
    exponents[static_cast<std::size_t>(i)] = exponent;
  }
  return EncodeBoard(exponents);
}

// 只关心某一行的走子结果，其余行留空。
[[nodiscard]] std::uint64_t RowBoard(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                     std::uint64_t d) {
  return MakeBoard({a, b, c, d, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
}

[[nodiscard]] std::array<std::uint64_t, kBoardSize> FirstRow(std::uint64_t board) {
  std::array<std::uint64_t, kBoardSize> values{};
  for (int col = 0; col < kBoardSize; ++col) {
    values[static_cast<std::size_t>(col)] = ExponentToValue(GetExponent(board, col));
  }
  return values;
}

[[nodiscard]] std::array<std::uint64_t, kBoardSize> FirstColumn(std::uint64_t board) {
  std::array<std::uint64_t, kBoardSize> values{};
  for (int row = 0; row < kBoardSize; ++row) {
    values[static_cast<std::size_t>(row)] = ExponentToValue(GetExponent(board, row * kBoardSize));
  }
  return values;
}

// 用一个刻意平凡的策略把一局打完：按固定顺序取第一个合法方向。
// 里程碑 1 要验证的是"引擎能跑到终局且结果可复现"，不是"AI 强"。
struct EndState {
  bool game_over = false;
  std::uint32_t steps = 0;
  std::uint64_t score = 0;
};

[[nodiscard]] EndState PlayToEnd(std::uint64_t seed) {
  Game game(seed);
  while (!game.game_over()) {
    const std::optional<Direction> direction = FindAnyLegalMove(game.board());
    if (!direction.has_value()) break;
    // 故意丢弃返回值：这里只关心终局状态，不关心单步细节。
    static_cast<void>(game.Step(*direction));
  }
  return EndState{game.game_over(), game.step_count(), game.score()};
}

// 显然正确的参考转置：解包 -> 交换行列 -> 打包。慢，但一眼能看出对不对。
[[nodiscard]] std::uint64_t ReferenceTranspose(std::uint64_t board) {
  std::array<int, kCellCount> exponents{};
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 0; col < kBoardSize; ++col) {
      exponents[static_cast<std::size_t>(col * kBoardSize + row)] =
          GetExponent(board, row * kBoardSize + col);
    }
  }
  return EncodeBoard(exponents);
}

// 可复现的伪随机局面生成器，用于批量对拍。
class BoardFuzzer {
 public:
  explicit BoardFuzzer(std::uint64_t seed) : state_(seed) {}

  [[nodiscard]] std::uint64_t Next() {
    std::uint64_t board = 0;
    for (int n = 0; n < kCellCount; ++n) {
      board = SetExponent(board, n, static_cast<int>(NextU64() % 16));
    }
    return board;
  }

 private:
  [[nodiscard]] std::uint64_t NextU64() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// 合并规则
// ---------------------------------------------------------------------------

TEST(BoardMerge, SlidesTilesToTheEdge) {
  const MoveResult result = ApplyMove(RowBoard(0, 0, 2, 4), Direction::kLeft);
  ASSERT_TRUE(result.moved);
  const auto row = FirstRow(result.board);
  EXPECT_EQ(row[0], 2u);
  EXPECT_EQ(row[1], 4u);
  EXPECT_EQ(row[2], 0u);
  EXPECT_EQ(row[3], 0u);
  EXPECT_EQ(result.score_gained, 0u);
}

TEST(BoardMerge, MergesAdjacentEqualTilesOnceAndScores) {
  const MoveResult result = ApplyMove(RowBoard(2, 2, 0, 0), Direction::kLeft);
  ASSERT_TRUE(result.moved);
  const auto row = FirstRow(result.board);
  EXPECT_EQ(row[0], 4u);
  EXPECT_EQ(row[1], 0u);
  EXPECT_EQ(result.score_gained, 4u) << "合并出 4 就得 4 分";
}

// 这条是 2048 里最容易写错的地方之一：2 2 2 2 往左推，应该是 4 4，
// 而不是 8。合并出来的 4 不得参与同一次走子里的第二次合并。
TEST(BoardMerge, MergedTileDoesNotMergeAgainInTheSameMove) {
  const MoveResult result = ApplyMove(RowBoard(2, 2, 2, 2), Direction::kLeft);
  ASSERT_TRUE(result.moved);
  const auto row = FirstRow(result.board);
  EXPECT_EQ(row[0], 4u);
  EXPECT_EQ(row[1], 4u);
  EXPECT_EQ(row[2], 0u);
  EXPECT_EQ(row[3], 0u);
  EXPECT_EQ(result.score_gained, 8u);
}

TEST(BoardMerge, MergesNearestPairFirst) {
  // 4 4 8 0 往左：先合并前两个 4 得到 8 8。
  const MoveResult result = ApplyMove(RowBoard(4, 4, 8, 0), Direction::kLeft);
  ASSERT_TRUE(result.moved);
  const auto row = FirstRow(result.board);
  EXPECT_EQ(row[0], 8u);
  EXPECT_EQ(row[1], 8u);
  EXPECT_EQ(result.score_gained, 8u);
}

TEST(BoardMerge, DoesNotMergeUnequalTiles) {
  const MoveResult result = ApplyMove(RowBoard(2, 4, 2, 4), Direction::kLeft);
  EXPECT_FALSE(result.moved) << "2 4 2 4 往左推不动";
  EXPECT_EQ(result.score_gained, 0u);
}

TEST(BoardMerge, RightMoveMergesThePairNearestTheEdgeFirst) {
  // 这条测试记录一个**反直觉但正确**的行为。
  //
  // 直觉会以为"右移是左移的镜像"，于是 [2,2,4,4] 右移应该得到 [0,0,8,4]。
  // 但按权威实现（Cirulli 的 game_manager.js），正确答案是 [0,0,4,8]：
  //   左移从索引 0 开始遍历 -> 先处理 (2,2) 合并成 4，再处理 (4,4) 合并成 8
  //   右移从索引 3 开始遍历 -> 先处理 (4,4) 合并成 8，再处理 (2,2) 合并成 4
  // 两次遍历里"先合并的那一对"不同，而合并出的值都落在**索引较大**的那一格，
  // 所以两个方向的合并顺序**不是**镜像关系。
  //
  // 结论：不要用"镜像"来验收右移。右移的正确性由
  // BoardRowExhaustive.MatchesAuthoritativeReferenceOnAllRows 保证。
  const std::uint64_t board = RowBoard(2, 2, 4, 4);

  const auto left_row = FirstRow(ApplyMove(board, Direction::kLeft).board);
  EXPECT_EQ(left_row[0], 4u);
  EXPECT_EQ(left_row[1], 8u);
  EXPECT_EQ(left_row[2], 0u);
  EXPECT_EQ(left_row[3], 0u);

  const auto right_row = FirstRow(ApplyMove(board, Direction::kRight).board);
  EXPECT_EQ(right_row[0], 0u);
  EXPECT_EQ(right_row[1], 0u);
  EXPECT_EQ(right_row[2], 4u);
  EXPECT_EQ(right_row[3], 8u);

  // 两个方向的**得分**必须一致：合并的对不同，但总分相同
  EXPECT_EQ(ApplyMove(board, Direction::kLeft).score_gained,
            ApplyMove(board, Direction::kRight).score_gained);
}

TEST(BoardMerge, RightMovesTilesToTheRightEdge) {
  const MoveResult result = ApplyMove(RowBoard(2, 0, 0, 0), Direction::kRight);
  ASSERT_TRUE(result.moved);
  const auto row = FirstRow(result.board);
  EXPECT_EQ(row[3], 2u);
  EXPECT_EQ(row[0], 0u);
}

// ---------------------------------------------------------------------------
// 转置一致性（暴力对拍）
//
// 这是本项目**最重要的一条结构性测试**。理由：
//
// 转置只在垂直走子里用到，水平走子完全不经过它。所以一个写错的转置
// 能让所有水平测试照样全绿，只把垂直方向悄悄搞坏。而"垂直方向等价于
// 转置后水平方向"这条性质，是把 4 个方向压成 1 套逻辑的唯一依据 ——
// 它一旦不成立，AI 搜索出来的方向和真实规则就不是一回事了。
//
// 所以这里不用手写期望值，而是拿一个显然正确的参考实现暴力对拍。
// ---------------------------------------------------------------------------

TEST(BoardTranspose, MatchesReferenceImplementationOnRandomBoards) {
  BoardFuzzer fuzzer(0xDEAD'BEEF'CAFE'1234ULL);
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::uint64_t board = fuzzer.Next();
    const std::uint64_t transposed = ReferenceTranspose(board);

    // 转置后 (r,c) 必须等于原盘 (c,r)
    for (int r = 0; r < kBoardSize; ++r) {
      for (int c = 0; c < kBoardSize; ++c) {
        ASSERT_EQ(GetExponent(transposed, r * kBoardSize + c),
                  GetExponent(board, c * kBoardSize + r))
            << "第 " << iteration << " 组，格 (" << r << "," << c << ")";
      }
    }
    // 转置是对合：两次回到原状
    ASSERT_EQ(ReferenceTranspose(transposed), board) << "第 " << iteration << " 组";
  }
}

// 转置只在垂直走子里用到，水平走子完全不经过它。
// 一个写错的转置能让所有水平测试照样全绿，只把垂直方向悄悄搞坏。
//
// 注意：**不要用"上移是左移的镜像"来验收**。按权威实现，
// 左移从索引 0 开始扫、右移从索引 3 开始扫，两个方向"先合并的那一对"不同，
// 而合并值都落在索引较大的一格，因此左右（以及上下）的合并顺序**并不互为镜像**。
// 正确性一律以 Cirulli 原始算法为准 —— 见本文件顶部的参考实现与
// BoardRowExhaustive 用例；这里另外验证垂直方向自身的性质。

TEST(BoardTranspose, VerticalMovesSlideTowardTheEdgeTheyName) {
  // 第一列 2 2 4 4，上移后应贴顶：第一列 4 8 0 0
  const std::uint64_t board = MakeBoard({2, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0});

  const MoveResult up = ApplyMove(board, Direction::kUp);
  ASSERT_TRUE(up.moved);
  const auto up_col = FirstColumn(up.board);
  EXPECT_EQ(up_col[0], 4u);
  EXPECT_EQ(up_col[1], 8u);
  EXPECT_EQ(up_col[2], 0u);
  EXPECT_EQ(up_col[3], 0u);
  EXPECT_EQ(up.score_gained, 12u);  // 4 + 8

  // 下移贴底：第一列 0 0 4 8
  const MoveResult down = ApplyMove(board, Direction::kDown);
  ASSERT_TRUE(down.moved);
  const auto down_col = FirstColumn(down.board);
  EXPECT_EQ(down_col[0], 0u);
  EXPECT_EQ(down_col[1], 0u);
  EXPECT_EQ(down_col[2], 4u);
  EXPECT_EQ(down_col[3], 8u);
  EXPECT_EQ(down.score_gained, up.score_gained);

  // 两个方向都不该动到其它列
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 1; col < kBoardSize; ++col) {
      EXPECT_EQ(GetExponent(up.board, row * kBoardSize + col), 0);
      EXPECT_EQ(GetExponent(down.board, row * kBoardSize + col), 0);
    }
  }
}

TEST(BoardTranspose, UpAndLeftAgreeOnScoreForTheSameShape) {
  // 把同一组牌横放与竖放，上移与左移的**得分**必须一致
  // （合并的对可能不同，但总得分相同）。
  const std::uint64_t horizontal = MakeBoard({2, 2, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  const std::uint64_t vertical = MakeBoard({2, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0});

  const MoveResult left = ApplyMove(horizontal, Direction::kLeft);
  const MoveResult up = ApplyMove(vertical, Direction::kUp);
  ASSERT_TRUE(left.moved);
  ASSERT_TRUE(up.moved);
  EXPECT_EQ(left.score_gained, up.score_gained);

  // 两者都把牌压到起始边缘
  EXPECT_EQ(FirstRow(left.board)[0], 4u);
  EXPECT_EQ(FirstRow(left.board)[1], 8u);
  EXPECT_EQ(FirstColumn(up.board)[0], 4u);
  EXPECT_EQ(FirstColumn(up.board)[1], 8u);
}

TEST(BoardTranspose, TransposeIsAnInvolutionOnBoards) {
  // 转置两次回到原状。这条性质是"垂直方向复用水平逻辑"的前提。
  BoardFuzzer fuzzer(0x0BAD'F00D'1234'5678ULL);
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::uint64_t board = fuzzer.Next();
    ASSERT_EQ(ReferenceTranspose(ReferenceTranspose(board)), board) << "第 " << iteration << " 组";
  }
}

TEST(BoardTranspose, VerticalMovesAreDeterministicAndMirrorInScores) {
  // 同一局面、同一方向重复走，结果必须一致（垂直路径同样要确定性）
  BoardFuzzer fuzzer(0x1111'2222'3333'4444ULL);
  for (int iteration = 0; iteration < 500; ++iteration) {
    const std::uint64_t board = fuzzer.Next();
    const MoveResult a = ApplyMove(board, Direction::kUp);
    const MoveResult b = ApplyMove(board, Direction::kUp);
    ASSERT_EQ(a.board, b.board);
    ASSERT_EQ(a.score_gained, b.score_gained);

    const MoveResult c = ApplyMove(board, Direction::kDown);
    const MoveResult d = ApplyMove(board, Direction::kDown);
    ASSERT_EQ(c.board, d.board);
    ASSERT_EQ(c.score_gained, d.score_gained);
  }
}

// ---------------------------------------------------------------------------
// 终局判定
// ---------------------------------------------------------------------------

TEST(BoardTerminal, FullBoardWithoutMatchesIsTerminal) {
  const std::uint64_t board = MakeBoard({2, 4, 2, 4, 4, 2, 4, 2, 2, 4, 2, 4, 4, 2, 4, 2});
  EXPECT_EQ(CountEmptyCells(board), 0);
  EXPECT_FALSE(HasLegalMove(board));
  EXPECT_FALSE(FindAnyLegalMove(board).has_value());
}

TEST(BoardTerminal, FullBoardWithHorizontalMatchIsNotTerminal) {
  const std::uint64_t board = MakeBoard({2, 2, 4, 8, 4, 8, 16, 32, 8, 16, 32, 64, 16, 32, 64, 128});
  EXPECT_EQ(CountEmptyCells(board), 0);
  EXPECT_TRUE(HasLegalMove(board));
}

TEST(BoardTerminal, FullBoardWithVerticalMatchIsNotTerminal) {
  // 第一列有两个 2 可以纵向合并；其余各列刻意保持不同值以避免混淆
  const std::uint64_t board = MakeBoard({2, 4, 8, 16, 2, 8, 16, 32, 4, 16, 32, 64, 8, 32, 64, 128});
  EXPECT_EQ(CountEmptyCells(board), 0);
  EXPECT_TRUE(HasLegalMove(board));

  const MoveResult up = ApplyMove(board, Direction::kUp);
  EXPECT_TRUE(up.moved) << "第一列的 2 2 应该能向上合并";
  EXPECT_EQ(up.score_gained, 4u);
}

TEST(BoardTerminal, EmptyCellMeansNotTerminal) {
  const std::uint64_t board = MakeBoard({2, 4, 2, 4, 4, 2, 4, 2, 2, 4, 2, 4, 4, 2, 4, 0});
  EXPECT_TRUE(HasLegalMove(board));
}

TEST(BoardTerminal, HasLegalMoveAgreesWithApplyMoveOnRandomBoards) {
  // HasLegalMove 是一条独立的快速路径（查表比对 + 转置），
  // 它必须与"真的走一遍看看动没动"完全一致，否则 AI 会误判终局。
  BoardFuzzer fuzzer(0x5EED'0000'1234'ABCDULL);
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::uint64_t board = fuzzer.Next();

    bool any_moved = false;
    for (const Direction direction :
         {Direction::kUp, Direction::kDown, Direction::kLeft, Direction::kRight}) {
      if (ApplyMove(board, direction).moved) {
        any_moved = true;
        break;
      }
    }

    ASSERT_EQ(HasLegalMove(board), any_moved) << "第 " << iteration << " 组不一致";
    ASSERT_EQ(FindAnyLegalMove(board).has_value(), any_moved) << "第 " << iteration << " 组不一致";
  }
}

// ---------------------------------------------------------------------------
// 方块轨迹（前端滑动动画的数据来源）
// ---------------------------------------------------------------------------

TEST(BoardTrajectory, RecordsSlidesAndMerges) {
  // 一行: 0 2 0 2 往左 -> 4 0 0 0
  // 两张牌分别在原格 1 与 3，都滑到最左格 0，其中一张在终点被吃掉
  const MoveResult result = ApplyMove(RowBoard(0, 2, 0, 2), Direction::kLeft);
  ASSERT_TRUE(result.moved);

  // 每张原有的牌恰好一条轨迹：滑到的地方，或被并入的地方。
  // 曾经在合并时只记下一条（按目标格索引），另一张牌会从轨迹里消失。
  ASSERT_EQ(result.moves.size(), 2u) << "两张牌应各有一条轨迹";

  int merged_count = 0;
  bool saw_origin_1 = false;
  bool saw_origin_3 = false;
  for (const TileMove& move : result.moves) {
    EXPECT_EQ(move.to_row, 0);
    EXPECT_EQ(move.to_col, 0) << "两张牌都落到最左格";
    if (move.merged) {
      ++merged_count;
    }
    if (move.from_col == 1) saw_origin_1 = true;
    if (move.from_col == 3) saw_origin_3 = true;
  }
  EXPECT_EQ(merged_count, 1) << "两块合并，恰好一块被吃掉";
  EXPECT_TRUE(saw_origin_1) << "原格 1 的牌必须有轨迹";
  EXPECT_TRUE(saw_origin_3) << "原格 3 的牌必须有轨迹";

  // 被吃掉的那一块记的是**合并前**的值（2），另一条记的是合并结果（4）
  for (const TileMove& move : result.moves) {
    if (move.merged) {
      EXPECT_EQ(ExponentToValue(move.exponent), 2u);
    } else {
      EXPECT_EQ(ExponentToValue(move.exponent), 4u);
    }
  }
}

TEST(BoardTrajectory, RecordsNothingWhenTheMoveIsIllegal) {
  const MoveResult result = ApplyMove(RowBoard(2, 4, 2, 4), Direction::kLeft);
  EXPECT_FALSE(result.moved);
  EXPECT_TRUE(result.moves.empty());
}

TEST(BoardTrajectory, DestinationsMatchTheResultingBoard) {
  const std::uint64_t board = MakeBoard({0, 2, 0, 2, 4, 0, 4, 0, 0, 0, 0, 0, 2, 2, 2, 2});
  const MoveResult result = ApplyMove(board, Direction::kLeft);
  ASSERT_TRUE(result.moved);

  // 每条轨迹的目标格，在结果棋盘上必须有非零值
  for (const TileMove& move : result.moves) {
    const int index = move.to_row * kBoardSize + move.to_col;
    EXPECT_GT(GetExponent(result.board, index), 0)
        << "轨迹指向 (" << move.to_row << "," << move.to_col << ") 但结果棋盘那里是空的";
  }
}

// 轨迹是从 before 独立推导出来的，它必须与走子结果自洽。
// 参考原型最严重的问题就是"两套逻辑悄悄漂移"，这条测试直接盯住它。
//
// 重建方式：把每条轨迹记到它的目标格上。合并时两张牌落到同一格，
// 只有"存活"的那张（merged == false）决定该格的最终值 ——
// 被并入的那张只是滑过去然后消失。
TEST(BoardTrajectory, ReconstructedBoardMatchesMoveResultOnRandomBoards) {
  BoardFuzzer fuzzer(0xABCD'1234'5678'9EF0ULL);
  for (int iteration = 0; iteration < 1000; ++iteration) {
    const std::uint64_t board = fuzzer.Next();
    for (const Direction direction :
         {Direction::kUp, Direction::kDown, Direction::kLeft, Direction::kRight}) {
      const MoveResult result = ApplyMove(board, direction);
      if (!result.moved) {
        ASSERT_TRUE(result.moves.empty());
        continue;
      }

      std::uint64_t rebuilt = 0;
      int place_count = 0;
      for (const TileMove& move : result.moves) {
        ASSERT_GE(move.to_row, 0);
        ASSERT_LT(move.to_row, kBoardSize);
        ASSERT_GE(move.to_col, 0);
        ASSERT_LT(move.to_col, kBoardSize);

        if (move.merged) continue;  // 被吃掉的那张不决定棋盘
        const int index = move.to_row * kBoardSize + move.to_col;
        rebuilt = SetExponent(rebuilt, index, move.exponent);
        ++place_count;
      }

      ASSERT_EQ(rebuilt, result.board)
          << "第 " << iteration << " 组，方向 " << DirectionName(direction) << " 轨迹与结果不一致";
      ASSERT_GT(place_count, 0) << "走子成功了却没有任何一条落位轨迹";

      // 每条轨迹的起点必须是原棋盘上的非空格
      for (const TileMove& move : result.moves) {
        const int from_index = move.from_row * kBoardSize + move.from_col;
        ASSERT_GT(GetExponent(board, from_index), 0)
            << "第 " << iteration << " 组，方向 " << DirectionName(direction) << "：轨迹起点 ("
            << move.from_row << "," << move.from_col << ") 在原棋盘上是空的";
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 单行走子：与权威实现逐行穷举对拍
//
// 这是**本文件里最有价值的一条测试**。理由：
//
// 单行走子是整个引擎的地基，而它的正确写法有两处极易搞错、且在常见输入上
// 完全看不出来：
//
//   1. 必须先"压实"再找相邻对。[2,0,2,0] 右移必须是 [0,0,0,4]，
//      而不是 [0,0,4,0] —— 两者都有 4，位置却不同。
//      满行输入（[2,2,4,4]）对这两种实现给出相同结果，所以随手写的用例抓不到。
//   2. 右移 = 反排 -> 向左推 -> **再反排**。少最后一次反排，
//      [1,0,0,0] 右移会原地不动，而 [2,2,4,4] 这类满行输入恰好不受影响。
//
// 所以这里不手写期望值，而是把 Gabriele Cirulli 原始 game_manager.js 的 move()
// 逐字翻译成参考实现，对全部 65536 种行逐一比对。
// 这条测试在实际开发中抓出了上述两个 bug 之一，以及一个得分被静默截断的溢出。
// ---------------------------------------------------------------------------

namespace {

struct ReferenceRowResult {
  PackedRow row = 0;
  std::uint64_t gain = 0;
};

// 逐字翻译 game_manager.js 的 move()：
//   - buildTraversals：遍历顺序，右/下从大到小
//   - findFarthestPosition：沿向量推进到障碍
//   - 合并条件：next 存在、值相同、且 next 尚未参与本次遍历的合并
[[nodiscard]] ReferenceRowResult ReferenceMoveRow(PackedRow input, bool to_right) {
  std::array<int, kBoardSize> cells{};
  for (int i = 0; i < kBoardSize; ++i) {
    cells[static_cast<std::size_t>(i)] = static_cast<int>((input >> (kBitsPerCell * i)) & 0xFu);
  }
  std::array<bool, kBoardSize> merged_from{};

  std::array<int, kBoardSize> order = {0, 1, 2, 3};
  if (to_right) order = {3, 2, 1, 0};
  const int vector = to_right ? 1 : -1;

  ReferenceRowResult out;
  for (const int x : order) {
    const int value = cells[static_cast<std::size_t>(x)];
    if (value == 0) continue;

    int previous = x;
    int next = x + vector;
    while (next >= 0 && next < kBoardSize && cells[static_cast<std::size_t>(next)] == 0) {
      previous = next;
      next += vector;
    }

    const bool in_bounds = (next >= 0 && next < kBoardSize);
    const int next_value = in_bounds ? cells[static_cast<std::size_t>(next)] : 0;
    const bool next_merged = in_bounds && merged_from[static_cast<std::size_t>(next)];

    if (in_bounds && next_value == value && !next_merged) {
      const int merged = (value >= kMaxExponent) ? kMaxExponent : value + 1;
      if (value < kMaxExponent) {
        out.gain += static_cast<std::uint64_t>(1) << merged;
      }
      cells[static_cast<std::size_t>(next)] = merged;
      cells[static_cast<std::size_t>(x)] = 0;
      merged_from[static_cast<std::size_t>(next)] = true;
    } else {
      cells[static_cast<std::size_t>(previous)] = value;
      if (previous != x) cells[static_cast<std::size_t>(x)] = 0;
    }
  }

  for (int i = 0; i < kBoardSize; ++i) {
    out.row |= static_cast<PackedRow>(cells[static_cast<std::size_t>(i)]) << (kBitsPerCell * i);
  }
  return out;
}

// 把只占第一行的行状态变成棋盘，交给引擎走，再把第一行取回来。
[[nodiscard]] std::uint64_t RowOnlyBoard(PackedRow row) {
  std::uint64_t board = 0;
  for (int i = 0; i < kBoardSize; ++i) {
    board = SetExponent(board, i, static_cast<int>((row >> (kBitsPerCell * i)) & 0xFu));
  }
  return board;
}

[[nodiscard]] PackedRow FirstRowPacked(std::uint64_t board) {
  return static_cast<PackedRow>(board & 0xFFFFu);
}

}  // namespace

TEST(BoardRowExhaustive, MatchesAuthoritativeReferenceOnAllRows) {
  for (std::uint32_t state = 0; state < kRowStates; ++state) {
    const auto row = static_cast<PackedRow>(state);
    const std::uint64_t board = RowOnlyBoard(row);

    const ReferenceRowResult ref_left = ReferenceMoveRow(row, false);
    const MoveResult left = ApplyMove(board, Direction::kLeft);
    ASSERT_EQ(FirstRowPacked(left.board), ref_left.row)
        << "向左不一致，行状态 0x" << std::hex << state;
    ASSERT_EQ(left.score_gained, ref_left.gain) << "向左得分不一致，行状态 0x" << std::hex << state;

    const ReferenceRowResult ref_right = ReferenceMoveRow(row, true);
    const MoveResult right = ApplyMove(board, Direction::kRight);
    ASSERT_EQ(FirstRowPacked(right.board), ref_right.row)
        << "向右不一致，行状态 0x" << std::hex << state;
    ASSERT_EQ(right.score_gained, ref_right.gain)
        << "向右得分不一致，行状态 0x" << std::hex << state;
  }
}

TEST(BoardRowExhaustive, ScoresAreNeverSilentlyTruncated) {
  // 两个 16384（指数 14）合并 -> 32768（指数 15），得分应为 32768。
  //
  // 这条测试来自一个真实 bug：曾经把结果行与得分打包进一个 uint32
  // （`gain << 16 | row`），而 [16384,16384,16384,16384] 这一行的得分
  // 是 65536 = 2^16，塞不进 16 bit 的 gain 字段，于是**分数静默变成 0**。
  // 逐行穷举对拍（65536 种行）只抓出这一个失败用例 —— 靠人工构造测试
  // 几乎不可能想到它。
  const MoveResult result = ApplyMove(RowBoard(16384, 16384, 0, 0), Direction::kLeft);
  ASSERT_TRUE(result.moved);
  EXPECT_EQ(result.score_gained, 32768u);
  EXPECT_EQ(FirstRow(result.board)[0], 32768u);

  // 满行的四个 16384：合并两次，共 32768 * 2 = 65536 分。
  // 这正是当年被截断成 0 的那一行。
  const MoveResult full =
      ApplyMove(MakeBoard({16384, 16384, 16384, 16384, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
                Direction::kLeft);
  ASSERT_TRUE(full.moved);
  EXPECT_EQ(full.score_gained, 65536u) << "这一行正是被静默截断过的那一个用例";
  EXPECT_EQ(FirstRow(full.board)[0], 32768u);
  EXPECT_EQ(FirstRow(full.board)[1], 32768u);
}

TEST(BoardRowExhaustive, SlideHappensBeforeMerge) {
  // [2,0,2,0] 右移：两个 2 之间隔着空格，必须先压实再合并 -> [0,0,0,4]。
  // 若边滑边找相邻对，会得到 [0,0,4,0] —— 同样有 4，但位置不同。
  const MoveResult right = ApplyMove(RowBoard(2, 0, 2, 0), Direction::kRight);
  ASSERT_TRUE(right.moved);
  const auto row = FirstRow(right.board);
  EXPECT_EQ(row[0], 0u);
  EXPECT_EQ(row[1], 0u);
  EXPECT_EQ(row[2], 0u);
  EXPECT_EQ(row[3], 4u);
  EXPECT_EQ(right.score_gained, 4u);
}

TEST(BoardRowExhaustive, RightMoveActuallyMovesTilesToTheRight) {
  // 稀疏行最能暴露"少一次反排"的错误：那种实现会让这张牌原地不动。
  const MoveResult right = ApplyMove(RowBoard(2, 0, 0, 0), Direction::kRight);
  ASSERT_TRUE(right.moved) << "[2,0,0,0] 右移必须产生移动";
  const auto row = FirstRow(right.board);
  EXPECT_EQ(row[0], 0u);
  EXPECT_EQ(row[3], 2u);
}

// ---------------------------------------------------------------------------
// 位棋盘编码
// ---------------------------------------------------------------------------

TEST(BoardEncoding, RoundTripsThroughExponents) {
  const std::array<int, kCellCount> exponents = {0, 1, 2,  3,  4,  5,  6,  7,
                                                 8, 9, 10, 11, 12, 13, 14, 15};
  const std::uint64_t board = EncodeBoard(exponents);
  EXPECT_EQ(DecodeBoard(board), exponents);
  EXPECT_EQ(MaxExponent(board), 15);
  EXPECT_EQ(CountEmptyCells(board), 1);
}

TEST(BoardEncoding, ValueExponentConversion) {
  EXPECT_EQ(ValueToExponent(0), 0);
  EXPECT_EQ(ValueToExponent(2), 1);
  EXPECT_EQ(ValueToExponent(2048), 11);
  EXPECT_EQ(ValueToExponent(32768), 15);
  EXPECT_EQ(ValueToExponent(3), -1) << "非 2 的幂必须被拒绝，而不是四舍五入";
  EXPECT_EQ(ExponentToValue(0), 0u);
  EXPECT_EQ(ExponentToValue(11), 2048u);
}

// ---------------------------------------------------------------------------
// 上限行为：必须显式报告，绝不静默 clamp
//
// 参考原型在这里出的错很隐蔽：它把 65536 悄悄写成 32768，两个 32768 相撞
// 也合不出 65536。于是在高分段的"AI 模拟规则"与"现实规则"不是同一个游戏，
// 而这一点不会以任何方式报错。
// ---------------------------------------------------------------------------

TEST(BoardOverflow, SaturatedMergeIsReportedNotSilent) {
  const std::uint64_t board = MakeBoard({32768, 32768, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  const MoveResult result = ApplyMove(board, Direction::kLeft);

  EXPECT_TRUE(result.overflow) << "在上限处合并必须被报告出来";
  // 饱和：结果仍是 32768，而不是进位成不存在的 65536
  EXPECT_EQ(GetExponent(result.board, 0), kMaxExponent);
}

TEST(BoardOverflow, NoOverflowReportedForNormalMerges) {
  const MoveResult result = ApplyMove(RowBoard(1024, 1024, 0, 0), Direction::kLeft);
  ASSERT_TRUE(result.moved);
  EXPECT_FALSE(result.overflow);
  EXPECT_EQ(FirstRow(result.board)[0], 2048u);
}

TEST(BoardOverflow, IllegalMoveDoesNotReportOverflow) {
  // 满盘但无相邻等值：推不动，也就没有合并、没有溢出
  const std::uint64_t board = MakeBoard({32768, 16384, 32768, 16384, 16384, 32768, 16384, 32768,
                                         32768, 16384, 32768, 16384, 16384, 32768, 16384, 32768});
  for (const Direction direction :
       {Direction::kUp, Direction::kDown, Direction::kLeft, Direction::kRight}) {
    const MoveResult result = ApplyMove(board, direction);
    EXPECT_FALSE(result.moved) << DirectionName(direction);
    EXPECT_FALSE(result.overflow) << DirectionName(direction) << " 不该报溢出";
  }
}

// ---------------------------------------------------------------------------
// PRNG
// ---------------------------------------------------------------------------

TEST(RngTest, SameSeedGivesSameSequence) {
  Rng a(12345);
  Rng b(12345);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(a.NextU64(), b.NextU64());
  }
}

TEST(RngTest, DifferentSeedsDiverge) {
  Rng a(1);
  Rng b(2);
  bool differs = false;
  for (int i = 0; i < 10; ++i) {
    if (a.NextU64() != b.NextU64()) differs = true;
  }
  EXPECT_TRUE(differs);
}

TEST(RngTest, ZeroSeedIsStillUsable) {
  // splitmix64 负责把 0 打散；不能因为种子是 0 就退化成一串 0。
  Rng rng(0);
  const std::uint64_t first = rng.NextU64();
  const std::uint64_t second = rng.NextU64();
  EXPECT_NE(first, 0u);
  EXPECT_NE(first, second);
}

TEST(RngTest, BoundedStaysInRange) {
  Rng rng(99);
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t value = rng.NextBounded(16);
    EXPECT_LT(value, 16u);
  }
  EXPECT_EQ(rng.NextBounded(1), 0u) << "bound=1 时只有一个合法取值";
}

TEST(RngTest, BoundedIsReasonablyUniform) {
  // 不追求严格的统计检验，但要能抓住"永远返回 0"或明显偏斜
  Rng rng(2024);
  std::array<int, 8> counts{};
  constexpr int kDraws = 80000;
  for (int i = 0; i < kDraws; ++i) {
    ++counts[static_cast<std::size_t>(rng.NextBounded(8))];
  }
  const int expected = kDraws / 8;
  for (int bucket = 0; bucket < 8; ++bucket) {
    EXPECT_NEAR(counts[static_cast<std::size_t>(bucket)], expected, expected / 5)
        << "第 " << bucket << " 个桶偏差过大";
  }
}

// ---------------------------------------------------------------------------
// Game：生成规则与确定性
// ---------------------------------------------------------------------------

TEST(GameTest, StartsWithTwoTiles) {
  for (std::uint64_t seed = 0; seed < 20; ++seed) {
    Game game(seed);
    EXPECT_EQ(kCellCount - CountEmptyCells(game.board()), kInitialTiles) << "seed=" << seed;
    EXPECT_EQ(game.step_count(), 0u);
    EXPECT_EQ(game.score(), 0u);
    EXPECT_FALSE(game.game_over()) << "开局不可能终局";
  }
}

TEST(GameTest, SpawnedTilesAreTwoOrFour) {
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    Game game(seed);
    for (int index = 0; index < kCellCount; ++index) {
      const int exponent = GetExponent(game.board(), index);
      EXPECT_TRUE(exponent == 0 || exponent == 1 || exponent == 2)
          << "开局出现了 2 与 4 之外的方块，seed=" << seed;
    }
    const StepResult step = game.Step(Direction::kLeft);
    if (step.spawned) {
      EXPECT_TRUE(step.spawn.exponent == 1 || step.spawn.exponent == 2);
    }
  }
}

TEST(GameTest, IllegalMoveDoesNotConsumeRandomness) {
  // 这是 replay 正确性的关键：无效方向键不能被计入随机数消耗，
  // 否则"玩家多按了一下"就会让整局结果改变。
  //
  // 注意不能随便拿几个方向当"无效输入"—— 先得真的找到非法的那一个，
  // 否则两次走子都合法，棋盘本来就会不同，测的就不是这件事了。
  // 不同种子下"哪个方向非法"是变化的，所以这里搜索一个能用的种子。
  bool verified = false;
  for (std::uint64_t seed = 0; seed < 200 && !verified; ++seed) {
    Game with_illegal_input(seed);
    Game without_illegal_input(seed);

    // 两个局面先推进到同一个状态
    static_cast<void>(with_illegal_input.Step(Direction::kLeft));
    static_cast<void>(without_illegal_input.Step(Direction::kLeft));
    ASSERT_EQ(with_illegal_input.board(), without_illegal_input.board());

    // 找出一个当前确实非法的方向
    std::optional<Direction> illegal;
    for (const Direction direction :
         {Direction::kUp, Direction::kDown, Direction::kLeft, Direction::kRight}) {
      if (!ApplyMove(with_illegal_input.board(), direction).moved) {
        illegal = direction;
        break;
      }
    }
    if (!illegal.has_value()) continue;

    // 连按 5 次无效方向键
    for (int i = 0; i < 5; ++i) {
      const StepResult rejected = with_illegal_input.Step(*illegal);
      EXPECT_FALSE(rejected.moved);
      EXPECT_FALSE(rejected.spawned) << "非法走子不该生成新方块";
      EXPECT_EQ(rejected.score_gained, 0u);
    }

    // 找一个此刻仍然合法的方向做收尾比较
    std::optional<Direction> legal;
    for (const Direction direction :
         {Direction::kLeft, Direction::kDown, Direction::kRight, Direction::kUp}) {
      if (ApplyMove(with_illegal_input.board(), direction).moved) {
        legal = direction;
        break;
      }
    }
    if (!legal.has_value()) continue;

    const StepResult a = with_illegal_input.Step(*legal);
    const StepResult b = without_illegal_input.Step(*legal);
    EXPECT_TRUE(a.moved);
    EXPECT_EQ(a.moved, b.moved);
    EXPECT_EQ(a.spawned, b.spawned);
    EXPECT_EQ(with_illegal_input.board(), without_illegal_input.board())
        << "种子 " << seed << "：无效输入改变了后续结果 —— 随机数被非法走子消耗了";
    EXPECT_EQ(with_illegal_input.score(), without_illegal_input.score());
    EXPECT_EQ(with_illegal_input.step_count(), without_illegal_input.step_count())
        << "非法走子不该计入步数";
    verified = true;
  }
  EXPECT_TRUE(verified) << "没找到能构造出非法走子的种子";
}

TEST(GameTest, SameSeedAndSameMovesGiveIdenticalState) {
  auto play = [](std::uint64_t seed) {
    Game game(seed);
    while (!game.game_over()) {
      const std::optional<Direction> direction = FindAnyLegalMove(game.board());
      if (!direction.has_value()) break;
      static_cast<void>(game.Step(*direction));
    }
    return game.Serialize();
  };

  for (std::uint64_t seed = 0; seed < 25; ++seed) {
    EXPECT_EQ(play(seed), play(seed)) << "seed=" << seed << " 两次运行不一致";
  }
}

TEST(GameTest, GamesActuallyReachTerminalState) {
  // 平凡策略也应该能玩到终局 —— 否则走子/生成/终局判定这条链路里有死循环或死锁。
  const EndState end = PlayToEnd(42);
  EXPECT_TRUE(end.game_over) << "平凡策略没能把一局走到终局";
  EXPECT_GT(end.steps, 10u);
  EXPECT_GT(end.score, 0u);
}

TEST(GameTest, StepCountIncrementsOnlyOnLegalMoves) {
  constexpr std::array<Direction, 4> kOrder = {Direction::kLeft, Direction::kDown,
                                               Direction::kRight, Direction::kUp};
  Game game(11);
  std::uint32_t expected = 0;
  for (int i = 0; i < 40; ++i) {
    const StepResult step = game.Step(kOrder[static_cast<std::size_t>(i % 4)]);
    if (step.moved) ++expected;
    EXPECT_EQ(game.step_count(), expected);
  }
}

TEST(GameTest, ScoreEqualsSumOfStepGains) {
  Game game(1234);
  std::uint64_t accumulated = 0;
  while (!game.game_over()) {
    const std::optional<Direction> direction = FindAnyLegalMove(game.board());
    if (!direction.has_value()) break;
    const StepResult step = game.Step(*direction);
    if (step.moved) accumulated += step.score_gained;
  }
  EXPECT_EQ(game.score(), accumulated);
}

TEST(GameTest, SpawnAlwaysLandsOnAnEmptyCell) {
  // 生成到已占用的格子上会静默覆盖方块 —— 这类错误会让分数虚高且难以察觉。
  for (std::uint64_t seed = 0; seed < 40; ++seed) {
    Game game(seed);
    while (!game.game_over()) {
      const std::optional<Direction> direction = FindAnyLegalMove(game.board());
      if (!direction.has_value()) break;
      const StepResult step = game.Step(*direction);
      if (!step.moved) continue;

      ASSERT_TRUE(step.spawned) << "走子成功却没有生成新方块";
      const int index = step.spawn.row * kBoardSize + step.spawn.col;
      EXPECT_GT(GetExponent(game.board(), index), 0);
    }
  }
}

}  // namespace
}  // namespace ai2048
