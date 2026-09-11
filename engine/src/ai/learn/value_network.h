// n-tuple 价值网络 —— 学出来的盘面评估，替代手写启发式。
//
// ## 为什么要有这个
//
// 手写启发式（evaluate.cpp）已经到顶了：实测多给 6 倍算力只换来 ~1.1 倍分数
// （d4 42,792 → d8 46,851），而且 8192 到达率在 100 局上仍是 0%。
// 瓶颈不在搜索，在**评估函数认不出什么盘面能走到 8192**。
//
// 这条路（Szubert 2014 的 n-tuple + TD 学习）不手搓权重，而是从
// **自我对弈的真实得分**里学。
//
// ## 网络结构
//
// 把 16 个格子分成若干 **tuple**（一组固定的位置），每个 tuple 的取值
// 组合成一个下标，去查一张权重表，最后把各 tuple 的查表结果相加：
//
//     价值(盘面) = Σ_tuple W[tuple][该 tuple 的格子指数组合]
//
// 单个 tuple 的表大小是 16^长度：
//     4-tuple →      65,536 项（256 KB）
//     5-tuple →   1,048,576 项（  4 MB）
//     6-tuple →  16,777,216 项（ 67 MB）
//
// ## 容量与样本效率的取舍（C1 → C2 的核心认识）
//
// C1 只用 4 个 4-tuple（26 万参数），实测只能学到约 2,300 分。
// 原因是**表达能力**：4 个 tuple 全在同一行内，看不到跨行结构，
// 而 2048 的棋力核心（蛇形、跨行单调链）恰恰是跨行的。
//
// 提升有两条路，可以并用：
//   1. **加数量**：多个不同形状的 4-tuple（行/列/2×2/L 形/对角）。
//      参数增长线性，每个样本能更新到所有 tuple —— 样本效率高。
//   2. **加长度**：6-tuple 能表达跨 6 格的形状，容量大得多。
//      但每个样本只更新到很少的权重，需要**更多局数**才收敛。
//
// 所以 C2 先走"加数量"（MixedTuples，便宜且立竿见影），
// 再叠加"加长度"（WithSixTuples）。
//
// ## 权重表布局
//
// 所有 tuple 的权重放在**一块连续数组**里，用一张偏移表寻址：
//     offset = offsets[tuple_index] + 该 tuple 的组合下标
// 一次查表就是一次数组访问，没有间接跳转 —— 训练和推断都靠它提速。

#ifndef AI2048_AI_LEARN_VALUE_NETWORK_H_
#define AI2048_AI_LEARN_VALUE_NETWORK_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/board.h"

namespace ai2048::learn {

/** 单个 tuple 最多覆盖几个格子。6 是内存与表达能力的折中（见文件头）。 */
inline constexpr int kMaxTupleLength = 6;

/** 一个 tuple：一组格子位置（线性下标 0..15）。 */
struct Tuple {
  std::array<int, kMaxTupleLength> cells{};
  int length = 0;  // 实际使用前 length 个

  /** 该 tuple 的组合数：16^length。 */
  [[nodiscard]] std::uint32_t StateCount() const noexcept;

  /** 提取该 tuple 在当前盘面上的组合下标。 */
  [[nodiscard]] std::uint32_t Index(std::uint64_t board) const noexcept;
};

/** 造一个 tuple。
 *  @param cells 格子下标，长度必须等于 count
 *  @param count 格子数（2..kMaxTupleLength）
 */
[[nodiscard]] Tuple MakeTuple(const std::vector<int>& cells);

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
   * @param tuples tuple 的集合。空的话会被替换成一个占位 tuple（见 .cpp）。
   */
  explicit ValueNetwork(std::vector<Tuple> tuples);

  /** C1 布局：按行切成 4 个 4-tuple。表达能力不足，只用于对照。 */
  [[nodiscard]] static std::vector<Tuple> RowTuples();

  /**
   * C2 布局 A：混合形状的 4-tuple（行 / 列 / 2×2 / L 形 / 对角），共 12 个。
   * 参数约 79 万。
   */
  [[nodiscard]] static std::vector<Tuple> MixedTuples();

  /**
   * C2 布局 B：MixedTuples 再加若干个 6-tuple。
   * @param six_tuple_count 6-tuple 的数量（0~4）。每个约 67MB。
   */
  [[nodiscard]] static std::vector<Tuple> WithSixTuples(int six_tuple_count);

  /**
   * C2 布局 C：MixedTuples 再加 12 个**真蛇形前缀**（长度 4/5/6）。
   *
   * 与 WithSixTuples 的区别是它抓的是**换行处的相邻关系** ——
   * 蛇形走到行尾折返时的那对格子，横 tuple 和竖 tuple 都看不到。
   * 实测已证明"加大尺寸"（mixed → six2，参数 ×44）没有收益，
   * 所以这里换的是**形状**，不是尺寸。
   *
   * 参数约 157 万，内存约 6MB —— 比 six2 的 131MB 小得多，训练也快得多。
   */
  [[nodiscard]] static std::vector<Tuple> SerpentineTuples();

  /**
   * 一次评估的"痕迹"：价值 + 本次用到的权重下标。
   *
   * 训练时必须走带 trace 的路径 —— TD 更新要往这些下标上写，
   * 事后重算一遍下标会慢一倍。
   */
  struct Trace {
    double value = 0.0;
    std::array<std::uint32_t, 32> offsets{};  // 每个 tuple 一个权重下标
    int count = 0;
  };

  /**
   * 带痕迹的评估。
   *
   * @param terminal 是否是**终局**。为真时价值定义为 0 ——
   *   也就是"从这里之后再也拿不到分了"。
   *
   *   这不是可选的细节。不区分终局的话，最后一步的 afterstate 会被当成
   *   "后面还有分可拿"，它的价值朝着那一步的奖励收敛而不是朝着 0；
   *   这个偏置顺着 TD 链污染整张表，表现就是**加容量、加局数都卡在
   *   同一个分数上**（实测 4-tuple → 6-tuple，参数 ×85，分数都是 2,300）。
   */
  [[nodiscard]] Trace EvaluateWithTrace(std::uint64_t board, bool terminal = false) const noexcept;

  /** 盘面价值（归一化后）。terminal 的含义同上。 */
  [[nodiscard]] double Evaluate(std::uint64_t board, bool terminal = false) const noexcept;

  /** 按 trace 记录的权重下标做一次梯度上升。 */
  void ApplyGradient(const Trace& trace, double delta, double learning_rate) noexcept;

  /** 所有参数置零（训练起点）。网络输出 0 是一个中性起点，比随机权重稳。 */
  void Reset() noexcept;

  [[nodiscard]] std::size_t tuple_count() const noexcept { return tuples_.size(); }
  [[nodiscard]] std::size_t parameter_count() const noexcept { return weights_.size(); }
  /** 参数占用的内存（MB），用于在训练前提示体积。 */
  [[nodiscard]] double parameter_megabytes() const noexcept {
    return static_cast<double>(weights_.size()) * sizeof(float) / (1024.0 * 1024.0);
  }

  /** 保存/加载权重（二进制格式，含 tuple 布局，见 .cpp 的说明）。 */
  [[nodiscard]] bool Save(const std::string& path, std::string* error) const;
  [[nodiscard]] bool Load(const std::string& path, std::string* error);

 private:
  std::vector<Tuple> tuples_;
  /** 每个 tuple 在 weights_ 里的起始偏移。 */
  std::vector<std::uint32_t> offsets_;
  std::vector<float> weights_;
};

}  // namespace ai2048::learn

#endif  // AI2048_AI_LEARN_VALUE_NETWORK_H_
