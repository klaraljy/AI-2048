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
// ---------------------------------------------------------------------------
// 蛇形模板（借鉴参考实现的 snakeScore，但补齐了它最关键的一点：**四套模板**）
//
// 思路：把"大牌沿一条不交叉的路径从角落蜿蜒排开"编码成一个线性模板，
// 每格一个权重，越靠近锚点角落的路径权重越高。
//
// 单一模板是不够的 —— 这是之前的实现缺陷：模板锚定左上角，
// 而最大块实际在别的角时，这个模板等于在奖励**错误的形状**，
// 贡献接近于噪声。参考实现的做法是四角各一套，并按当前最大块的位置
// 自动挑一套（chooseBestAnchor），这里照做。
//
// 权重是 1..16 的一个排列，和恒为 136。用**实际值**（而不是指数）相乘，
// 这一点与参考实现一致：分数最大化的本质是把大牌放在高权重路径上，
// 用指数会让大牌之间几乎无差别（11 与 12 只差 1），蛇形就失去了意义。
// ---------------------------------------------------------------------------

/** 蛇形路径的权重，行优先给出，从指定角开始蜿蜒。 */
struct SnakeTemplate {
  int row0 = 0;  // 锚点行：0 或 3
  int col0 = 0;  // 锚点列：0 或 3
  std::array<int, kCellCount> weights{};
};

/**
 * 生成一套蛇形模板。
 *
 * 路径形状：从 (row0, col0) 出发，先在**首行**横着走满 4 格，
 * 下移一行，再反向走回来，如此往复 —— 这样每行内部是单调的，
 * 行与行之间用一个"折返"连接，整条路径不交叉。
 */
[[nodiscard]] constexpr SnakeTemplate MakeSnakeTemplate(int row0, int col0) {
  SnakeTemplate t;
  t.row0 = row0;
  t.col0 = col0;
  const int row_step = row0 == 0 ? 1 : -1;
  const int start_col = col0;
  const int col_step = col0 == 0 ? 1 : -1;

  // 权重 16 在锚点角，沿路径递减到 1。
  int weight = 16;
  for (int i = 0; i < kBoardSize; ++i) {
    const int row = row0 + i * row_step;
    for (int j = 0; j < kBoardSize; ++j) {
      // 偶数行顺着走，奇数行反着走 → 蛇形
      const int jj = (i % 2 == 0) ? j : (kBoardSize - 1 - j);
      const int col = start_col + jj * col_step;
      t.weights[static_cast<std::size_t>(row * kBoardSize + col)] = weight--;
    }
  }
  return t;
}

inline constexpr SnakeTemplate kSnakeTopLeft = MakeSnakeTemplate(0, 0);
inline constexpr SnakeTemplate kSnakeTopRight = MakeSnakeTemplate(0, kBoardSize - 1);
inline constexpr SnakeTemplate kSnakeBottomLeft = MakeSnakeTemplate(kBoardSize - 1, 0);
inline constexpr SnakeTemplate kSnakeBottomRight =
    MakeSnakeTemplate(kBoardSize - 1, kBoardSize - 1);

/** 权重和恒为 136，用它归一化，使权重系数的量级与其它项可比。 */
inline constexpr int kSnakeWeightSum = 16 * 17 / 2;  // = 136

/**
 * 选锚点：优先取**最大块所在的那个角**；最大块不在角上时，
 * 取"蛇形分最高"的角（也就是当前排布最接近哪个角的形状）。
 *
 * 参考实现用的是 cornerControl×1200 + snake×0.018 + edge×6 + sticky。
 * 这里只保留前两项里最本质的部分：
 *   - 最大块在角上 → 直接用它（这一条最重要，也最确定）
 *   - 否则比 snake 原始和（未归一化），选形状最接近的
 * 不引入 sticky：那是"锚点粘滞"，属于跨步状态，会破坏评估函数的纯函数性
 * （同一盘面必须给同一分，否则置换表会出错）。
 */
