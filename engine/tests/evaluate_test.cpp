// 评估函数的单元测试。
//
// 这个文件存在的理由，和 2026-09 那次发现直接相关：
//
// **评估函数的错误不会让任何东西崩掉，也不会让测试变红。** 权重量级写错
// （比如 merge 的原始值只有 1.3，却给了 30 的权重，于是它对决策的影响力
// 只剩 0.7%）只表现为"AI 弱一点"，而"弱一点"在噪声面前根本看不出来 ——
// 实测每局分数标准差约 15,000，分辨 5% 的差异要 500 局。
//
// 所以这里的测试分两类：
//   1. **语义**：每一项在它该有值的时候确实有值、量级符合预期
//      （量级错了，权重就必然错 —— 两者只有乘起来才有意义）
//   2. **可区分性**：不同表述的项必须在相同局面上给出**不同**结果 ——
//      否则它就是另一项的重复计权，加进来只会稀释信号

#include "ai/evaluate.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "core/board.h"

namespace ai2048 {
namespace {

// 值写数值，内部转成指数。EXPECT 在返回非 void 的函数里不能用，所以用断言式。
[[nodiscard]] std::uint64_t MakeBoard(const std::array<std::uint64_t, kCellCount>& values) {
  std::array<int, kCellCount> exponents{};
  for (int i = 0; i < kCellCount; ++i) {
    const std::uint64_t value = values[static_cast<std::size_t>(i)];
    const int exponent = ValueToExponent(value);
    if (exponent < 0) return 0;  // 调用方写错了非 2 的幂，后面断言会失败
    exponents[static_cast<std::size_t>(i)] = exponent;
  }
  return EncodeBoard(exponents);
}

[[nodiscard]] Weights WeightsWithMergeValue(float weight) {
  Weights w;
  w.merge = 0.0F;
  w.merge_value = weight;
  return w;
}

[[nodiscard]] Weights WeightsWithMergeCount(float weight) {
  Weights w;
  w.merge = weight;
  w.merge_value = 0.0F;
  return w;
}

}  // namespace

// ---------------------------------------------------------------------------
// merge_value：语义与可区分性
// ---------------------------------------------------------------------------

TEST(EvaluateMergeValue, SamePairCountButDifferentValuesDiffer) {
  // 这是这一项存在的**全部理由**：
  //   A 行：两个 2 合成 → 数量版 1 对，价值版 √4 = 2
  //   B 行：两个 512 合成 → 数量版 1 对，价值版 √1024 = 32
  // 数量版对两者一视同仁，价值版差 16 倍。
  const std::uint64_t small = MakeBoard({2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  const std::uint64_t big = MakeBoard({512, 512, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  ASSERT_NE(small, 0u);
  ASSERT_NE(big, 0u);

  const EvaluationBreakdown with_count_a =
      EvaluateWithBreakdown(small, WeightsWithMergeCount(1.0F));
  const EvaluationBreakdown with_count_b = EvaluateWithBreakdown(big, WeightsWithMergeCount(1.0F));
  // 数量版：两者相同（这正是它的缺陷）。
  EXPECT_FLOAT_EQ(with_count_a.merge, with_count_b.merge)
      << "数量版的语义就是数对，两行各 1 对，应当相等";

  const EvaluationBreakdown with_value_a =
      EvaluateWithBreakdown(small, WeightsWithMergeValue(1.0F));
  const EvaluationBreakdown with_value_b = EvaluateWithBreakdown(big, WeightsWithMergeValue(1.0F));
  // 价值版：大合并明显更高。
  EXPECT_GT(with_value_b.merge_value, with_value_a.merge_value)
      << "价值版对大合并没有更高评价 —— 它退化成了数量版";
}

TEST(EvaluateMergeValue, UsesSquareRootSoScaleStaysTame) {
  // 权重函数是 √(结果牌值)，不是牌值本身。这条测试锁住这个选择：
  // 合两个 2 → √4 = 2；合两个 512 → √1024 = 32。比值 16。
  // 若改成牌值本身，比值会是 256 —— 那一项会压倒其它所有项。
  const std::uint64_t small = MakeBoard({2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  const std::uint64_t big = MakeBoard({512, 512, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});

  const float a = EvaluateWithBreakdown(small, WeightsWithMergeValue(1.0F)).merge_value;
  const float b = EvaluateWithBreakdown(big, WeightsWithMergeValue(1.0F)).merge_value;

  // 行 + 列都会数：水平相邻一对会被行查表数到，竖列没有。
  EXPECT_NEAR(a, 2.0F, 1e-3F) << "合两个 2 的结果牌值是 4，√4 = 2";
  EXPECT_NEAR(b, 32.0F, 1e-3F) << "合两个 512 的结果牌值是 1024，√1024 = 32";
  // 明确断言"不是牌值本身"：那样 b 会是 1024。
  EXPECT_LT(b, 100.0F) << "量级太大 —— 说明加权用的是牌值本身而不是它的平方根";
}

TEST(EvaluateMergeValue, ZeroWhenNoAdjacentEqualPair) {
  const std::uint64_t board =
      MakeBoard({2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2, 4, 8, 16, 32, 64});
  ASSERT_NE(board, 0u);
  const EvaluationBreakdown b = EvaluateWithBreakdown(board, WeightsWithMergeValue(1.0F));
  EXPECT_FLOAT_EQ(b.merge_value, 0.0F) << "没有任何相邻等值对，这一项必须是 0";
  EXPECT_FLOAT_EQ(b.merge, 0.0F);
}

TEST(EvaluateMergeValue, DefaultIsOffSoExistingBaselineStillHolds) {
  // 新项一律默认关闭 —— 否则所有历史基准立刻不可比（见 evaluate.h 的同一条说明）。
  const Weights defaults;
  EXPECT_FLOAT_EQ(defaults.merge_value, 0.0F);
  // 而 merge 现在是重标定过的 750，不是最初的 30。
  EXPECT_FLOAT_EQ(defaults.merge, 750.0F);
}

// ---------------------------------------------------------------------------
// 量级：权重只有乘上"原始量级"才有意义
// ---------------------------------------------------------------------------

TEST(EvaluateMagnitude, MergeValueRawMagnitudeIsSmallerThanMergeWeightSuggests) {
  // 这条测试记录的是一个真实踩过的坑：`merge` 的原始值平均只有 1.3，
  // 所以写 30 的时候它对决策的影响力只剩 0.7%（等于不存在）。
  // 这里断言它的**单行原始量级**上界，防止有人以为"写 30 就是 30 的分量"。
  const std::uint64_t row = MakeBoard({2, 2, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  const EvaluationBreakdown b = EvaluateWithBreakdown(row, WeightsWithMergeCount(1.0F));
  // 权重 1.0 时，这一项的数值就是原始值。一行两对 = 2。
  EXPECT_NEAR(b.merge, 2.0F, 1e-3F);
  // 也就是说：整盘的原始值是个位数，而不是几十几百。
  EXPECT_LT(b.merge, 10.0F) << "原始量级比预期大 —— 权重的含义需要重新标定";
}

TEST(EvaluateMagnitude, EmptyCellsDominatesRawMagnitude) {
  // 另一边的极端：empty 的原始值能到 16，比 merge 大一个数量级。
  // 这就是"权重数字不能直接比大小"的根据。
  const std::uint64_t empty = 0;
  const EvaluationBreakdown b = EvaluateWithBreakdown(empty, Weights());
  EXPECT_FLOAT_EQ(static_cast<float>(b.empty_cells), 16.0F);
}

// ---------------------------------------------------------------------------
// 结构性：关闭的项必须恰好贡献 0
// ---------------------------------------------------------------------------

TEST(EvaluateDisabledTerms, ZeroWeightTermsContributeExactlyZero) {
  // edge_support / gradient / mobility / islands / snake_rank / max_tile
  // 默认都是 0。它们贡献非零会静默抬高总分，且难以察觉。
  const std::uint64_t board =
      MakeBoard({2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2, 4, 8, 16, 32, 64});
  ASSERT_NE(board, 0u);
  const EvaluationBreakdown b = EvaluateWithBreakdown(board, Weights());
  EXPECT_FLOAT_EQ(b.edge_support, 0.0F);
  EXPECT_FLOAT_EQ(b.gradient, 0.0F);
  EXPECT_FLOAT_EQ(b.mobility, 0.0F);
  EXPECT_FLOAT_EQ(b.islands, 0.0F);
  EXPECT_FLOAT_EQ(b.snake_rank, 0.0F);
  EXPECT_FLOAT_EQ(b.max_tile, 0.0F);
  EXPECT_FLOAT_EQ(b.merge_value, 0.0F);
}

TEST(EvaluateDisabledTerms, TotalEqualsSumOfParts) {
  // total 必须**恰好**是各项之和。之前 mobility/islands 的赋值被复制粘贴了
  // 两遍（重复代码），虽然结果没错，但这类重复极易演化成"某一项漏加/多加"。
  const std::array<std::array<std::uint64_t, kCellCount>, 3> boards = {{
      {2, 0, 4, 2, 8, 4, 16, 4, 32, 16, 64, 8, 128, 64, 256, 512},
      {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {2, 2, 0, 0, 4, 4, 0, 0, 8, 8, 0, 0, 16, 16, 0, 0},
  }};
  for (const auto& values : boards) {
    const std::uint64_t board = MakeBoard(values);
    const EvaluationBreakdown b = EvaluateWithBreakdown(board, Weights());
    const float sum = b.empty + b.monotonicity + b.smoothness + b.merge + b.merge_value + b.corner +
                      b.snake + b.snake_rank + b.max_tile + b.corner_control + b.edge_support +
                      b.gradient + b.mobility + b.islands;
    EXPECT_FLOAT_EQ(b.total, sum) << "total 与各项之和不符 —— 有一项漏加或多加了";
  }
}

}  // namespace ai2048
