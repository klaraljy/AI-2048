#include "ai/search.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

#include "core/rng.h"

namespace ai2048 {

namespace {

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
 *
 * ## 为什么这里是"转发"而不是自己再算一遍
 *
 * 生成规则在 2026-09-12 改成了「加权随机 + 纯随机兜底」，位置分布是
 *
 *     简单 80% × exp(安全分 × 0.35) 加权 + 20% 均匀
 *     中等 100% 均匀
 *     困难 75% × exp(不利分 × 0.35) 加权 + 25% 均匀
 *
 * （强度与饱和上限见 core/game.h 的 kEasyWeightStrength / kScoreSaturation；
 * 那两个值是实测标定的，不是文档给的 0.70/0.85 —— 原因写在 game.h 里。）
 * 那套评分有十来个项（边缘/角落/空旷/可合并/拥挤/断裂/贴大块……），
 * **在本文件里重写一份必然分叉**。本项目已经因为"两份实现悄悄分叉"
 * 吃过一次亏：走子后的生成退回均匀分布，难度只在开局生效，而外表看不出问题。
 *
 * 所以现在直接调 core 的 SpawnCellWeights —— 评分与权重表只有一份真相。
 *
 * ⚠️ 但 SpawnCellWeights 返回的是**未归一化**的相对权重（给 AI 的期望值计算用，
 * 那里常数因子会整体约掉）。本函数的契约是**归一化概率**（空格上权重和恰好为 1），
 * 所以这里补一次归一化。两个口径都对，混起来会让"预测 vs 实际"的整体对拍
 * 差一个常数倍 —— 那正是本文件里最容易悄悄出错的地方。
 *
 * ## 踩过的坑（保留记录，改这段之前先读）
 *
 * 早期版本把基础项写成 `1/|空格|`（按"均匀"满额填），再加偏置项。
 * 那等价于假设"未命中偏置时仍按 1/空格 分配"，但后续归一化会把它压掉，
 * 于是命中格的相对优势被系统性高估（实测 easy 角上预测 0.142 而实际 0.196）。
 * 现在的基础项由 core 的公式给出：`(1−w) × 总权重 / 空格数`，
 * 其中"总权重"同时缩放两个分支，所以比例是对的。
 */
std::array<double, kCellCount> SpawnWeights(std::uint64_t board, Difficulty difficulty) {
  std::array<double, kCellCount> weights = SpawnCellWeights(board, difficulty);

  double total = 0.0;
  for (const double value : weights) total += value;
  if (total <= 0.0) return weights;  // 没有空格：全零，调用方无需归一化

  for (double& value : weights) value /= total;
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
  //
  // 试过"深度优先替换 + 2 路组相联"（浅层结果不许挤掉深层结果），实测**没有收益**：
  // 每步节点数基本不变（约 6.4k），命中/写入反而从 37% 掉到 26%。
  // 原因大概是深层条目本来就少，"不让浅层挤掉深层"省的查找次数抵不上
  // 桶结构带来的额外冲突。既然测不出好处，就保留最简单、已经验证过的版本。
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

/**
 * 选出 chance 节点这一层要展开的出生位置。
 *
 * 两段式（见 SearchConfig::chance_important_cells 的说明）：
 *   1. **重要格**：按权重降序取前 K 个 —— 确定性，保证最该看的分支一定被看。
 *   2. **抽样格**：从剩下的空格里按权重无偏抽 M 个 —— 随机，但用
 *      `sampling_rng`（按 (棋盘, 深度) 播种的独立 RNG），所以同一局面
 *      永远得到同一棵树；也**不消耗游戏 RNG**。
 *
 * 抽样那一半的意义：保证冷门位置**不是永远不看**，只是"这次没看"。
 * 只取 top-K 会有系统性盲区 —— 某些空格在整个搜索里永远不会被考虑。
 *
 * @param weight_of  取某格权重（调用方已归一化，只用于比较大小与抽样）
 * @param out        输出缓冲区，长度至少 kCellCount
 * @param out_count  输出：选中的格子数
 */
void SelectChanceCells(const SearchConfig& config, const std::array<int, kCellCount>& empty_cells,
                       int empty_count, const std::function<double(int)>& weight_of,
                       Rng* sampling_rng, int* out, int* out_count) {
  *out_count = 0;
  if (empty_count <= 0) return;

  const int important = std::max(0, config.chance_important_cells);
  const int sampled = std::max(0, config.chance_sample_cells);

  // --- 1) 重要格：按权重降序，稳定排序（权重相同时按索引，保证确定性）---------
  std::array<int, kCellCount> order{};
  for (int i = 0; i < empty_count; ++i)
    order[static_cast<std::size_t>(i)] = empty_cells[static_cast<std::size_t>(i)];
  std::stable_sort(order.begin(), order.begin() + empty_count,
                   [&](int a, int b) { return weight_of(a) > weight_of(b); });

  bool taken[kCellCount] = {};
  const int take_important = std::min(important, empty_count);
  for (int i = 0; i < take_important; ++i) {
    out[(*out_count)++] = order[static_cast<std::size_t>(i)];
    taken[order[static_cast<std::size_t>(i)]] = true;
  }

  // --- 2) 抽样格：从剩下的空格里按权重无偏抽 ----------------------------------
  // 权重的绝对大小不重要（只用来当抽样概率），所以直接用 weight_of 的原值。
  const int remaining = empty_count - take_important;
  const int take_sampled = std::min(sampled, remaining);
  if (take_sampled > 0) {
    std::array<int, kCellCount> pool{};
    std::array<double, kCellCount> pool_weight{};
    int pool_size = 0;
    double pool_total = 0.0;
    for (int i = take_important; i < empty_count; ++i) {
      const int index = order[static_cast<std::size_t>(i)];
      const double w = weight_of(index);
      if (w <= 0.0) continue;
      pool[static_cast<std::size_t>(pool_size)] = index;
      pool_weight[static_cast<std::size_t>(pool_size)] = w;
      pool_total += w;
      ++pool_size;
    }

    for (int slot = 0; slot < take_sampled && pool_size > 0; ++slot) {
      // 逆变换抽样：一次随机数按累积权重落点。
      const std::uint64_t roll = sampling_rng->NextBounded(1'000'000);
      const double target = (static_cast<double>(roll) / 1'000'000.0) * pool_total;
      int pick = pool_size - 1;  // 边界兜底
      double acc = 0.0;
      for (int i = 0; i < pool_size; ++i) {
        acc += pool_weight[static_cast<std::size_t>(i)];
        if (target < acc) {
          pick = i;
          break;
        }
      }
      const int index = pool[static_cast<std::size_t>(pick)];
      if (taken[index]) continue;  // 理论上不会发生；保持健壮
      out[(*out_count)++] = index;
      taken[index] = true;
      // 从池里移除，保证不重复抽到同一格。
      pool_total -= pool_weight[static_cast<std::size_t>(pick)];
      pool[pick] = pool[pool_size - 1];
      pool_weight[pick] = pool_weight[static_cast<std::size_t>(pool_size - 1)];
      --pool_size;
    }
  }
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

    // ---------------------------------------------------------------------------
    // 选出这一层要展开的出生位置
    // ---------------------------------------------------------------------------
    // 优先用"重要格 + 抽样格"（见 SearchConfig::chance_important_cells 的说明），
    // 它把分支因子从"空格数×2"压到约 16，从而让树能长到 5~6 步；
    // 两者都为 0 时才退回旧的"枚举全部空格"。
    int selected[kCellCount];
    int selected_count = 0;
    if (config_.chance_important_cells > 0 || config_.chance_sample_cells > 0) {
      // ⚠️ 抽样 RNG 必须按 **(这个棋盘, 这个深度)** 重新播种。
      // 不能让它顺着搜索顺序一路用下去：那样"抽到哪几个格"会取决于
      // 这个节点在整棵树里的**到达顺序**，而到达顺序又受置换表命中、
      // 迭代加深、时间截断影响 —— 结果就是"同一次搜索重跑结果不同"。
      // 按棋盘播种之后，同一局面的同一层永远抽到同一批格子，
      // 搜索是纯函数，确定性保住了。
      sampling_rng_ = Rng(board ^ (static_cast<std::uint64_t>(depth) * 0x9E37'79B9'7F4A'7C15ULL) ^
                          0xA5A5'5A5A'1234'5678ULL);
      SelectChanceCells(
          config_, empty_cells, empty_count, [&](int index) { return per_cell_weight(index); },
          &sampling_rng_, selected, &selected_count);
    } else {
      selected_count = empty_count;
      for (int i = 0; i < empty_count; ++i) {
        selected[i] = empty_cells[static_cast<std::size_t>(i)];
      }
      if (config_.chance_sample_limit > 0) {
        selected_count = std::min(empty_count, config_.chance_sample_limit);
      }
    }

    // 2 与 4 的概率按**当前难度**取，与实际生成规则一致（见 FourSpawnProbability）。
    const double p_four = FourSpawnProbability(config_.difficulty);
    const double p_two = 1.0 - p_four;

    float total = 0.0F;
    double weight_sum = 0.0;

    for (int i = 0; i < selected_count; ++i) {
      const int index = selected[i];
      const double cell_share = per_cell_weight(index) * probability;
      for (int exponent = 1; exponent <= 2; ++exponent) {
        const double branch_probability = cell_share * (exponent == 2 ? p_four : p_two);
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

  // 选"抽样格"用的 RNG。**独立于游戏 RNG**，按 (棋盘, 深度) 播种 ——
  // 所以同一局面永远得到同一棵树，"同种子逐字节一致"不受影响；
  // 也绝不消耗游戏 RNG 的随机流（否则会改变后续生成）。
  Rng sampling_rng_{0};
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

double FourSpawnProbability(Difficulty difficulty) noexcept {
  switch (difficulty) {
    case Difficulty::kEasy:
      return static_cast<double>(kFourSpawnEasy) / static_cast<double>(kSpawnValueDenominator);
    case Difficulty::kHard:
      return static_cast<double>(kFourSpawnHard) / static_cast<double>(kSpawnValueDenominator);
    case Difficulty::kNormal:
    default:
      return static_cast<double>(kFourSpawnNormal) / static_cast<double>(kSpawnValueDenominator);
  }
}

int AdaptiveDepth(std::uint64_t board, const SearchConfig& config) noexcept {
  const int empty = CountEmptyCells(board);
  int depth = config.base_depth;

  // 旧版只看空格数。问题是**空格多 ≠ 宽松**：棋盘中央被高牌隔成两半时，
  // 空位可能还有 6~8 个，可走方向却只剩 1 个。旧策略会判成"宽松"反而减深度，
  // 恰恰在最需要算清的局面上下手最轻。所以现在两个信号都给：
  //
  //   空格多  → 容错高，减一层省钱
  //   方向少  → 局面危险，加层（这是**新增**的，也是更准的那个）
  //
  // 两者叠加时先减后加，最后统一 clamp。
  if (empty >= config.many_empty_threshold) depth -= config.depth_penalty_when_many_empty;
  if (empty <= config.few_empty_threshold) depth += config.depth_bonus_when_few_empty;

  // CountMobility 要试走 4 个方向，是这里唯一有实际开销的调用；
  // 但自适应深度每局只算一次（每次决策一次），代价可以忽略。
  if (CountMobility(board) <= config.few_mobility_threshold) {
    depth += config.depth_bonus_when_few_mobility;
  }

  // 对齐到偶数层。所有调整都已经是偶数，这一步是**兜底**：
  // 调用方或以后有人把某项调整改成奇数时，至少这里不会静默丢掉加成，
  // 而是把它变成可预测的向下取整。
  return AlignToEvenLayers(std::clamp(depth, config.min_depth, config.max_depth));
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
  //
  // 目标深度对齐到偶数层：循环只走偶数，未对齐的话奇数会被白白丢掉
  // （见 AlignToEvenLayers 的说明 —— 这个坑真的踩过）。
  const int target_depth = AlignToEvenLayers(AdaptiveDepth(board, config));
  Searcher searcher(config, table);

  std::array<float, 4> best_value{};
  best_value.fill(-std::numeric_limits<float>::infinity());
  std::array<int, 4> order = {0, 1, 2, 3};

  // 超时判据用"这一轮搜完了几个**合法**方向"。
  //
  // ⚠️ 不能用 `improved_any`（只要搜过至少一个就采用）。那样在超时时会把
  // "部分方向是新深度的值、其余还是旧深度的值"混在一起比较 —— 而
  // 深层搜索**整体**比浅层更悲观（多算了对手的好运气），所以新值系统性偏低，
  // 混合比较等于在惩罚"碰巧被排在前面、来得及重搜"的方向。
  //
  // 正确做法：只有**全部**合法方向都在本轮拿到了新值，这一轮才可采信。
  const int legal_total = legal_count;
  int deepest_complete = 0;

  for (int depth = 2; depth <= target_depth; depth += 2) {
    std::array<float, 4> this_value{};
    this_value.fill(-std::numeric_limits<float>::infinity());
    int evaluated = 0;

    // 按上一轮的值从高到低搜，好的分支先算，超时时至少手里有它。
    std::sort(order.begin(), order.end(), [&](int a, int b) {
      return best_value[static_cast<std::size_t>(a)] > best_value[static_cast<std::size_t>(b)];
    });

    for (const int index : order) {
      const std::size_t i = static_cast<std::size_t>(index);
      if (!candidates[i].legal) continue;

      const MoveResult move = ApplyMove(board, candidates[i].direction);
      this_value[i] = searcher.SearchChance(move.board, depth - 1, 1.0);
      ++evaluated;

      if (searcher.OutOfTime()) {
        searcher.stats_.timed_out = true;
        break;
      }
    }

    // 只有合法方向全部拿到新值，这一轮才算完成、才允许覆盖上一轮。
    if (evaluated == legal_total) {
      best_value = this_value;
      deepest_complete = depth;
      searcher.stats_.reached_depth = depth;
    }
    if (searcher.stats_.timed_out) break;
  }

  // 完成深度记进统计 —— 界面/跑批靠它显示"实际搜到多深"。
  // 注意 reached_depth 只在完整完成时更新，所以它就是 deepest_complete。
  (void)deepest_complete;

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