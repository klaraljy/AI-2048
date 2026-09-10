#include "core/rng.h"

namespace ai2048 {

namespace {

// splitmix64：用于把单个种子扩展成 4 个非零状态字。
// 直接把 seed 拆成 4 个字会产生大量质量很差的状态（尤其是小种子），
// 所以先过一遍 splitmix64。
[[nodiscard]] std::uint64_t SplitMix64(std::uint64_t* x) noexcept {
  std::uint64_t z = (*x += 0x9E37'79B9'7F4A'7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
  return z ^ (z >> 31);
}

[[nodiscard]] constexpr std::uint64_t RotateLeft(std::uint64_t x, int k) noexcept {
  return (x << k) | (x >> (64 - k));
}

}  // namespace

Rng::Rng(std::uint64_t seed) noexcept {
  std::uint64_t x = seed;
  for (std::uint64_t& word : state_) {
    word = SplitMix64(&x);
  }
}

std::uint64_t Rng::NextU64() noexcept {
  const std::uint64_t result = RotateLeft(state_[1] * 5, 7) * 9;
  const std::uint64_t t = state_[1] << 17;

  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = RotateLeft(state_[3], 45);

  return result;
}

std::uint64_t Rng::NextBounded(std::uint64_t bound) noexcept {
  if (bound <= 1) return 0;

  // Lemire 拒绝采样：算出阈值，丢弃会造成偏差的那一小段区间。
  const std::uint64_t threshold = (0 - bound) % bound;
  while (true) {
    const std::uint64_t r = NextU64();
    if (r >= threshold) return r % bound;
  }
}

bool Rng::Chance(std::uint64_t numerator, std::uint64_t denominator) noexcept {
  if (numerator == 0) return false;
  if (numerator >= denominator) return true;
  return NextBounded(denominator) < numerator;
}

}  // namespace ai2048
