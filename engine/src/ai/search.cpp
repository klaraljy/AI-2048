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
};

TranspositionTable::TranspositionTable(std::size_t capacity) : impl_(new Impl()) {
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
  const Impl::Entry& entry = impl_->entries[MixHash(board, kind) & impl_->mask];
  if (!entry.valid || entry.key != board || entry.kind != kind) return std::nullopt;
  // 只接受"至少一样深"的结果。更浅的结果不能冒充更深的结果。
  if (entry.depth < static_cast<std::uint8_t>(depth)) return std::nullopt;
  return entry.value;
}

void TranspositionTable::Store(std::uint64_t board, int depth, std::uint8_t kind, float value) {
  if (impl_->entries.empty()) return;
  Impl::Entry& entry = impl_->entries[MixHash(board, kind) & impl_->mask];
  // 简单替换：新结果总是写入。命中率靠容量保证。
  entry.key = board;
  entry.value = value;
  entry.depth = static_cast<std::uint8_t>(std::min(depth, 255));
  entry.kind = kind;
  entry.valid = true;
}

namespace {

// ---------------------------------------------------------------------------
// 搜索上下文
// ---------------------------------------------------------------------------

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
    if (OutOfTime()) return Evaluate(board, config_.weights);
    if (depth <= 0) return Evaluate(board, config_.weights);

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
      best = std::max(best, SearchChance(move.board, depth - 1, probability));
    }

    // 没有合法走子说明这一局已经结束，直接按静态评估给分。
    if (!any_legal) best = Evaluate(board, config_.weights);

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
    if (OutOfTime()) return Evaluate(board, config_.weights);
    if (depth <= 0) return Evaluate(board, config_.weights);

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
    if (empty_count == 0) return Evaluate(board, config_.weights);

    const double per_cell = probability / static_cast<double>(empty_count);

    int limit = empty_count;
    if (config_.chance_sample_limit > 0) {
      limit = std::min(empty_count, config_.chance_sample_limit);
    }

    float total = 0.0F;
    double weight_sum = 0.0;

    for (int i = 0; i < limit; ++i) {
      const int index = empty_cells[static_cast<std::size_t>(i)];
      for (int exponent = 1; exponent <= 2; ++exponent) {
        const double piece_probability = per_cell * (exponent == 2 ? kProbFour : kProbTwo);
        // 概率剪枝：极不可能的分支直接丢弃。
        if (config_.enable_probability_cutoff && piece_probability < config_.probability_cutoff) {
          ++stats_.pruned_by_probability;
          continue;
        }

        const std::uint64_t next = SetExponent(board, index, exponent);
        total += static_cast<float>(
            piece_probability * static_cast<double>(SearchMax(next, depth - 1, piece_probability)));
        weight_sum += piece_probability;
      }
    }

    // 归一化被剪掉的分支，否则期望值会被系统性压低。
    const float result = weight_sum > 0.0
                             ? static_cast<float>(static_cast<double>(total) / weight_sum)
                             : Evaluate(board, config_.weights);

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
    return result;  // move 保持 nullopt：确实无步可走
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
