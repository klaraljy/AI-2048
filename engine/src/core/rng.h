// 引擎自己的伪随机数发生器。
//
// 为什么不用 <random>：本项目的核心承诺是"同一种子 -> 逐字节一致的结果"，
// 而标准库**不保证**任何具体引擎的算法、也不保证 uniform_int_distribution
// 在各个实现上的映射方式。换一个标准库版本就可能让历史分数全部失效。
//
// xoshiro256** 是公有领域算法，输出完全由这段代码决定，任何平台、
// 任何编译器、任何标准库版本上都产生同一个序列。

#ifndef AI2048_CORE_RNG_H_
#define AI2048_CORE_RNG_H_

#include <cstdint>

namespace ai2048 {

class Rng {
 public:
  // seed 为 0 时仍然会得到一个有效状态（splitmix64 负责把 0 打散）。
  explicit Rng(std::uint64_t seed) noexcept;

  [[nodiscard]] std::uint64_t NextU64() noexcept;

  // 返回 [0, bound) 内的均匀整数。bound 必须 > 0。
  //
  // 用拒绝采样而不是取模 —— 取模会引入偏差，而偏差会让"同一种子的
  // 两次运行"之外的一切统计都不可信。
  [[nodiscard]] std::uint64_t NextBounded(std::uint64_t bound) noexcept;

  // 以 numerator/denominator 的概率返回 true。
  [[nodiscard]] bool Chance(std::uint64_t numerator, std::uint64_t denominator) noexcept;

 private:
  std::uint64_t state_[4];
};

}  // namespace ai2048

#endif  // AI2048_CORE_RNG_H_
