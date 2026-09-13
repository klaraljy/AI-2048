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
#include "core/game.h"

namespace ai2048 {

// 置换表。**必须在一局之内跨步复用** ——
// 早期实现每步新建一张表，等于每步分配并清零 16MB，
// 单局 939 步就白花 8.8 秒（占该局总耗时的 98%），而搜索本身只要 0.15 秒。
//
// 用法：一局开始时构造一个（或 Reset），之后每步传给 SearchBestMove。
//
// ⚠️ 表内容由「棋盘 + 深度 + 难度」共同决定（难度影响 chance 节点的概率分布）。
// 所以**换难度必须 Reset**：不重置会读到按另一套生成规则算出的值。
// 同一局之内难度不变，因此跨步复用仍然安全。
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

// ---------------------------------------------------------------------------
// ⚠️ 深度单位换算：**本引擎的 depth 是"树层数"，不是"玩家步数"**
//
// 这是全项目最容易出错的一处口径，必须用函数而不是脑算：
//
//     max 节点（玩家走一步）→ chance 节点（生成一个随机块）→ max 节点 → …
//
// 所以「走 n 步前瞻」= `LayersForMoves(n)` = 2n 层。
//
// | 树层数 | 玩家步数 | 说明 |
// |---|---|---|
// | 2  | 1 | 只看一步 |
// | 8  | 4 | 本项目长期基线用的档位 |
// | 16 | 8 | 公开基准（macroxue / nneonneo）说的 "depth 8" 是这个 |
//
// 为什么专门写函数：《AI算法设计2》第七节给的深度表用的是**玩家步数**
// （空位 10+ 建议 4、空位 1~3 建议 7~8）。照着字面把 "8" 填进来会得到
// 8 层 = 4 步 —— 只有文档建议的一半，而且**表面上完全看不出错**。
// ---------------------------------------------------------------------------

/** 玩家步数 → 树层数（每步 = max + chance 两层）。 */
[[nodiscard]] constexpr int LayersForMoves(int moves) noexcept {
  return moves <= 0 ? 0 : moves * 2;
}

/** 树层数 → 玩家步数（向下取整）。 */
[[nodiscard]] constexpr int MovesForLayers(int layers) noexcept {
  return layers <= 0 ? 0 : layers / 2;
}

/**
 * 把任意深度对齐到**合法档位**（偶数层）。
 *
 * 搜索只在偶数层上成对展开（max → chance），奇数层会停在 max 节点上，
 * 那一层只能判断合法性、拿不到任何局面信息 —— 白花时间。迭代加深的循环
 * 也因此只走偶数：`for (depth = 2; depth <= target; depth += 2)`。
 *
 * ⚠️ 由此产生一个**隐蔽的失效模式**：任何奇数层数的目标都会被向下取整，
 * 于是"空格少 +1"这类调整会被**静默吞掉**。实测过一次：把 max_depth 从 8
 * 提到 14 之后，10 局对拍的结果**逐局完全相同** —— 因为自适应算出 9 或 11，
 * 循环只跑到 8，加成从未生效。自适应深度的**每一项调整都应该是偶数**，
 * 否则改了等于没改。
 */
[[nodiscard]] constexpr int AlignToEvenLayers(int layers) noexcept {
  return layers <= 0 ? 0 : layers - (layers % 2);
}

struct SearchConfig {
  // 基础搜索深度，单位是**树层数**（换算见上）。**偶数**：max 与 chance 逐层交替。
  //
  // 实测对照（100 局，旧生成规则，见 docs/results/）：
  //     depth 4（2 步）  平均 17,644    到 2048 占 28%
  //     depth 6（3 步）  平均 30,715    到 2048 占 63%
  //     depth 8（4 步）  平均 39,455    到 2048 占 80%
  // 参照 macroxue depth 8（8 步 = 本引擎 depth 16）平均 711,769 —— 差了将近 20 倍。
  // 这个差距主要来自**前瞻步数**，不是启发式写法。
  int base_depth = 8;

