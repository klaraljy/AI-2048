// 量级标定探针：把评估函数各项在**真实对局局面**上的贡献量级打出来。
//
// 目的：权重（400 / 250 / 2200 …）只有在**各项原始量级可比**的前提下才谈得上
// "相对重要性"。如果某一项的原始值天然比别的大两个数量级，那么"权重"排出来的
// 优先级是偶然的，而不是设计出来的 —— 那意味着有大得多的提升空间。
//
// 这个探针几秒钟出结果，不需要跑分，所以不受"一小时只能跑 260 局"的限制。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "ai/evaluate.h"
#include "ai/search.h"
#include "core/board.h"
#include "core/game.h"

using namespace ai2048;

namespace {

struct Row {
  std::string name;
  double abs_sum = 0.0;  // Σ|原始值|
  double abs_max = 0.0;
  int nonzero = 0;
  double signed_sum = 0.0;
};

struct Acc {
  std::vector<Row> rows;
  long long samples = 0;

  void Add(const char* name, double raw) {
    for (Row& row : rows) {
      if (row.name == name) {
        row.abs_sum += std::abs(raw);
        row.signed_sum += raw;
        row.abs_max = std::max(row.abs_max, std::abs(raw));
        if (raw != 0.0) ++row.nonzero;
        return;
      }
    }
    Row row;
    row.name = name;
    row.abs_sum = std::abs(raw);
    row.signed_sum = raw;
    row.abs_max = std::abs(raw);
    row.nonzero = raw != 0.0 ? 1 : 0;
    rows.push_back(row);
  }
};

}  // namespace

int main(int argc, char** argv) {
  const int depth = (argc > 1) ? std::atoi(argv[1]) : 4;
  const int games = (argc > 2) ? std::atoi(argv[2]) : 3;

  const Weights weights;  // 默认权重
  Acc acc;

  for (int seed = 1; seed <= games; ++seed) {
    Game game(static_cast<std::uint64_t>(seed), Difficulty::kNormal);
    SearchConfig config;
    config.base_depth = depth;
    config.min_depth = 2;
    config.max_depth = depth;
    config.time_budget_ms = 0;
    TranspositionTable table(1u << 18, false);
    std::optional<Direction> last_move;

    while (!game.game_over()) {
      const EvaluationBreakdown b = EvaluateWithBreakdown(game.board(), weights);
      ++acc.samples;
      // 记录的是**原始项**（除以权重还原），这样看的是量级本身。
      acc.Add("empty_cells", static_cast<double>(b.empty_cells));
      acc.Add("monotonicity_raw", b.monotonicity / weights.monotonicity);
      acc.Add("smoothness_raw", b.smoothness / weights.smoothness);
      acc.Add("merge_raw", b.merge / weights.merge);
      acc.Add("snake_raw", b.snake / weights.snake);
      acc.Add("corner_control_raw", b.corner_control / weights.corner_control);
      acc.Add("edge_support_raw", b.edge_support / weights.edge_support);
      acc.Add("gradient_raw", b.gradient / weights.gradient);
      acc.Add("max_exponent", static_cast<double>(b.max_tile) / weights.max_tile);
      acc.Add("mobility_raw", static_cast<double>(b.mobility_directions));

      acc.Add("W_empty", b.empty);
      acc.Add("W_monotonicity", b.monotonicity);
      acc.Add("W_smoothness", b.smoothness);
      acc.Add("W_merge", b.merge);
      acc.Add("W_snake", b.snake);
      acc.Add("W_corner", b.corner);
      acc.Add("W_corner_control", b.corner_control);
      acc.Add("W_edge_support", b.edge_support);
      acc.Add("W_gradient", b.gradient);

      const SearchResult decision = SearchBestMove(game.board(), config, &table, last_move);
      if (!decision.move.has_value()) break;
      if (!game.Step(*decision.move).moved) break;
      last_move = decision.move;
    }
  }

  std::printf("样本数 = %lld （depth %d，%d 局）\n\n", acc.samples, depth, games);
  std::printf("%-20s %14s %14s %10s\n", "项", "平均|原始值|", "最大|原始值|", "非零占比");
  std::printf("%s\n", std::string(62, '-').c_str());
  for (const Row& row : acc.rows) {
    if (row.name.rfind("W_", 0) == 0) continue;
    std::printf("%-20s %14.2f %14.2f %9.1f%%\n", row.name.c_str(),
                row.abs_sum / static_cast<double>(acc.samples), row.abs_max,
                100.0 * static_cast<double>(row.nonzero) / static_cast<double>(acc.samples));
  }

  std::printf("\n加权后各项的**平均绝对贡献**（这才是它们对总分的实际影响力）：\n\n");
  std::printf("%-20s %14s %10s\n", "项", "平均|贡献|", "占比");
  std::printf("%s\n", std::string(48, '-').c_str());
  double total = 0.0;
  for (const Row& row : acc.rows) {
    if (row.name.rfind("W_", 0) != 0) continue;
    total += row.abs_sum / static_cast<double>(acc.samples);
  }
  for (const Row& row : acc.rows) {
    if (row.name.rfind("W_", 0) != 0) continue;
    const double mean = row.abs_sum / static_cast<double>(acc.samples);
    std::printf("%-20s %14.1f %9.1f%%\n", row.name.c_str(), mean,
                total > 0 ? 100.0 * mean / total : 0.0);
  }
  return 0;
}
