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

struct Weights {
  // 空格越多越好。残局每一格都更珍贵，所以 empty_late 用于空格很少时。
  float empty = 270.0F;
  float empty_late = 700.0F;
  int empty_late_threshold = 3;

  // 单调性：按牌面等级加权，让"大数字不单调"被重罚。
  float monotonicity = 47.0F;

  // 平滑度：相邻牌数值接近。
  float smoothness = 32.0F;

  // 合并潜力：相邻等值对。
  float merge = 18.0F;

  // 最大牌靠角的奖励（按空格数缩放）。
  float corner = 2200.0F;

  // 蛇形权重：把整盘牌按一条蛇形路径加权求和。
  float snake = 0.35F;

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
