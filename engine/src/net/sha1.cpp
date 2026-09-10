#include "net/sha1.h"

#include <array>
#include <cstring>

namespace ai2048::net {

namespace {

[[nodiscard]] constexpr std::uint32_t RotateLeft(std::uint32_t value, int bits) noexcept {
  return (value << bits) | (value >> (32 - bits));
}

struct Sha1State {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::uint64_t total_bytes = 0;
  std::array<std::uint8_t, 64> buffer{};
  std::size_t buffered = 0;
};

void ProcessBlock(Sha1State& state, const std::uint8_t* block) {
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; ++i) {
    w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  std::uint32_t a = state.h[0];
  std::uint32_t b = state.h[1];
  std::uint32_t c = state.h[2];
  std::uint32_t d = state.h[3];
  std::uint32_t e = state.h[4];

  for (int i = 0; i < 80; ++i) {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    const std::uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = RotateLeft(b, 30);
    b = a;
    a = temp;
  }

  state.h[0] += a;
  state.h[1] += b;
  state.h[2] += c;
  state.h[3] += d;
  state.h[4] += e;
}

void Update(Sha1State& state, const std::uint8_t* data, std::size_t length) {
  state.total_bytes += length;

  // 先填满手头的缓冲
  if (state.buffered > 0) {
    const std::size_t need = 64 - state.buffered;
    const std::size_t take = length < need ? length : need;
    std::memcpy(state.buffer.data() + state.buffered, data, take);
    state.buffered += take;
    data += take;
    length -= take;
    if (state.buffered == 64) {
      ProcessBlock(state, state.buffer.data());
      state.buffered = 0;
    }
  }

  // 整块直接处理
  while (length >= 64) {
    ProcessBlock(state, data);
    data += 64;
    length -= 64;
  }

  // 剩下的留着
  if (length > 0) {
    std::memcpy(state.buffer.data(), data, length);
    state.buffered = length;
  }
}

[[nodiscard]] std::array<std::uint8_t, 20> Finish(Sha1State& state) {
  const std::uint64_t bit_length = state.total_bytes * 8;

  // 追加 0x80，然后补零到 56 mod 64
  const std::uint8_t pad_byte = 0x80;
  Update(state, &pad_byte, 1);
  const std::uint8_t zero = 0x00;
  while (state.buffered != 56) {
    Update(state, &zero, 1);
  }

  // 追加 64 位大端长度
  std::uint8_t length_bytes[8];
  for (int i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (56 - i * 8));
  }
  Update(state, length_bytes, 8);

  std::array<std::uint8_t, 20> digest{};
  for (int i = 0; i < 5; ++i) {
    digest[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(state.h[i] >> 24);
    digest[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(state.h[i] >> 16);
    digest[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(state.h[i] >> 8);
    digest[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(state.h[i]);
  }
  return digest;
}

}  // namespace

std::string Sha1Raw(std::string_view input) {
  Sha1State state;
  Update(state, reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
  const std::array<std::uint8_t, 20> digest = Finish(state);
  return std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
}

std::string Sha1Hex(std::string_view input) {
  static constexpr char kHex[] = "0123456789abcdef";
  const std::string raw = Sha1Raw(input);
  std::string out;
  out.reserve(raw.size() * 2);
  for (const char ch : raw) {
    const auto byte = static_cast<unsigned char>(ch);
    out += kHex[byte >> 4];
    out += kHex[byte & 0x0F];
  }
  return out;
}

std::string Base64Encode(std::string_view input) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);

  std::size_t i = 0;
  while (i + 2 < input.size()) {
    const auto b0 = static_cast<unsigned char>(input[i]);
    const auto b1 = static_cast<unsigned char>(input[i + 1]);
    const auto b2 = static_cast<unsigned char>(input[i + 2]);
    out += kTable[b0 >> 2];
    out += kTable[((b0 & 0x03) << 4) | (b1 >> 4)];
    out += kTable[((b1 & 0x0F) << 2) | (b2 >> 6)];
    out += kTable[b2 & 0x3F];
    i += 3;
  }

  const std::size_t remaining = input.size() - i;
  if (remaining == 1) {
    const auto b0 = static_cast<unsigned char>(input[i]);
    out += kTable[b0 >> 2];
    out += kTable[(b0 & 0x03) << 4];
    out += "==";
  } else if (remaining == 2) {
    const auto b0 = static_cast<unsigned char>(input[i]);
    const auto b1 = static_cast<unsigned char>(input[i + 1]);
    out += kTable[b0 >> 2];
    out += kTable[((b0 & 0x03) << 4) | (b1 >> 4)];
    out += kTable[(b1 & 0x0F) << 2];
    out += '=';
  }
  return out;
}

}  // namespace ai2048::net
