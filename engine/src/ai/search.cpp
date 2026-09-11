#include "ai/search.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace ai2048 {

namespace {

// 生成 2 与 4 的概率。规则固定：90% 出 2。
constexpr double kProbFour = 0.1;
constexpr double kProbTwo = 0.9;

// 节点类型：同一个棋盘在 max 节点与 chance 节点上的含义不同，
// 必须编进置换表的 key，混用会给出错误的值。
constexpr std::uint8_t kKindMax = 1;
constexpr std::uint8_t kKindChance = 2;

// 「这一局已经输了」的分数。
//
// **必须是有限值，不能用 -infinity。** 这是踩过的坑：
// 根节点用 `-inf` 当"这个方向还没搜过"的哨兵，见下面的 `std::isinf` 判断。
// 如果死局也用 -inf 表示，那么"必败"和"没搜过"就分不开了 ——
// 根节点会把必败方向当成未搜索，回退到 quick_estimate，于是必败局面拿到正分
// （实测：差一步死的局面在深度 8 上打 +2357）。
//
// 取一个绝对值远大于任何评估结果的有限值即可：评估的量级在千位，
// 这里用 -1e30，既不会与正常分数混淆，也不会在累加/平均时溢出。
constexpr float kLostValue = -1.0e30F;

}  // namespace

/**
 * 每个格子被打上新方块的**相对权重**，按当前难度的生成规则算。
 *
 * 必须与 core/game.cpp 的 Game::SpawnRandomTile **语义一致** ——
 * 这是 AI 的"世界模型"，与实际游戏不符会让它低估风险或高估机会。
 * 一致性由 tests/difficulty_test.cpp 的 DifficultyWorldModel 系列断言保证
 * （它是把这里的权重归一化后，与实际采样 20000 次得到的频率逐格比较）。
 *
 * 生成规则是两个分支的混合，所以权重也必须是**两个条件分布的加权和**：
 *
 *   偏置分支以概率 b 在候选集合 C 里均匀选一个；
 *   否则（概率 1-b）在全盘空格里均匀选一个。
 *
 *   → 命中候选的格子： w = b/|C| + (1-b)/|空格|
 *   → 其他空格：       w =         (1-b)/|空格|
 *   → 已占格：         不作要求（搜索只遍历空格）
 *
 * 其中 easy 的 b=0.7、C=空角落；hard 的 b=0.8、C=最大块的相邻空格；
 * normal 没有偏置分支，退化成 w = 1/|空格|。
 *
 * ⚠️ 踩过的坑：一开始把基础项写成 `1/|空格|`（也就是按"均匀"满额填），
 * 然后把偏置项加上去 —— 那等价于假设"未命中偏置时仍然按 1/空格 分配"，
 * 但后面的归一化会把它压掉，于是命中格的相对优势被系统性高估
 * （实测 easy 角上预测 0.142 而实际 0.196）。
 * 正确的基础项是 `(1-b)/|空格|`。
 *
 * 用 double 而不是 float：这些权重会被反复相乘累加，
 * float 在深搜里会累积可见的误差。
 */
