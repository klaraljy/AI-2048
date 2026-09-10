#include "ai/evaluate.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/board.h"

namespace ai2048 {

namespace {

// 蛇形路径上的格位权重：16 最高，1 最低。
//
// 路径从左上开始蛇形推进（大牌应在高权重位）：
//   16  15  14  13
//    9  10  11  12
//    8   7   6   5
//    1   2   3   4
//
// 它把"行内单调 + 大牌靠左上角 + 尽量不留孔洞"这三件事编码成一个线性模板，
// 实现成本极低，在浅深度下比通用启发式更鲁棒。
constexpr std::array<int, kCellCount> kSnakeWeights = {
    16, 15, 14, 13, 9, 10, 11, 12, 8, 7, 6, 5, 1, 2, 3, 4,
};

constexpr int kSnakeWeightSum =
    16 + 15 + 14 + 13 + 9 + 10 + 11 + 12 + 8 + 7 + 6 + 5 + 1 + 2 + 3 + 4;

// 单调性按"牌面等级"加权：相邻两格等级差越大、且方向与单调方向相反，罚得越重。
// 让大数字的不单调被重罚，是强启发式的关键细节 ——
// 小数字乱一点无所谓，大数字错位往往是致命的。
[[nodiscard]] float RowMonotonicity(PackedRow row) {
  std::array<int, kBoardSize> v{};
  for (int i = 0; i < kBoardSize; ++i) {
    v[static_cast<std::size_t>(i)] = static_cast<int>((row >> (kBitsPerCell * i)) & 0xFu);
  }

  float increasing_penalty = 0.0F;  // 期望非递减时的反向落差
  float decreasing_penalty = 0.0F;  // 期望非递增时的反向落差
  for (int i = 0; i + 1 < kBoardSize; ++i) {
    const int left = v[static_cast<std::size_t>(i)];
    const int right = v[static_cast<std::size_t>(i + 1)];
    if (left > right) {
      increasing_penalty += static_cast<float>(left - right);
    } else {
      decreasing_penalty += static_cast<float>(right - left);
    }
  }
  return -std::min(increasing_penalty, decreasing_penalty);
}

// 平滑度：相邻非零牌等级差的绝对值之和的负数。
[[nodiscard]] float RowSmoothness(PackedRow row) {
  std::array<int, kBoardSize> v{};
  for (int i = 0; i < kBoardSize; ++i) {
    v[static_cast<std::size_t>(i)] = static_cast<int>((row >> (kBitsPerCell * i)) & 0xFu);
  }

  float penalty = 0.0F;
  for (int i = 0; i + 1 < kBoardSize; ++i) {
    const int left = v[static_cast<std::size_t>(i)];
    const int right = v[static_cast<std::size_t>(i + 1)];
    if (left != 0 && right != 0) {
      penalty += static_cast<float>(std::abs(left - right));
    }
  }
  return -penalty;
}

// 合并潜力：相邻等值非零对的数量。
[[nodiscard]] float RowMergePotential(PackedRow row) {
  float count = 0.0F;
  for (int i = 0; i + 1 < kBoardSize; ++i) {
    const int left = static_cast<int>((row >> (kBitsPerCell * i)) & 0xFu);
    const int right = static_cast<int>((row >> (kBitsPerCell * (i + 1))) & 0xFu);
    if (left != 0 && left == right) count += 1.0F;
  }
  return count;
}

struct PerRowTables {
  std::array<float, kRowStates> monotonicity{};
  std::array<float, kRowStates> smoothness{};
  std::array<float, kRowStates> merge{};
};

[[nodiscard]] const PerRowTables& Tables() {
  static const PerRowTables tables = [] {
    PerRowTables t;
    for (std::uint32_t state = 0; state < kRowStates; ++state) {
      const auto row = static_cast<PackedRow>(state);
      t.monotonicity[state] = RowMonotonicity(row);
      t.smoothness[state] = RowSmoothness(row);
      t.merge[state] = RowMergePotential(row);
    }
    return t;
  }();
  return tables;
}

[[nodiscard]] constexpr PackedRow ExtractRow(std::uint64_t board, int row) noexcept {
  return static_cast<PackedRow>((board >> (kBitsPerCell * kBoardSize * row)) & 0xFFFFu);
}

// 转置，用于把列也按行处理。与 core/board.cpp 用的是同一套位技巧；
// 这里独立实现一份是为了让 ai/ 不依赖 core 的内部头文件。
[[nodiscard]] std::uint64_t Transpose(std::uint64_t board) noexcept {
  std::uint64_t result = 0;
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 0; col < kBoardSize; ++col) {
      result =
          SetExponent(result, col * kBoardSize + row, GetExponent(board, row * kBoardSize + col));
    }
  }
  return result;
}