  // 自适应深度。原先只看**空格数**，现在改成看**可移动方向数** —— 见
  // AdaptiveDepth 的说明：空位多但方向被堵死的局面是存在的。
  //
  // ⚠️ **每一项调整都必须是偶数。** 搜索只在偶数层成对展开，见
  // AlignToEvenLayers 的说明 —— 这里踩过一次：所有调整都是 ±1，
  // 结果 max_depth 从 8 提到 14 之后对拍结果逐局完全相同，加成从未生效。
  int depth_penalty_when_many_empty = 2;  // 空格 >= 阈值时 -2
  int many_empty_threshold = 8;
  int depth_bonus_when_few_empty = 2;  // 空格 <= 阈值时 +2
  int few_empty_threshold = 4;
  /** 可移动方向数 <= 此值时加成（危险局面，值得多搜）。 */
  int few_mobility_threshold = 2;
  int depth_bonus_when_few_mobility = 2;
  int min_depth = 2;
  // 自适应深度的上限。
  //
  // ⚠️ **必须大于 base_depth，否则加成会被完全夹掉。**
  // 这里踩过一次：上限原先是 8、基础深度也是 8，"方向少时 +2" 永远等于 8，
  // 等于自适应只减不增 —— 而且不报错，只是那一档功能从未生效过。
  //
  // 放开到 14（7 步）会让危险局面**真的**搜到 10~12 层，代价是那一步明显变慢。
  // 所以它必须配合时间预算使用：迭代加深只在**整轮跑完**时才采用结果，
  // 超时就退回上一轮，慢的那一步不会变成不可接受的延迟。
  // 对拍用的固定深度模式（--time 0）不受影响。
  int max_depth = 14;

  // 概率剪枝：累计概率低于此值的分支直接丢弃。
  bool enable_probability_cutoff = true;
  double probability_cutoff = 0.0005;

  /**
   * 单调性是否跳过空位（true = 新行为，与 nneonneo/macroxue 一致）。
   *
   * 存在的唯一目的是**把这个"修复"当成一次权重改动来对拍**：
   * 跳过空位会整体抬高单调性总分，等价于隐式放大 `monotonicity` 权重。
   * 需要能一键切回旧行为才能做单变量对照（见 evaluate.cpp 的说明）。
   */
  bool monotonicity_skips_empty = true;

  // chance 节点的采样上限（0 = 不限制，枚举全部空格）。
  //
  // ⚠️ `chance_sample_limit > 0` 时取的是**按索引顺序的前 N 个**空格
  // （索引 0 → 15，也就是从左上往右下），**有明确的空间偏差** ——
  // 棋盘上半部永远被展开、下半部永远被忽略。这个缺陷是 2026-09-13 才发现的。
  // 保留它只为兼容旧配置；新配置请用下面两项。
  int chance_sample_limit = 0;

  // 按**重要性**选择要展开的出生位置（2026-09-13 新增，取代上面那个上限）
  //
  // ⚠️ **实测无效，默认关闭（0+0）。** 启用后：depth 6 下平均分 43,862 → 35,384
  //    （−19%），而**「最深达到」两边都是 10 层** —— 它减少节点换来的只是
  //    "少看"，没有换来深度。根因是这个实现有**归一化偏差**：
  //    chance 节点末尾的 `total / weight_sum` 只累计**被选中**的分支，
  //    所以它算的是"选中子集的归一化平均"，不是真实期望值的无偏估计 ——
  //    没被采样到的格子里的坏情况 AI 看不见，于是**系统性低估风险**。
  //    要修就得做成真正的重要性采样（按 1/选中概率 加权），那会引入方差。
  //
  // 保留实现与这段结论：它否掉了"分支因子是搜索深度瓶颈"这个假设
  // （A3 才是），这个否定本身有价值。要用请先修归一化偏差。
  //
  // 做法（启用时）：只展开 **重要格 + 抽样格** 两部分，
  //   1. 用当时的格权重排序，取最靠前的 `chance_important_cells` 个 —— 确定性。
  //   2. 从剩下的空格里**按权重无偏抽** `chance_sample_cells` 个 —— 随机，
  //      但用**按 (棋盘, 深度) 播种的独立 RNG**，所以同一局面永远得到同一棵树，
  //      「同种子逐字节一致」不受影响；也**不消耗游戏 RNG**。
  // 两项都为 0 时枚举全部空格（**当前默认**）。
  int chance_important_cells = 0;
  int chance_sample_cells = 0;

