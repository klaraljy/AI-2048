// 决策影响力探针：度量每一项在**同一局面的候选走子之间**的差异。
//
// 为什么不是"绝对量级"：某一项如果在所有候选走子上取值都差不多，
// 那它对**选择**毫无影响 —— 哪怕它的绝对量级占比 50%。
// 真正决定决策的是"项 × 候选之间的散布"，即"换一个走子，这一项变多少"。
//
// 这里对每个局面取 4 个方向的 afterstate，对每一项算 (max-min) / (|均值|+1)，
// 既看绝对散布，也看相对散布。
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

struct Stat {
  std::string name;
  double spread_sum = 0.0;   // Σ(max-min) 跨候选
  double absmean_sum = 0.0;  // Σ|候选均值|
  double max_spread = 0.0;
  long long n = 0;
};

struct Acc {
  std::vector<Stat> stats;

  void Add(const char* name, double spread, double abs_mean) {
    for (Stat& s : stats) {
      if (s.name == name) {
        s.spread_sum += spread;
        s.absmean_sum += abs_mean;
        s.max_spread = std::max(s.max_spread, spread);
        ++s.n;
        return;
      }
    }
    Stat s;
    s.name = name;
    s.spread_sum = spread;
    s.absmean_sum = abs_mean;
    s.max_spread = spread;
    s.n = 1;
    stats.push_back(s);
  }
};

}  // namespace

int main(int argc, char** argv) {
  const int depth = (argc > 1) ? std::atoi(argv[1]) : 4;
  const int games = (argc > 2) ? std::atoi(argv[2]) : 3;

  const Weights weights;
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
      // 四个方向的 afterstate（这就是根节点真正比较的东西）。
      std::vector<EvaluationBreakdown> cands;
      for (const Direction d :
           {Direction::kUp, Direction::kDown, Direction::kLeft, Direction::kRight}) {
        const MoveResult move = ApplyMove(game.board(), d);
        if (move.moved) cands.push_back(EvaluateWithBreakdown(move.board, weights));
      }
      if (cands.size() >= 2) {
        auto measure = [&](const char* name, float EvaluationBreakdown::*field) {
          double lo = 1e300, hi = -1e300, sum = 0.0;
          for (const EvaluationBreakdown& c : cands) {
            const double v = static_cast<double>(c.*field);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
            sum += v;
          }
          acc.Add(name, hi - lo, std::abs(sum / static_cast<double>(cands.size())));
        };
        measure("empty", &EvaluationBreakdown::empty);
        measure("monotonicity", &EvaluationBreakdown::monotonicity);
        measure("smoothness", &EvaluationBreakdown::smoothness);
        measure("merge", &EvaluationBreakdown::merge);
        measure("snake", &EvaluationBreakdown::snake);
        measure("corner", &EvaluationBreakdown::corner);
        measure("corner_control", &EvaluationBreakdown::corner_control);
        measure("edge_support", &EvaluationBreakdown::edge_support);
        measure("gradient", &EvaluationBreakdown::gradient);
        measure("mobility", &EvaluationBreakdown::mobility);
        measure("islands", &EvaluationBreakdown::islands);
        measure("total", &EvaluationBreakdown::total);
      }

      const SearchResult decision = SearchBestMove(game.board(), config, &table, last_move);
      if (!decision.move.has_value()) break;
      if (!game.Step(*decision.move).moved) break;
      last_move = decision.move;
    }
  }

  std::printf("样本 = %lld 个局面（depth %d，%d 局）\n", acc.stats.empty() ? 0 : acc.stats[0].n,
              depth, games);
  std::printf(
      "度量：同一局面下 4 个候选走子之间 该项取值的 (max-min) —— 这才是它对**决策**的影响力\n\n");
  std::printf("%-18s %12s %12s %10s\n", "项", "平均散布", "最大散布", "散布/|均值|");
  std::printf("%s\n", std::string(56, '-').c_str());
  double total_spread = 0.0;
  for (const Stat& s : acc.stats) {
    if (s.name == "total") total_spread = s.spread_sum / static_cast<double>(s.n);
  }
  for (const Stat& s : acc.stats) {
    const double spread = s.spread_sum / static_cast<double>(s.n);
    const double absmean = s.absmean_sum / static_cast<double>(s.n);
    std::printf("%-18s %12.1f %12.1f %10.2f\n", s.name.c_str(), spread, s.max_spread,
                absmean > 1e-9 ? spread / absmean : 0.0);
  }
  std::printf("\n总分的平均散布 = %.1f\n", total_spread);
  if (total_spread > 0) {
    std::printf("\n各项占总散布的比例（决策权重）：\n");
    for (const Stat& s : acc.stats) {
      if (s.name == "total") continue;
      const double spread = s.spread_sum / static_cast<double>(s.n);
      std::printf("  %-18s %5.1f%%\n", s.name.c_str(), 100.0 * spread / total_spread);
    }
  }
  return 0;
}
