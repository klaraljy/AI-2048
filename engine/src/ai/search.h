// Expectimax 搜索：为给定局面挑一个方向。
//
// 结构：
//   max 节点   —— 玩家选方向（最多 4 个分支，可以排序以尽快找到好的）
//   chance 节点 —— 随机生成新方块（空格数 x {2, 4}，最多 32 个分支）
//
// **expectimax 不能用 alpha-beta**：没有 min 节点，值不是有界的，
// 无法剪枝。唯一可做的是按**累计概率阈值**砍掉极不可能的分支。
//
// 关于确定性：搜索本身是确定的（同局面同配置给出同一个方向）。
// 但时间预算会引入机器相关的差异 —— 那是**搜索深度**的差异，
// 不是规则差异。跑批时用固定深度即可完全复现。

#ifndef AI2048_AI_SEARCH_H_
#define AI2048_AI_SEARCH_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "ai/evaluate.h"
#include "core/board.h"

namespace ai2048 {

// 置换表。**必须在一局之内跨步复用** ——
// 早期实现每步新建一张表，等于每步分配并清零 16MB，
// 单局 939 步就白花 8.8 秒（占该局总耗时的 98%），而搜索本身只要 0.15 秒。
//
// 用法：一局开始时构造一个（或 Reset），之后每步传给 SearchBestMove。
// 表内容只由棋盘与深度决定，不依赖搜索历史，所以跨步复用不影响正确性。
class TranspositionTable {
 public:
  static constexpr std::size_t kDefaultCapacity = 1u << 20;

  // use_symmetry_keys 见 SearchConfig::use_symmetry_keys 的说明 —— 它有正确性代价。
  explicit TranspositionTable(std::size_t capacity = kDefaultCapacity,
                              bool use_symmetry_keys = false);
  ~TranspositionTable();
  TranspositionTable(const TranspositionTable&) = delete;
  TranspositionTable& operator=(const TranspositionTable&) = delete;
  TranspositionTable(TranspositionTable&&) = delete;
  TranspositionTable& operator=(TranspositionTable&&) = delete;

  [[nodiscard]] bool Enabled() const noexcept;
  void Reset() noexcept;

  [[nodiscard]] std::optional<float> Lookup(std::uint64_t board, int depth, std::uint8_t kind);
  void Store(std::uint64_t board, int depth, std::uint8_t kind, float value);

  // 实现细节，定义在 .cpp 里。这里只是前置声明。
  // 必须放在可访问的位置，否则 .cpp 里无法定义这个嵌套类型。
  struct Impl;

 private:
  Impl* impl_;
};

struct SearchConfig {
  // 基础搜索深度。**偶数**：max 与 chance 逐层交替。
  //
  // ⚠️ **深度计法与公开基准不同，比较时务必换算。**
  //
  // 这里 depth 数的是**树层数**，每两层才等于"一步前瞻"：
  //     depth 2 = 1 步（玩家走一步 + 生成一个随机块）
  //     depth 8 = 4 步
  //     depth 16 = 8 步
  // 而公开基准（macroxue / nneonneo）报的 "depth 8" 指的是 **8 步前瞻**，
  // 换算过来相当于这里的 depth 16。
  //
  // 实测对照（100 局，本项目，见 docs/results/）：
  //     depth 4（2 步）  平均 17,644    到 2048 占 28%
  //     depth 6（3 步）  平均 30,715    到 2048 占 63%
  //     depth 8（4 步）  平均 39,455    到 2048 占 80%
  // 参照 macroxue depth 8（8 步）平均 711,769 —— 差了将近 20 倍。
  // 这个差距主要来自**前瞻步数**，不是启发式写法。
  int base_depth = 8;

  // 自适应深度：空格多时盘面宽松，可减一层省钱；
  // 空格少时每一步都关键，加一层。
  int depth_penalty_when_many_empty = 1;  // 空格 >= 阈值时 -1
  int many_empty_threshold = 8;
  int depth_bonus_when_few_empty = 1;  // 空格 <= 阈值时 +1
  int few_empty_threshold = 4;
  int min_depth = 2;
  int max_depth = 8;

  // 概率剪枝：累计概率低于此值的分支直接丢弃。
  bool enable_probability_cutoff = true;
  double probability_cutoff = 0.0005;

  // chance 节点的采样上限（0 = 不限制，枚举全部空格）。
  // 限制后按固定顺序保留前 N 个空格 —— 保持搜索的确定性。
  int chance_sample_limit = 0;

  // 时间预算（毫秒）。0 = 不限时。**超时也必须返回已完成搜索中的最佳合法步。**
  int time_budget_ms = 0;

  // 置换表是否使用 8 重对称规范键。
  //
  // ⚠️ 这个开关有**正确性代价**，不是纯粹的性能优化：
  // 评估函数是对称的（旋转/镜像后的盘面形状评分相同），但**搜索语义不是**
  // —— 2048 里"哪一对先合并"取决于扫描方向（左移先合并靠左的，右移先合并靠右的），
  // 所以旋转后的盘面最佳走子并不严格等价。用对称键会命中"看起来一样但语义不同"的条目。
  //
  // 实测节点数只降约 10%，收益远小于理论上的 8 倍，所以要谨慎使用。
  bool use_symmetry_keys = false;

  Weights weights;

  // 根节点上的方向偏好：抑制来回摆动。
  float consistency_bonus = 40.0F;
  float anti_oscillation_penalty = 60.0F;
  float direction_bias = 16.0F;
};

// 一个候选方向及其评分，供调试面板展示。
struct MoveEvaluation {
  Direction direction = Direction::kLeft;
  bool legal = false;
  float future_score = 0.0F;  // 走子后的局面搜索值
  float total_score = 0.0F;   // 加上根节点的方向调整后
  float quick_estimate = 0.0F;
};

struct SearchStats {
  std::uint64_t nodes = 0;
  std::uint64_t chance_nodes = 0;
  std::uint64_t tt_hits = 0;
  std::uint64_t tt_stores = 0;
  std::uint64_t pruned_by_probability = 0;
  int reached_depth = 0;
  bool timed_out = false;
  double elapsed_ms = 0.0;
};

struct SearchResult {
  std::optional<Direction> move;  // 无合法走子时为 nullopt
  std::vector<MoveEvaluation> evaluations;
  SearchStats stats;
};

// 选择最佳方向。
//
// table 可为 nullptr（那就完全不使用置换表）。同一个 table 应在**一局之内**
// 跨步复用 —— 见 TranspositionTable 的说明。
//
// last_move 用于根节点的方向偏好（同向小幅加分、反向小幅扣分），
// 抑制"左-右-左-右"这种来回摆动。不传则不加这项调整。
//
// **只有"无合法走子"时才返回空 move**：只要存在合法方向，就一定返回其中一个
// （必要时退化为浅层结果）。调用方会把空 move 当成"AI 无步可走"。
[[nodiscard]] SearchResult SearchBestMove(std::uint64_t board, const SearchConfig& config,
                                          TranspositionTable* table = nullptr,
                                          std::optional<Direction> last_move = std::nullopt);

// 当前配置下该局面的目标深度（供调试面板显示）。
[[nodiscard]] int AdaptiveDepth(std::uint64_t board, const SearchConfig& config) noexcept;

}  // namespace ai2048

#endif  // AI2048_AI_SEARCH_H_