  // AI 搜索时假设的**生成难度**。
  //
  // 这决定 chance 节点里"新方块落在各空格的相对概率"。默认 kNormal
  // （全盘均匀）= 标准 2048，也是历史基准使用的假设。
  //
  // 为什么必须可配：难度改的就是落点分布。如果 AI 一律按均匀分布评估，
  // 那么 hard 档下它会**低估**"新块贴着自己最大块出现"的风险 ——
  // 世界模型与实际游戏不符，走子会偏乐观。
  //
  // ⚠️ 它参与搜索结果，所以**换难度必须清空置换表**
  // （见 game.h 的说明；服务端在收到 configure 时处理）。
  ai2048::Difficulty difficulty = ai2048::Difficulty::kNormal;

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

  // 学习出来的叶子评估（可选）。为空时用上面的手写 weights。
  //
  // ## 为什么用裸函数指针 + void*，而不是 std::function 或直接引用 ValueNetwork
  //
  // 直接引用 learn::ValueNetwork 会让 ai2048_core **反向依赖** ai2048_train，
  // 而 train 是依赖 core 的 —— 成环。裸函数指针把依赖留在调用方
  // （CLI 负责把网络绑上去），core 完全不知道网络的存在。
  //
  // ## ⚠️ 单位必须与 weights 同尺度
  //
  // search 内部的评价值与手写启发式在同一量纲上（原始分的量级，上万），
  // 而 n-tuple 网络输出的是**归一化分**（分/1000，量级 0~50）。
  // 直接接上不会报错，但 consistency_bonus / anti_oscillation_penalty /
  // direction_bias 这些几十量级的调节项会瞬间变成主导项，搜索行为会坏掉。
  // 所以适配器要乘回 kScoreScale —— 见 cli/main.cpp 的 MakeNetworkEvaluator。
  //
  // 另外请注意：置换表的内容现在取决于 leaf_evaluator，**换评估函数必须
  // Reset 表**，理由与"换难度必须 Reset"相同（表里存的是旧评估算出的值）。
  float (*leaf_evaluator)(void* context, std::uint64_t board, bool terminal) = nullptr;
  void* leaf_evaluator_context = nullptr;

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

/**
 * 按难度算出**每个格子被打上新方块的相对权重**。
 *
 * 这是 AI 的"世界模型"：chance 节点用它给各分支加权，从而让搜索的假设
 * 与实际生成规则一致。非空格子的权重定义为 0。
 *
 * 暴露出来是为了**可测试** —— 它与 core/game.cpp 的 Game::SpawnRandomTile
 * 是同一套规则的两种表达，写错了不会有任何报错，只会让 AI 悄悄变弱或变乐观。
 * tests/difficulty_test.cpp 会拿它和实际生成分布对拍。
 *
 * @return 长度 kCellCount 的数组，索引 = row * kBoardSize + col
 */
[[nodiscard]] std::array<double, kCellCount> SpawnWeights(std::uint64_t board,
                                                          Difficulty difficulty);

/**
 * chance 节点上生成 4 的概率（其余为 2）。必须与实际生成规则一致。
 *
 * ⚠️ **这个方法存在的唯一理由是防止它再次写死。** 位置权重（SpawnWeights）
 * 早就按难度接上了，数值概率却漏了 —— 在 2026-09 把 P(4) 改成按难度分档
 * （10/15/20%）之后，搜索里仍写着 0.9 / 0.1 用了很久，没有任何报错：
 *
 * | 难度 | 实际 P(4) | 曾经写死 | 偏差 |
 * |---|---|---|---|
 * | easy   | 10% | 10% | 无 |
 * | normal | 15% | 10% | 低估 4 |
 * | hard   | 20% | 10% | 低估 4 一倍 |
 *
 * 后果不是"略弱"，而是**系统性偏乐观**：4 比 2 难缠（要多合一次才等价），
 * 低估 4 等于告诉 AI"冒险划算"，于是它更容易把自己堵死。
 *
 * 暴露出来是为了**可测试**：tests/difficulty_test.cpp 会拿它和
 * Game 实际生成的 4 占比对拍。两边是同一套规则的两个副本，写错了不报错。
 */
[[nodiscard]] double FourSpawnProbability(Difficulty difficulty) noexcept;

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