[[nodiscard]] const SnakeTemplate& ChooseSnakeTemplate(std::uint64_t board) {
  int max_exponent = 0;
  int max_index = -1;
  for (int index = 0; index < kCellCount; ++index) {
    const int exponent = GetExponent(board, index);
    if (exponent > max_exponent) {
      max_exponent = exponent;
      max_index = index;
    }
  }

  if (max_index >= 0) {
    const int row = max_index / kBoardSize;
    const int col = max_index % kBoardSize;
    const bool top = row == 0;
    const bool bottom = row == kBoardSize - 1;
    const bool left = col == 0;
    const bool right = col == kBoardSize - 1;
    if (top && left) return kSnakeTopLeft;
    if (top && right) return kSnakeTopRight;
    if (bottom && left) return kSnakeBottomLeft;
    if (bottom && right) return kSnakeBottomRight;
  }

  // 最大块不在角上：选蛇形原始和最大的那套模板。
  const std::array<const SnakeTemplate*, 4> candidates = {&kSnakeTopLeft, &kSnakeTopRight,
                                                          &kSnakeBottomLeft, &kSnakeBottomRight};
  const SnakeTemplate* best = candidates[0];
  double best_sum = -1.0;
  for (const SnakeTemplate* candidate : candidates) {
    double sum = 0.0;
    for (int index = 0; index < kCellCount; ++index) {
      const std::uint64_t value = ExponentToValue(GetExponent(board, index));
      sum += static_cast<double>(value) *
             static_cast<double>(candidate->weights[static_cast<std::size_t>(index)]);
    }
    if (sum > best_sum) {
      best_sum = sum;
      best = candidate;
    }
  }
  return *best;
}

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
  float snake_rank = 0.0F;
  int max_exponent = 0;
  bool max_in_corner = false;
  int max_row = 0;
  int max_col = 0;
  float edge_support = 0.0F;
  float gradient = 0.0F;
  int mobility = 0;
  int islands = 0;
};

/**
 * 还有几个方向能走（0~4）。**内部实现**，对外入口是文件末尾的 `CountMobility`。
 *
 * 这是**最直接的死局预警**：其它项都只看静态形状（空位多少、单不单调、
 * 落差大不大），唯独看不到"下一步还走不走得动"。空位多但方向被堵死的局面
 * 是存在的 —— 静态项会给它高分。
 */
[[nodiscard]] int CountMobilityImpl(std::uint64_t board) noexcept {
  int count = 0;
  for (const Direction direction :
       {Direction::kUp, Direction::kDown, Direction::kLeft, Direction::kRight}) {
    if (ApplyMove(board, direction).moved) ++count;
  }
  return count;
}

/**
 * 孤立块数量：与任何上下左右邻居**既不相等、也不差一倍**的牌。
 *
 * 这种牌参与不了任何合并链 —— 旁边没有能立刻合成它的牌，也没有它能吃掉的牌，
 * 只能等更大的牌过来救。是纯粹的"占着格子不干活"。
 *
 * 与平滑度的区别：平滑度把整盘所有相邻对的落差加总，一张孤立的小牌会被
 * 大量正常相邻对稀释；这一项只数真正卡住的那几张，所以更尖锐。
 */
