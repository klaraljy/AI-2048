// 局面评估：expectimax 的叶子函数。
//
// 这里**不使用游戏真实分数**作为评分项。
// 真实分数会过度鼓励"立刻合并"，而 2048 里延迟合并往往收益更大
// （nneonneo 的原话：the actual score ... is too heavily weighted in favor of
// merging tiles when delayed merging could produce a large benefit）。
//
// 评分项都是"局面形状"的度量：
//   empty        空格数 —— 最基础的一条，没有空格就没有腾挪空间
//   monotonicity 单调性 —— 行/列上的牌按数值单调排列
//   smoothness   平滑度 —— 相邻牌的数值接近
//   merge        合并潜力 —— 相邻等值对的数量
//   corner       最大牌靠角
//   snake        蛇形权重 —— 把"单调 + 大牌在角 + 无孔洞"编码成一个模板
//
// 权重来自公开的调参结论（nneonneo / xificurk 用 CMA-ES 调出来的量级），
// 本项目后续会用固定的种子集自己再调一轮。

#ifndef AI2048_AI_EVALUATE_H_
#define AI2048_AI_EVALUATE_H_

#include <cstdint>

#include "core/board.h"

namespace ai2048 {

// 权重来自**本项目自己的种子集扫描**（见 docs/results/），不是照抄公开值。
//
// 调优过程记录（depth 4，40 局，seeds-v1）：
//   初始值（照公开论文量级拍的）        18,023
//   mono 47 -> 250                     30,825   ← 单调性是最大的杠杆
//   empty 270 -> 400                   34,121
//   merge 18 -> 30                     34,363
// 反面记录（都试过且更差，不要再走回头路）：
//   snake 放大到 2.0 / 6.0             13,797 ~ 17,703
//   empty 降到 150                     23,500
//   empty_late 降到 600 / 800          29,104 ~ 30,076
//   mono 超过 320                      27,043
struct Weights {
  // 空格越多越好。残局每一格都更珍贵，所以 empty_late 用于空格很少时。
  //
  // ⚠️ empty_late **不要调小**：降到 600/800 会明显变差 ——
  // 残局阶段的空间比早期更值钱，这个直觉是对的。
  float empty = 400.0F;
  float empty_late = 700.0F;
  int empty_late_threshold = 3;

  // 单调性：按牌面等级加权，让"大数字不单调"被重罚。
  // **这是本项目里最大的一个杠杆**（47 -> 250 带来 71% 的提升）。
  // 直觉：2048 的分数几乎完全由"能不能维持一条单调的链"决定，
  // 而单调性项正是唯一直接度量这件事的项。
  float monotonicity = 250.0F;

  // 平滑度：相邻牌数值接近。
  float smoothness = 32.0F;

  // 合并潜力：相邻等值对。
  float merge = 30.0F;

  // 最大牌靠角的奖励（按空格数缩放）。
  // 注意：单调性项已经隐含了"大牌靠角"（角是单调链的端点），
  // 所以这一项调大反而有害（试过 5000/6000，都更差）。
  float corner = 2200.0F;

  // 蛇形权重：把整盘牌按一条蛇形路径加权求和。
  //
  // ⚠️ 这一项**没有起到预期作用**：放大到 2.0 或 6.0 都明显变差。
  // 可能的原因是这个蛇形模板的绝对量级被其他项淹没了，
  // 或者"蛇形 + 高权重"压制了必要的短期合并。保持小值。
  float snake = 1.5F;

  // 最大牌本身的等级奖励，鼓励往高处走。
  float max_tile = 12.0F;
};

// 评估一个局面（**不走子**的局面，即 chance 节点之下的状态）。
[[nodiscard]] float Evaluate(std::uint64_t board, const Weights& weights) noexcept;

// 供调试面板使用的分项拆解。
struct EvaluationBreakdown {
  int empty_cells = 0;
  float empty = 0.0F;
  float monotonicity = 0.0F;
  float smoothness = 0.0F;
  float merge = 0.0F;
  float corner = 0.0F;
  float snake = 0.0F;
  float max_tile = 0.0F;
  float total = 0.0F;
};

[[nodiscard]] EvaluationBreakdown EvaluateWithBreakdown(std::uint64_t board,
                                                        const Weights& weights) noexcept;

}  // namespace ai2048

#endif  // AI2048_AI_EVALUATE_H_
