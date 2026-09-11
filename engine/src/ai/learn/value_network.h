// n-tuple 价值网络 —— 学出来的盘面评估，替代手写启发式。
//
// ## 为什么要有这个
//
// 手写启发式（evaluate.cpp）已经到顶了：实测多给 6 倍算力只换来 ~1.1 倍分数，
// 而且 8192 到达率在 100 局上仍是 0%。瓶颈不在搜索，在**评估函数认不出
// 什么盘面能走到 8192**。参考实现把 75% 的权重压在单一的蛇形项上，
// 也说明手写项的信息量就那么多。
//
// 这条路（Szubert 2014 的 n-tuple + TD 学习）不手搓权重，而是从
// **自我对弈的结果**里学：网络预测"这个盘面最终能拿多少分"，
// 用实际结果纠正它。它不依赖我猜的系数。
//
// ## 网络结构
//
// 把 16 个格子分成若干 **tuple**（一组固定的位置），每个 tuple 的取值
// 组合成一个下标，去查一张权重表，最后把各 tuple 的查表结果相加：
//
//     价值(盘面) = Σ_tuple W[tuple][该 tuple 的格子指数组合]
//
// 单个 4-tuple（4 个格子、指数 0..15）的下标空间是 16^4 = 65536。
// 这是标准做法：tuple 越小、表越小、泛化越好，但表达能力也越弱。
// C1 阶段刻意用**单 tuple**（约 12.8 万参数）先验证"能不能学"，
// 学得动再往上加 tuple 数量与长度。
//
// ## 权重表布局
//
// 所有 tuple 的权重放在**一块连续数组**里，按下标直接寻址：
//     offset = tuple_index * kTupleStates + pattern_index
// 这样一次查表就是一次数组访问，没有间接跳转 —— 训练和推断都靠它提速。

#ifndef AI2048_AI_LEARN_VALUE_NETWORK_H_
#define AI2048_AI_LEARN_VALUE_NETWORK_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/board.h"

namespace ai2048::learn {

/** 一个 tuple：一组格子位置（下标 0..15）。 */
struct Tuple {
  std::array<int, 4> cells{};  // 4 个格子的线性下标

  /** 提取该 tuple 在当前盘面上的组合下标（0 .. 16^4-1）。 */
  [[nodiscard]] std::uint32_t Index(std::uint64_t board) const noexcept;
};

/** 单个 4-tuple 的组合数：16^4 = 65536。 */
inline constexpr std::uint32_t kTupleStates = 16 * 16 * 16 * 16;

/**
 * n-tuple 价值网络。
 *
 * 输出是**归一化后的分数**（见 kScoreScale），不是原始分数 ——
 * 原始分数能到几十万，直接做回归时梯度尺度太差。
 */
class ValueNetwork {
 public:
  /** 分数缩放：网络预测的是 分数 / kScoreScale。 */
  static constexpr double kScoreScale = 1000.0;

  /**
   * 建一个网络。
   * @param tuples tuple 的集合（每个 4 格）。至少 1 个。
   */
  explicit ValueNetwork(std::vector<Tuple> tuples);

  /**
   * 标准布局：把 4x4 盘面按行切成 4 个 4-tuple。
   *
   * 这是最朴素的切法，也最容易被验证 —— C1 只要证明"能学"，
   * 不需要一开始就上最优的 tuple 设计（那是 C2/C3 的事）。
   */
  [[nodiscard]] static std::vector<Tuple> RowTuples();

  /** 盘面价值（归一化后）。 */
  [[nodiscard]] double Evaluate(std::uint64_t board) const noexcept;

  /**
   * 带梯度信息的评估：顺便记录本次用到的权重下标。
   *
   * TD 更新要往这些下标上写，所以训练时必须走这条路 ——
   * 事后重算一遍下标会慢一倍。
   */
  struct Trace {
    double value = 0.0;
    std::array<std::uint32_t, 8> offsets{};  // 每个 tuple 一个权重下标
    int count = 0;
  };

  [[nodiscard]] Trace EvaluateWithTrace(std::uint64_t board) const noexcept;

  /** 按 trace 记录的权重下标做一次梯度上升。 */
  void ApplyGradient(const Trace& trace, double delta, double learning_rate) noexcept;

  /** 所有参数置零（训练起点）。网络输出 0 是一个中性起点，比随机权重稳。 */
  void Reset() noexcept;

  [[nodiscard]] std::size_t tuple_count() const noexcept { return tuples_.size(); }
  [[nodiscard]] std::size_t parameter_count() const noexcept { return weights_.size(); }

  /** 保存/加载权重（简单的二进制格式，见 .cpp 里的说明）。 */
  [[nodiscard]] bool Save(const std::string& path, std::string* error) const;
  [[nodiscard]] bool Load(const std::string& path, std::string* error);

 private:
  std::vector<Tuple> tuples_;
  std::vector<float> weights_;
};

}  // namespace ai2048::learn

#endif  // AI2048_AI_LEARN_VALUE_NETWORK_H_