[[nodiscard]] int CountIslands(std::uint64_t board) noexcept {
  constexpr std::array<std::array<int, 2>, 4> kOffsets = {{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
  int islands = 0;

  for (int index = 0; index < kCellCount; ++index) {
    const int exponent = GetExponent(board, index);
    if (exponent == 0) continue;  // 空格不算孤立块

    const int row = index / kBoardSize;
    const int col = index % kBoardSize;
    bool has_related = false;

    for (const auto& offset : kOffsets) {
      const int r = row + offset[0];
      const int c = col + offset[1];
      if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
      const int neighbour = GetExponent(board, r * kBoardSize + c);
      if (neighbour == 0) continue;
      // 相等 → 能直接合并；差 1 → 有一方能吃掉另一方
      if (neighbour == exponent || neighbour == exponent - 1 || neighbour == exponent + 1) {
        has_related = true;
        break;
      }
    }

    if (!has_related) ++islands;
  }
  return islands;
}

/**
 * 位置排名蛇形分：沿蛇形路径的**指数衰减**位置权重。
 *
 *     raw = Σ_cell  exponent[cell] × decay^rank[cell]
 *     归一化：除以 16（格数），使其与其它"每格平均"量级的项可比
 *
 * rank 是格子沿蛇形路径的位置：0 = 锚点角（最优先），15 = 路径尽头。
 *
 * ## 与已有 snake 项的区别（这是它存在的理由）
 *
 * 已有的 `snake` 用的是 **1..16 线性权重 × 牌的数值**。线性权重的问题是
 * **区分度太弱**：一张 1024 放在路径头是 1024×16，放在路径尽头是 1024×1，
 * 差值 15,360 —— 而该项目在总分里的系数只有 0.015，折算下来影响约 230 分，
 * 相对上万的总分几乎无感。所以它实际上只在惩罚"大牌完全不在路径上"。
 *
 * 指数权重才会真正惩罚**位置错误**：decay=0.5 时，同一张牌从路径头
 * 挪到第 5 格，贡献从 1.0 掉到 0.03 —— 差 32 倍。
 * 这才是"大牌必须待在角上、沿路径递减排开"的强约束。
 *
 * ## 为什么用指数而不是数值
 *
 * 与 gradient 项一致：数值会让量级失控（2048 与 1024 差 1024，
 * 而指数只差 1），且大牌会压过所有其它项。用指数时每一项的量级
 * 都在 0~15，权重系数才好定。
 */
[[nodiscard]] float SnakeRankScore(std::uint64_t board, const SnakeTemplate& snake_template) {
  constexpr double kDecay = 0.5;  // rank 每靠后一格，权重减半
  constexpr double kMaxWeight = 16.0;

  double sum = 0.0;
  for (int index = 0; index < kCellCount; ++index) {
    const int exponent = GetExponent(board, index);
    if (exponent == 0) continue;
    // 模板权重 16..1 → rank 0..15
    const int rank =
        static_cast<int>(kMaxWeight) - snake_template.weights[static_cast<std::size_t>(index)];
    sum += static_cast<double>(exponent) * std::pow(kDecay, rank);
  }
  // 除以格数：让它的量级与"每格平均"的项可比，权重系数才好解释。
  return static_cast<float>(sum / static_cast<double>(kCellCount));
}

// 行内梯度：按位置递减加权求和。
// 奖励"从左到右递减"的排布 —— 比单调性更细，因为它看落差出现在**哪里**。
// 用指数而不是数值，避免大数字把量级拉爆。
[[nodiscard]] float RowGradient(PackedRow row) {
  static constexpr std::array<int, kBoardSize> kPositionWeights = {8, 4, 2, 1};
  float sum = 0.0F;
  for (int i = 0; i < kBoardSize; ++i) {
    const int exponent = static_cast<int>((row >> (kBitsPerCell * i)) & 0xFu);
    sum += static_cast<float>(exponent * kPositionWeights[static_cast<std::size_t>(i)]);
  }
  return sum;
}

[[nodiscard]] RawTerms ComputeTerms(std::uint64_t board) noexcept {
  const PerRowTables& tables = Tables();
  RawTerms terms;

  // 行
  for (int row = 0; row < kBoardSize; ++row) {
    const PackedRow packed = ExtractRow(board, row);
    terms.monotonicity += tables.monotonicity[packed];
    terms.smoothness += tables.smoothness[packed];
    terms.merge += tables.merge[packed];
    terms.gradient += RowGradient(packed);
  }

  // 列（转置后同样按行处理）
  const std::uint64_t transposed = Transpose(board);
  for (int row = 0; row < kBoardSize; ++row) {
    const PackedRow packed = ExtractRow(transposed, row);
    terms.monotonicity += tables.monotonicity[packed];
    terms.smoothness += tables.smoothness[packed];
    terms.merge += tables.merge[packed];
    terms.gradient += RowGradient(packed);
  }

  // 蛇形 + 空格 + 最大牌位置
  //
  // 蛇形按**实际值**累加（不是指数）—— 见 kSnakeWeights 上方的说明。
  // 锚点由 ChooseSnakeTemplate 自动选，所以最大块在哪个角都能正确评估。
  const SnakeTemplate& snake_template = ChooseSnakeTemplate(board);
  double snake_sum = 0.0;
  int max_index = 0;
  for (int index = 0; index < kCellCount; ++index) {
    const int exponent = GetExponent(board, index);
    if (exponent == 0) {
      ++terms.empty_cells;
      continue;
    }
    snake_sum += static_cast<double>(ExponentToValue(exponent)) *
                 static_cast<double>(snake_template.weights[static_cast<std::size_t>(index)]);
    if (exponent > terms.max_exponent) {
      terms.max_exponent = exponent;
      max_index = index;
    }
  }
  // 归一化到"每格平均"的量级，避免权重系数必须写得很小才不压过其它项。
  terms.snake = static_cast<float>(snake_sum / kSnakeWeightSum);
  terms.snake_rank = SnakeRankScore(board, snake_template);

  terms.max_row = max_index / kBoardSize;
  terms.max_col = max_index % kBoardSize;
  terms.max_in_corner = (terms.max_row == 0 || terms.max_row == kBoardSize - 1) &&
                        (terms.max_col == 0 || terms.max_col == kBoardSize - 1);

  // 边支撑：最大牌所在行与列上的等级之和。
  // 大牌靠边时需要沿边有牌"顶着"，否则一次不利生成就能把它挤离角。
  int edge_sum = 0;
  for (int i = 0; i < kBoardSize; ++i) {
    edge_sum += GetExponent(board, terms.max_row * kBoardSize + i);
    edge_sum += GetExponent(board, i * kBoardSize + terms.max_col);
  }
  terms.edge_support = static_cast<float>(edge_sum);

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
  out.snake_rank = terms.snake_rank * weights.snake_rank;

  // 按《AI算法设计2》补的两项。原始项在 ComputeRawTerms 里按权重门控计算
  // （CountMobility 要试走 4 个方向，是这里最贵的），这里只乘权重。
  out.mobility_directions = terms.mobility;
  out.island_tiles = terms.islands;
  // 可移动性用**平方**：4→3 个方向无所谓，2→1 才是真的危险。
  out.mobility = static_cast<float>(terms.mobility * terms.mobility) * weights.mobility;
  // 孤立块是惩罚项 —— 权重应为负，符号交给权重本身。
  out.islands = static_cast<float>(terms.islands) * weights.islands;
  out.mobility_directions = terms.mobility;
  out.island_tiles = terms.islands;
  // 可移动性用**平方**：4→3 个方向无所谓，2→1 才是真的危险。
  out.mobility = static_cast<float>(terms.mobility * terms.mobility) * weights.mobility;
  // 孤立块是惩罚项，权重应为负（与文档一致）；这里只做乘法，符号交给权重。
  out.islands = static_cast<float>(terms.islands) * weights.islands;
  out.max_tile = static_cast<float>(terms.max_exponent) * weights.max_tile;

  // 最大牌在角上才给奖励，并按空格数缩放 —— 没有腾挪空间时，
  // "大牌在角"这件事本身不再能兑现成收益。
  out.corner = terms.max_in_corner ? weights.corner * (static_cast<float>(terms.empty_cells) /
                                                       static_cast<float>(kCellCount))
                                   : 0.0F;

  // 锚点控制：到**最近的**那个角的曼哈顿距离。
  // 距离 0 给满额奖励，越远扣得越狠。
  const int distance_to_row_edge = std::min(terms.max_row, kBoardSize - 1 - terms.max_row);
  const int distance_to_col_edge = std::min(terms.max_col, kBoardSize - 1 - terms.max_col);
  const int anchor_distance = distance_to_row_edge + distance_to_col_edge;

  float anchor_factor = 0.0F;
  switch (anchor_distance) {
    case 0:
      anchor_factor = 1.0F;
      break;
    case 1:
      anchor_factor = 0.2F;
      break;
    case 2:
      anchor_factor = -0.6F;
      break;
    default:
      anchor_factor = -1.2F;
      break;
  }
  out.corner_control = weights.corner_control * anchor_factor;

  // 边支撑：大牌靠边时需要沿边有牌顶着。
  out.edge_support = terms.edge_support * weights.edge_support;

  // 梯度：奖励沿一个方向递减的排布。
  out.gradient = terms.gradient * weights.gradient;

  out.total = out.empty + out.monotonicity + out.smoothness + out.merge + out.corner + out.snake +
              out.snake_rank + out.max_tile + out.corner_control + out.edge_support + out.gradient +
              out.mobility + out.islands;
  return out;
}

}  // namespace

int CountMobility(std::uint64_t board) noexcept { return CountMobilityImpl(board); }

/**
 * 原始项 + 两个**按权重门控**的补充项。
 *
 * 为什么门控：`CountMobility` 要试走 4 个方向，是整条评估链里最贵的计算；
 * 而叶子评估是搜索里调用次数最多的函数。这两项默认权重是 0，
 * 不判一下就等于每次叶子都白花这份算力。
 *
 * 门控放在这里而不是 ComputeTerms 里，是因为 ComputeTerms 只看盘面、
 * 拿不到权重 —— 而"要不要算"必须由权重决定。
 */
[[nodiscard]] RawTerms ComputeTermsGated(std::uint64_t board, const Weights& weights) noexcept {
  RawTerms terms = ComputeTerms(board);
  if (weights.mobility != 0.0F) terms.mobility = CountMobilityImpl(board);
  if (weights.islands != 0.0F) terms.islands = CountIslands(board);
  return terms;
}

float Evaluate(std::uint64_t board, const Weights& weights) noexcept {
  return ToBreakdown(ComputeTermsGated(board, weights), weights).total;
}

EvaluationBreakdown EvaluateWithBreakdown(std::uint64_t board, const Weights& weights) noexcept {
  return ToBreakdown(ComputeTermsGated(board, weights), weights);
}

}  // namespace ai2048