struct RawTerms {
  int empty_cells = 0;
  float monotonicity = 0.0F;
  float smoothness = 0.0F;
  float merge = 0.0F;
  float snake = 0.0F;
  int max_exponent = 0;
  bool max_in_corner = false;
};

[[nodiscard]] RawTerms ComputeTerms(std::uint64_t board) noexcept {
  const PerRowTables& tables = Tables();
  RawTerms terms;

  // 行
  for (int row = 0; row < kBoardSize; ++row) {
    const PackedRow packed = ExtractRow(board, row);
    terms.monotonicity += tables.monotonicity[packed];
    terms.smoothness += tables.smoothness[packed];
    terms.merge += tables.merge[packed];
  }

  // 列（转置后同样按行处理）
  const std::uint64_t transposed = Transpose(board);
  for (int row = 0; row < kBoardSize; ++row) {
    const PackedRow packed = ExtractRow(transposed, row);
    terms.monotonicity += tables.monotonicity[packed];
    terms.smoothness += tables.smoothness[packed];
    terms.merge += tables.merge[packed];
  }

  // 蛇形 + 空格 + 最大牌
  int snake_sum = 0;
  int max_index = 0;
  for (int index = 0; index < kCellCount; ++index) {
    const int exponent = GetExponent(board, index);
    if (exponent == 0) {
      ++terms.empty_cells;
      continue;
    }
    snake_sum += exponent * kSnakeWeights[static_cast<std::size_t>(index)];
    if (exponent > terms.max_exponent) {
      terms.max_exponent = exponent;
      max_index = index;
    }
  }
  terms.snake = static_cast<float>(snake_sum) / static_cast<float>(kSnakeWeightSum);

  const int max_row = max_index / kBoardSize;
  const int max_col = max_index % kBoardSize;
  terms.max_in_corner =
      (max_row == 0 || max_row == kBoardSize - 1) && (max_col == 0 || max_col == kBoardSize - 1);
  return terms;
}

[[nodiscard]] EvaluationBreakdown ToBreakdown(const RawTerms& terms, const Weights& weights) {
  EvaluationBreakdown out;
  out.empty_cells = terms.empty_cells;

  const float empty_weight =
      terms.empty_cells <= weights.empty_late_threshold ? weights.empty_late : weights.empty;
  out.empty = static_cast<float>(terms.empty_cells) * empty_weight;
  out.monotonicity = terms.monotonicity * weights.monotonicity;
  out.smoothness = terms.smoothness * weights.smoothness;
  out.merge = terms.merge * weights.merge;
  out.snake = terms.snake * weights.snake;
  out.max_tile = static_cast<float>(terms.max_exponent) * weights.max_tile;

  // 最大牌在角上才给奖励，并按空格数缩放 —— 没有腾挪空间时，
  // "大牌在角"这件事本身不再能兑现成收益。
  out.corner = terms.max_in_corner ? weights.corner * (static_cast<float>(terms.empty_cells) /
                                                       static_cast<float>(kCellCount))
                                   : 0.0F;

  out.total = out.empty + out.monotonicity + out.smoothness + out.merge + out.corner + out.snake +
              out.max_tile;
  return out;
}

}  // namespace

float Evaluate(std::uint64_t board, const Weights& weights) noexcept {
  return ToBreakdown(ComputeTerms(board), weights).total;
}

EvaluationBreakdown EvaluateWithBreakdown(std::uint64_t board, const Weights& weights) noexcept {
  return ToBreakdown(ComputeTerms(board), weights);
}

}  // namespace ai2048