std::array<double, kCellCount> SpawnWeights(std::uint64_t board, Difficulty difficulty) {
  std::array<double, kCellCount> weights{};

  int empty_count = 0;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) == 0) ++empty_count;
  }
  if (empty_count == 0) {
    weights.fill(0.0);
    return weights;
  }

  // 先把"候选集合"和偏置概率定下来
  std::array<int, kCellCount> candidates{};
  int candidate_count = 0;
  double bias = 0.0;

  if (difficulty == Difficulty::kEasy) {
    for (const int index : {0, 3, 12, 15}) {
      if (GetExponent(board, index) == 0) {
        candidates[static_cast<std::size_t>(candidate_count)] = index;
        ++candidate_count;
      }
    }
    bias = static_cast<double>(kEasyCornerNumerator) / static_cast<double>(kDifficultyDenominator);
  } else if (difficulty == Difficulty::kHard) {
    constexpr std::array<std::array<int, 2>, 4> kOffsets = {{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    // 与生成规则一致：**按等级从高到低**找第一个"四周有空位"的方块。
    //
    // 两个必须做对的地方（都踩过）：
    //   1. 不能只看最大块 —— 它在残局常被围死，而偏置本该落在
    //      "还有空位的大块"旁边，否则残局里这条规则等于不生效。
    //   2. **每个等级要检查它的所有方块**，不能只看行优先第一个 ——
    //      否则只有位置最靠前的方块能触发，表现成"新方块全挤在左上角"。
    for (int exponent = MaxExponent(board); exponent >= 1 && candidate_count == 0; --exponent) {
      for (int index = 0; index < kCellCount && candidate_count == 0; ++index) {
        if (GetExponent(board, index) != exponent) continue;

        const int row = index / kBoardSize;
        const int col = index % kBoardSize;
        for (const auto& offset : kOffsets) {
          const int r = row + offset[0];
          const int c = col + offset[1];
          if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
          const int neighbour = r * kBoardSize + c;
          if (GetExponent(board, neighbour) == 0) {
            candidates[static_cast<std::size_t>(candidate_count)] = neighbour;
            ++candidate_count;
          }
        }
        // 这个方块被围死就继续看同等级的下一个
      }
    }
    bias = static_cast<double>(kHardNearMaxNumerator) / static_cast<double>(kDifficultyDenominator);
  }

  // 偏置不可用（没有候选格）时整条规则退化成全盘均匀 —— 与生成实现一致。
  if (candidate_count == 0) bias = 0.0;

  const double uniform = 1.0 / static_cast<double>(empty_count);
  const double base_share = (1.0 - bias) * uniform;
  weights.fill(base_share);

  if (candidate_count > 0 && bias > 0.0) {
    const double candidate_share = bias / static_cast<double>(candidate_count);
    for (int i = 0; i < candidate_count; ++i) {
      weights[static_cast<std::size_t>(candidates[static_cast<std::size_t>(i)])] += candidate_share;
    }
  }

  return weights;
}

namespace {

// 混合函数：避免低位相同的棋盘挤在同一个桶里。
[[nodiscard]] std::uint64_t MixHash(std::uint64_t board, std::uint8_t kind) noexcept {
  std::uint64_t x = board ^ (static_cast<std::uint64_t>(kind) * 0x9E37'79B9'7F4A'7C15ULL);
  x ^= x >> 33;
  x *= 0xFF51'AFD7'ED55'8CC7ULL;
  x ^= x >> 33;
  x *= 0xC4CE'B9FE'1A85'EC53ULL;
  x ^= x >> 33;
  return x;
}

// ---------------------------------------------------------------------------
// 8 重对称（旋转 + 镜像）
//
// 4x4 棋盘在 8 种旋转/镜像下**棋力完全等价** —— 从任意一个变换出发，
// 最佳走子与期望值都只差一个同样的变换。所以置换表可以把这 8 个盘面
// 当成同一个 key，命中率理论上提升 8 倍。
//
// 这一步对深搜特别重要：越往深处，同一盘面以不同朝向重复出现的概率越高。
// ---------------------------------------------------------------------------

// 反排 4 格行里的 nibble（等价于水平镜像）。
[[nodiscard]] constexpr std::uint64_t ReverseCellsInRows(std::uint64_t x) noexcept {
  x = ((x & 0x3333'3333'3333'3333ULL) << 2) | ((x & 0xCCCC'CCCC'CCCC'CCCCULL) >> 2);
  x = ((x & 0x0F0F'0F0F'0F0F'0F0FULL) << 4) | ((x & 0xF0F0'F0F0'F0F0'F0F0ULL) >> 4);
  return x;
}

// 交换相邻的两行（等价于交换上下的 2x2 块）。
[[nodiscard]] constexpr std::uint64_t SwapRowPairs(std::uint64_t x) noexcept {
  return ((x & 0x0000'FFFF'0000'FFFFULL) << 16) | ((x & 0xFFFF'0000'FFFF'0000ULL) >> 16);
}

// 交换相邻的两列（等价于交换左右的 2x2 块）。
[[nodiscard]] constexpr std::uint64_t SwapColumnPairs(std::uint64_t x) noexcept {
  return ((x & 0x00FF'00FF'00FF'00FFULL) << 8) | ((x & 0xFF00'FF00'FF00'FF00ULL) >> 8);
}

[[nodiscard]] std::uint64_t TransposeBoard(std::uint64_t b) noexcept {
  std::uint64_t r = 0;
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 0; col < kBoardSize; ++col) {
      r = SetExponent(r, col * kBoardSize + row, GetExponent(b, row * kBoardSize + col));
    }
  }
  return r;
}

// 8 个变换里的字典序最小者。它作为置换表的 key。
[[nodiscard]] std::uint64_t CanonicalKey(std::uint64_t board) noexcept {
  const std::uint64_t transposed = TransposeBoard(board);

  std::uint64_t best = board;
  const std::uint64_t candidates[8] = {
      board,
      ReverseCellsInRows(board),
      SwapRowPairs(board),
      ReverseCellsInRows(SwapRowPairs(board)),
      transposed,
      ReverseCellsInRows(transposed),
      SwapRowPairs(transposed),
      ReverseCellsInRows(SwapRowPairs(transposed)),
  };
  for (const std::uint64_t candidate : candidates) {
    best = std::min(best, candidate);
  }
  return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// 置换表
// ---------------------------------------------------------------------------

struct TranspositionTable::Impl {
  struct Entry {
    std::uint64_t key = 0;
    float value = 0.0F;
    std::uint8_t depth = 0;
    std::uint8_t kind = 0;
    bool valid = false;
  };

  std::vector<Entry> entries;
  std::size_t mask = 0;
  bool use_symmetry_keys = false;
};

TranspositionTable::TranspositionTable(std::size_t capacity, bool use_symmetry_keys)
    : impl_(new Impl()) {
  impl_->use_symmetry_keys = use_symmetry_keys;
  if (capacity == 0) return;
  // 取 2 的幂，用位与代替取模
  std::size_t size = 1;
  while (size * 2 <= capacity) size *= 2;
  impl_->entries.assign(size, Impl::Entry{});
  impl_->mask = size - 1;
}

TranspositionTable::~TranspositionTable() { delete impl_; }

bool TranspositionTable::Enabled() const noexcept { return !impl_->entries.empty(); }

void TranspositionTable::Reset() noexcept {
  std::fill(impl_->entries.begin(), impl_->entries.end(), Impl::Entry{});
}

std::optional<float> TranspositionTable::Lookup(std::uint64_t board, int depth, std::uint8_t kind) {
  if (impl_->entries.empty()) return std::nullopt;
  const std::uint64_t key = impl_->use_symmetry_keys ? CanonicalKey(board) : board;
  const Impl::Entry& entry = impl_->entries[MixHash(key, kind) & impl_->mask];
  if (!entry.valid || entry.key != key || entry.kind != kind) return std::nullopt;
  // 只接受"至少一样深"的结果。更浅的结果不能冒充更深的结果。
  if (entry.depth < static_cast<std::uint8_t>(depth)) return std::nullopt;
  return entry.value;
}

void TranspositionTable::Store(std::uint64_t board, int depth, std::uint8_t kind, float value) {
  if (impl_->entries.empty()) return;
  const std::uint64_t key = impl_->use_symmetry_keys ? CanonicalKey(board) : board;
  Impl::Entry& entry = impl_->entries[MixHash(key, kind) & impl_->mask];
  // 简单替换：新结果总是写入。命中率靠容量保证。
  entry.key = key;
  entry.value = value;
  entry.depth = static_cast<std::uint8_t>(std::min(depth, 255));
  entry.kind = kind;
  entry.valid = true;
}

namespace {

// ---------------------------------------------------------------------------
// 搜索上下文
// ---------------------------------------------------------------------------

/**
 * 叶子评估：配了学习评估就用它，否则用手写启发式。
 *
 * 抽成一个函数是为了让"到底用哪个评估"只有一处真相 ——
 * 之前 5 个叶子出口各写一遍 Evaluate(board, weights)，
 * 接管的时候漏掉一处就会变成两种评估混用，分数会毫无道理地掉。
 *
 * @param terminal 该局面是否已经终局。学习评估需要这个标志来输出 0
 *   （"之后再也拿不到分"）；手写启发式不区分终局，只看盘面形状。
 */
[[nodiscard]] float SearchEvaluate(const SearchConfig& config, std::uint64_t board,
                                   bool terminal = false) {
  if (config.leaf_evaluator != nullptr) {
    // terminal 默认参数不够用：chance 节点的叶子是"生成新方块之后"的盘面，
    // 它完全可能已经被堵死。学习评估不被告知终局的话会把死局评成正分 ——
    // 这正是 C1 阶段"加容量、加局数都卡在 2,300 分"的那个 bug 的搜索侧版本。
    //
    // 探测代价很低（最多 4 次走子，且通常第一次就命中），
    // 而且只在接了学习评估时才付这个代价。
    const bool is_dead = terminal || !HasLegalMove(board);
    return config.leaf_evaluator(config.leaf_evaluator_context, board, is_dead);
  }
  return Evaluate(board, config.weights);
}

class Searcher {
 public:
  Searcher(const SearchConfig& config, TranspositionTable* table)
      : config_(config), table_(table) {}

  [[nodiscard]] double ElapsedMs() const noexcept {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
        .count();
  }

  [[nodiscard]] bool OutOfTime() noexcept {
    if (config_.time_budget_ms <= 0) return false;
    if (timed_out_) return true;
    if (ElapsedMs() >= static_cast<double>(config_.time_budget_ms)) {
      timed_out_ = true;
      return true;
    }
    return false;
  }

  // max 节点：玩家选一个方向，取所有方向里的最大值。
  [[nodiscard]] float SearchMax(std::uint64_t board, int depth, double probability) {
    ++stats_.nodes;
    if (OutOfTime()) return SearchEvaluate(config_, board);

    if (table_ != nullptr) {
      if (const auto cached = table_->Lookup(board, depth, kKindMax)) {
        ++stats_.tt_hits;
        return *cached;
      }
    }

    float best = -std::numeric_limits<float>::infinity();
    bool any_legal = false;
    for (const Direction direction :
         {Direction::kUp, Direction::kDown, Direction::kLeft, Direction::kRight}) {
      const MoveResult move = ApplyMove(board, direction);
      if (!move.moved) continue;
      any_legal = true;
      if (depth <= 0) continue;  // 深度用完：这一层只看合法性，不再往下搜
      best = std::max(best, SearchChance(move.board, depth - 1, probability));
    }

    // **死局必须显式判定，不能靠静态评估"碰巧给低分"。**
    //
    // 这是从参考实现（F:\AI编程\2048-ai2\2048-ai）的缺陷里学来的一条：
    // 它的叶节点不检查死局，于是实测同一个必败局面
    // `expectimax(depth=0) = +11499`、`expectimax(depth=2) = -1e18` ——
    // 是否识别死局取决于它在第几层被发现，而且叶子上是**正分**。
    //
    // 本实现原先也有一版错法：用 -infinity 表示死局，而根节点又用 -infinity
    // 当"未搜索"的哨兵，两者混在一起，必败方向被回退成 quick_estimate，
    // 结果必败局面在深度 8 上照样拿正分。现在用有限的 kLostValue 表示必败。
    if (!any_legal) {
      best = kLostValue;
    } else if (depth <= 0) {
      // 深度用完但仍有路可走：用静态评估。这里特意放在死局判定**之后**，
      // 顺序反了就会重演"地平线内的死局变成正分"。
      best = SearchEvaluate(config_, board);
    }

    if (table_ != nullptr) {
      table_->Store(board, depth, kKindMax, best);
      ++stats_.tt_stores;
    }
    return best;
  }

  // chance 节点：对每个空格、每种新块取值求加权平均。
  [[nodiscard]] float SearchChance(std::uint64_t board, int depth, double probability) {
    ++stats_.nodes;
    ++stats_.chance_nodes;
    if (OutOfTime()) return SearchEvaluate(config_, board);
    if (depth <= 0) return SearchEvaluate(config_, board);

    if (table_ != nullptr) {
      if (const auto cached = table_->Lookup(board, depth, kKindChance)) {
        ++stats_.tt_hits;
        return *cached;
      }
    }

    std::array<int, kCellCount> empty_cells{};
    int empty_count = 0;
    for (int index = 0; index < kCellCount; ++index) {
      if (GetExponent(board, index) == 0) {
        empty_cells[static_cast<std::size_t>(empty_count)] = index;
        ++empty_count;
      }
    }
    if (empty_count == 0) return SearchEvaluate(config_, board);

    // 每个空格被打上新方块的**相对权重**。
    //
    // 默认（kNormal）全部为 1 = 均匀分布，即标准 2048。
    // 另两档要按难度的生成规则加权，否则 AI 的世界模型与实际游戏不符：
    //   easy 下它低估了"角上会冒出新块"的概率（保守，无害）
    //   hard 下它低估了"最大块旁边会冒出新块"的风险（**有害**，走子偏乐观）
    //
    // 注意这里算的是**相对**权重，后面会按 weight_sum 归一化，
    // 所以只需要各格之间的比例正确，不必凑出绝对概率。
    std::array<double, kCellCount> cell_weight{};
    cell_weight.fill(1.0);
    double weight_total = static_cast<double>(empty_count);
    if (config_.difficulty != Difficulty::kNormal) {
      weight_total = 0.0;
      const std::array<double, kCellCount> weights = SpawnWeights(board, config_.difficulty);
      for (int i = 0; i < empty_count; ++i) {
        const auto index = static_cast<std::size_t>(empty_cells[static_cast<std::size_t>(i)]);
        cell_weight[index] = weights[index];
        weight_total += weights[index];
      }
    }

    // 归一化前先做概率剪枝的判据：某个空格本身的权重占比。
    // 全盘均匀时就是 1/空格数（与旧实现一致）。
    const auto per_cell_weight = [&](int index) {
      if (weight_total <= 0.0) return 0.0;
      return cell_weight[static_cast<std::size_t>(index)] / weight_total;
    };

    int limit = empty_count;
    if (config_.chance_sample_limit > 0) {
      limit = std::min(empty_count, config_.chance_sample_limit);
    }

    float total = 0.0F;
    double weight_sum = 0.0;

    for (int i = 0; i < limit; ++i) {
      const int index = empty_cells[static_cast<std::size_t>(i)];
      const double cell_share = per_cell_weight(index) * probability;
      for (int exponent = 1; exponent <= 2; ++exponent) {
        const double branch_probability = cell_share * (exponent == 2 ? kProbFour : kProbTwo);
        // 概率剪枝：极不可能的分支直接丢弃。
        if (config_.enable_probability_cutoff && branch_probability < config_.probability_cutoff) {
          ++stats_.pruned_by_probability;
          continue;
        }

        const std::uint64_t next = SetExponent(board, index, exponent);
        total += static_cast<float>(branch_probability * static_cast<double>(SearchMax(
                                                             next, depth - 1, branch_probability)));
        weight_sum += branch_probability;
      }
    }

    // 归一化被剪掉的分支，否则期望值会被系统性压低。
    const float result = weight_sum > 0.0
                             ? static_cast<float>(static_cast<double>(total) / weight_sum)
                             : SearchEvaluate(config_, board);

    if (table_ != nullptr) {
      table_->Store(board, depth, kKindChance, result);
      ++stats_.tt_stores;
    }
    return result;
  }

  SearchStats stats_;

 private:
  const SearchConfig& config_;
  TranspositionTable* table_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  bool timed_out_ = false;
};

// 根节点用的快速估值：只看形状，用于给方向排序与兜底。
[[nodiscard]] float QuickEstimate(std::uint64_t board, const Weights& weights) {
  return Evaluate(board, weights);
}

// 这一步是否把最大牌推向了"离它最近的那个角"。
// 返回 +1（靠近）/ 0（不动）/ -1（远离）。
//
// 只看最大牌，不看别的牌 —— 它是整条链的锚，锚一乱全盘皆输。
[[nodiscard]] float AnchorBiasForMove(std::uint64_t board, Direction direction) {
  int max_index = -1;
  int max_exponent = 0;
  for (int index = 0; index < kCellCount; ++index) {
    const int exponent = GetExponent(board, index);
    if (exponent > max_exponent) {
      max_exponent = exponent;
      max_index = index;
    }
  }
  if (max_index < 0) return 0.0F;

  const int row = max_index / kBoardSize;
  const int col = max_index % kBoardSize;

  // 最近的那个角
  const int target_row = (row * 2 < kBoardSize) ? 0 : kBoardSize - 1;
  const int target_col = (col * 2 < kBoardSize) ? 0 : kBoardSize - 1;

  const auto distance = [](int r, int c, int tr, int tc) {
    return std::abs(r - tr) + std::abs(c - tc);
  };
  const int before = distance(row, col, target_row, target_col);

  // 走子之后最大牌在哪
  const MoveResult move = ApplyMove(board, direction);
  if (!move.moved) return 0.0F;

  int next_index = -1;
  int next_max = 0;
  for (int index = 0; index < kCellCount; ++index) {
    const int exponent = GetExponent(move.board, index);
    if (exponent > next_max) {
      next_max = exponent;
      next_index = index;
    }
  }
  if (next_index < 0) return 0.0F;

  const int after =
      distance(next_index / kBoardSize, next_index % kBoardSize, target_row, target_col);
  if (after < before) return 1.0F;
  if (after > before) return -1.0F;
  return 0.0F;
}

}  // namespace

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

int AdaptiveDepth(std::uint64_t board, const SearchConfig& config) noexcept {
  const int empty = CountEmptyCells(board);
  int depth = config.base_depth;
  if (empty >= config.many_empty_threshold) depth -= config.depth_penalty_when_many_empty;
  if (empty <= config.few_empty_threshold) depth += config.depth_bonus_when_few_empty;
  return std::clamp(depth, config.min_depth, config.max_depth);
}

SearchResult SearchBestMove(std::uint64_t board, const SearchConfig& config,
                            TranspositionTable* table, std::optional<Direction> last_move) {
  SearchResult result;

  constexpr std::array<Direction, 4> kDirections = {Direction::kUp, Direction::kDown,
                                                    Direction::kLeft, Direction::kRight};

  // 先把四个方向的 afterstate 与快速估值算出来，用于合法性判定与排序。
  std::array<MoveEvaluation, 4> candidates{};
  int legal_count = 0;
  for (std::size_t i = 0; i < kDirections.size(); ++i) {
    const MoveResult move = ApplyMove(board, kDirections[i]);
    candidates[i].direction = kDirections[i];
    candidates[i].legal = move.moved;
    if (move.moved) {
      candidates[i].quick_estimate = QuickEstimate(move.board, config.weights);
      ++legal_count;
    }
  }

  if (legal_count == 0) {
    // 确实无步可走：move 保持 nullopt。
    //
    // 但 evaluations 仍要填满 4 条（全部 legal=false）。协议契约里
    // evaluatedMoves 是数组，前端拿它显示"AI 评估面板"；留空的话
    // 前端只能靠 totalScore 全是 -inf 去猜，不如直接给出四个明确的方向。
    for (const MoveEvaluation& candidate : candidates) {
      result.evaluations.push_back(candidate);
    }
    result.stats = SearchStats{};
    return result;
  }

  // 依次加深：先浅后深。这样即使超时，手里也有一个可用的结果 ——
  // 这也是"超时也必须返回合法步"的保证。
  const int target_depth = AdaptiveDepth(board, config);
  Searcher searcher(config, table);

  std::array<float, 4> best_value{};
  best_value.fill(-std::numeric_limits<float>::infinity());
  std::array<int, 4> order = {0, 1, 2, 3};

  for (int depth = 2; depth <= target_depth; depth += 2) {
    std::array<float, 4> this_value{};
    this_value.fill(-std::numeric_limits<float>::infinity());
    bool improved_any = false;

    // 按上一轮的值从高到低搜，好的分支先算，超时时至少手里有它。
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      return best_value[static_cast<std::size_t>(a)] > best_value[static_cast<std::size_t>(b)];
    });

    for (const int index : order) {
      const std::size_t i = static_cast<std::size_t>(index);
      if (!candidates[i].legal) continue;

      const MoveResult move = ApplyMove(board, candidates[i].direction);
      this_value[i] = searcher.SearchChance(move.board, depth - 1, 1.0);
      improved_any = true;

      if (searcher.OutOfTime()) {
        searcher.stats_.timed_out = true;
        break;
      }
    }

    if (improved_any) {
      best_value = this_value;
      searcher.stats_.reached_depth = depth;
    }
    if (searcher.stats_.timed_out) break;
  }

  // 汇总：加上根节点的方向调整，选出最终方向。
  float best_total = -std::numeric_limits<float>::infinity();
  std::optional<Direction> best_direction;

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (!candidates[i].legal) continue;

    float total = best_value[i];
    if (std::isinf(total)) {
      // 这一轮没搜到（例如超时提前退出），退回到快速估值，保证有分可排。
      total = candidates[i].quick_estimate;
    }

    // 方向偏好：同向小幅加分，反向小幅扣分，抑制来回摆动。
    if (last_move.has_value()) {
      if (*last_move == candidates[i].direction) {
        total += config.consistency_bonus;
      } else if (static_cast<int>(*last_move) / 2 ==
                 static_cast<int>(candidates[i].direction) / 2) {
        // kUp/kDown 同轴，kLeft/kRight 同轴：互为反向
        total -= config.anti_oscillation_penalty;
      }
    }

    // 锚点偏好：如果这一步把最大牌往"离它最近的那个角"推，小幅加分。
    // 目的不是替代评估函数，而是在几个搜索值接近的方向里，
    // 优先选那个能维持大牌位置一致性的 —— 来回换角会让链断掉。
    if (config.weights.anchor_bias != 0.0F) {
      total += config.weights.anchor_bias * AnchorBiasForMove(board, candidates[i].direction);
    }

    candidates[i].future_score = best_value[i];
    candidates[i].total_score = total;
    result.evaluations.push_back(candidates[i]);

    if (total > best_total) {
      best_total = total;
      best_direction = candidates[i].direction;
    }
  }

  std::sort(result.evaluations.begin(), result.evaluations.end(),
            [](const MoveEvaluation& a, const MoveEvaluation& b) {
              return a.total_score > b.total_score;
            });

  result.move = best_direction;
  result.stats = searcher.stats_;
  result.stats.elapsed_ms = searcher.ElapsedMs();

  // 兜底：只要存在合法方向，就**必须**给出一个。
  // 调用方会把空 move 当成"AI 无合法步"而停止演示。
  if (!result.move.has_value()) {
    for (const MoveEvaluation& evaluation : result.evaluations) {
      if (evaluation.legal) {
        result.move = evaluation.direction;
        break;
      }
    }
  }
  return result;
}

}  // namespace ai2048
