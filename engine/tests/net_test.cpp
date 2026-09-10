// WebSocket 握手用到的基础算法测试：SHA-1 与 Base64。
//
// 判据是**公开的已知向量**，不是"自己跟自己对" ——
// 自己跟自己测只能证明稳定，不能证明正确。
//
//   SHA-1: FIPS 180-1 / RFC 3174 的标准向量
//   Base64: RFC 4648 的标准向量
//   WebSocket 握手: RFC 6455 第 1.3 节的示例

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <array>

#include "net/sha1.h"
#include "net/websocket.h"

namespace ai2048::net {
namespace {

// ---------------------------------------------------------------------------
// SHA-1 标准向量（RFC 3174 / FIPS 180-1）
// ---------------------------------------------------------------------------

TEST(Sha1, EmptyString) {
  EXPECT_EQ(Sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(Sha1, Abc) {
  EXPECT_EQ(Sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

// 这条覆盖"一个块放不下"的路径
TEST(Sha1, LongStringAcrossBlocks) {
  EXPECT_EQ(Sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// 这条覆盖多块与长度填充：一百万个 'a'
TEST(Sha1, OneMillionA) {
  const std::string input(1000000, 'a');
  EXPECT_EQ(Sha1Hex(input), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// 填充逻辑的边界：SHA-1 在 55/56、63/64 字节处行为会变。
// 期望值由 Node 的 crypto（独立实现）算出，不是自己跟自己对。
TEST(Sha1, PaddingBoundaries) {
  EXPECT_EQ(Sha1Hex(std::string(55, 'x')), "cef734ba81a024479e09eb5a75b6ddae62e6abf1");
  EXPECT_EQ(Sha1Hex(std::string(56, 'x')), "901305367c259952f4e7af8323f480d59f81335b");
  EXPECT_EQ(Sha1Hex(std::string(63, 'x')), "0ddc4e0cccd9a12850deb5abb0853a4425559fec");
  EXPECT_EQ(Sha1Hex(std::string(64, 'x')), "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163");
  EXPECT_EQ(Sha1Hex(std::string(65, 'x')), "78c741ddc482e4cdf8c474a0876347a0905b6233");
  EXPECT_EQ(Sha1Hex(std::string(128, 'x')), "150fa3fbdc899bd0b8f95a9fb6027f564d953762");
  EXPECT_EQ(Sha1Hex("hello world"), "2aae6c35c94fcfb415dbe95f408b9ce91ee846ed");
}

TEST(Sha1, RawIs20Bytes) {
  EXPECT_EQ(Sha1Raw("abc").size(), 20u);
  const std::string raw = Sha1Raw("abc");
  // 前 4 字节应等于 a9993e36
  EXPECT_EQ(static_cast<unsigned char>(raw[0]), 0xa9);
  EXPECT_EQ(static_cast<unsigned char>(raw[1]), 0x99);
  EXPECT_EQ(static_cast<unsigned char>(raw[2]), 0x3e);
  EXPECT_EQ(static_cast<unsigned char>(raw[3]), 0x36);
}

// 含嵌入 NUL 的输入不能被截断 —— 这是"按字节而不是按 C 字符串"的检查
TEST(Sha1, EmbeddedNulIsHandled) {
  const std::string with_nul("a\0b", 3);
  const std::string without_nul("ab", 2);
  EXPECT_NE(Sha1Hex(with_nul), Sha1Hex(without_nul));
  EXPECT_EQ(Sha1Hex(with_nul).size(), 40u);
}

// ---------------------------------------------------------------------------
// Base64 标准向量（RFC 4648 第 10 节）
// ---------------------------------------------------------------------------

TEST(Base64, Rfc4648Vectors) {
  EXPECT_EQ(Base64Encode(""), "");
  EXPECT_EQ(Base64Encode("f"), "Zg==");
  EXPECT_EQ(Base64Encode("fo"), "Zm8=");
  EXPECT_EQ(Base64Encode("foo"), "Zm9v");
  EXPECT_EQ(Base64Encode("foob"), "Zm9vYg==");
  EXPECT_EQ(Base64Encode("fooba"), "Zm9vYmE=");
  EXPECT_EQ(Base64Encode("foobar"), "Zm9vYmFy");
}

TEST(Base64, EncodesArbitraryBytes) {
  // 0x00 0xFF 0x10 -> 三种填充情形都要正确
  const std::string bytes("\x00\xff\x10", 3);
  EXPECT_EQ(Base64Encode(bytes), "AP8Q");

  const std::string two("\xff\xff", 2);
  EXPECT_EQ(Base64Encode(two), "//8=");

  const std::string one("\x00", 1);
  EXPECT_EQ(Base64Encode(one), "AA==");
}

// ---------------------------------------------------------------------------
// WebSocket 握手：RFC 6455 第 1.3 节的示例
//
// 客户端发 Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
// 服务端应回 Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
//
// 这是整条握手里最容易写错的一步（GUID 拼错、Base64 用错都会静默失败），
// 所以单独钉住。
// ---------------------------------------------------------------------------

TEST(WebSocketHandshake, Rfc6455Example) {
  constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  constexpr std::string_view kKey = "dGhlIHNhbXBsZSBub25jZQ==";

  const std::string concat = std::string(kKey) + std::string(kGuid);
  const std::string accept = Base64Encode(Sha1Raw(concat));

  EXPECT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// ---------------------------------------------------------------------------
// WebSocket 分帧
//
// 这里最需要覆盖的是**跨包**：TCP 会把帧切开，也会把多帧粘在一起。
// "一次读到一个完整帧"是错的假设，而基于这个假设的实现只在
// payload 较大或网络恰好分包时才失败 —— 平时测不出来。
// ---------------------------------------------------------------------------

namespace {

/** 按 RFC 6455 造一个客户端帧（**带掩码**，与服务端发出的不同）。 */
[[nodiscard]] std::string MakeClientFrame(WsOpcode opcode, std::string_view payload,
                                          std::uint32_t mask = 0x01020304u,
                                          bool force_extended_16 = false,
                                          bool force_extended_64 = false) {
  std::string out;
  out += static_cast<char>(0x80u | static_cast<std::uint8_t>(opcode));

  const std::size_t length = payload.size();
  if (force_extended_64) {
    out += static_cast<char>(0x80u | 127u);
    for (int i = 7; i >= 0; --i) {
      out += static_cast<char>((static_cast<std::uint64_t>(length) >> (i * 8)) & 0xFFu);
    }
  } else if (force_extended_16 || length >= 126) {
    out += static_cast<char>(0x80u | 126u);
    out += static_cast<char>((length >> 8) & 0xFFu);
    out += static_cast<char>(length & 0xFFu);
  } else {
    out += static_cast<char>(0x80u | static_cast<std::uint8_t>(length));
  }

  const auto mask_bytes = std::array<unsigned char, 4>{
      static_cast<unsigned char>(mask & 0xFFu),
      static_cast<unsigned char>((mask >> 8) & 0xFFu),
      static_cast<unsigned char>((mask >> 16) & 0xFFu),
      static_cast<unsigned char>((mask >> 24) & 0xFFu),
  };
  for (const unsigned char b : mask_bytes) out += static_cast<char>(b);
  for (std::size_t i = 0; i < length; ++i) {
    out += static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask_bytes[i % 4]);
  }
  return out;
}

}  // namespace

TEST(WsFrame, DecodesSimpleTextFrame) {
  std::string buffer = MakeClientFrame(WsOpcode::kText, "hello");
  WsFrame frame;
  std::string error;

  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kOk) << error;
  EXPECT_EQ(frame.opcode, WsOpcode::kText);
  EXPECT_TRUE(frame.fin);
  EXPECT_EQ(frame.payload, "hello");
  EXPECT_TRUE(buffer.empty()) << "解出的帧应从缓冲中移除";
}

// 掩码必须被解除。漏掉这一步，请求体全是乱码 —— 而且只在 payload
// 非空时才发现，空帧会"看起来正常"。
TEST(WsFrame, UnmasksPayload) {
  std::string buffer = MakeClientFrame(WsOpcode::kText, "{\"id\":1}");
  WsFrame frame;
  std::string error;
  ASSERT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kOk) << error;
  EXPECT_EQ(frame.payload, "{\"id\":1}");
}

// 分片到达：一次只喂 1 个字节，应该先一直 kIncomplete，最后一字节才成帧。
TEST(WsFrame, HandlesByteByByteArrival) {
  const std::string whole = MakeClientFrame(WsOpcode::kText, "chunked delivery test");
  std::string buffer;
  WsFrame frame;
  std::string error;

  for (std::size_t i = 0; i + 1 < whole.size(); ++i) {
    buffer += whole[i];
    EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kIncomplete)
        << "喂到第 " << i << " 字节时不该成帧";
  }
  buffer += whole.back();
  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kOk) << error;
  EXPECT_EQ(frame.payload, "chunked delivery test");
}

TEST(WsFrame, HandlesTwoFramesInOneBuffer) {
  std::string buffer = MakeClientFrame(WsOpcode::kText, "first") +
                       MakeClientFrame(WsOpcode::kText, "second", 0x0A0B0C0Du);
  WsFrame frame;
  std::string error;

  ASSERT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kOk) << error;
  EXPECT_EQ(frame.payload, "first");
  ASSERT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kOk) << error;
  EXPECT_EQ(frame.payload, "second");
  EXPECT_TRUE(buffer.empty());
}

// 126 与 127 两种扩展长度编码
TEST(WsFrame, DecodesExtendedLengths) {
  const std::string medium(300, 'm');  // 需要 16 位长度
  std::string buffer = MakeClientFrame(WsOpcode::kText, medium, 0x11223344u, true);
  WsFrame frame;
  std::string error;
  ASSERT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kOk) << error;
  EXPECT_EQ(frame.payload.size(), 300u);
  EXPECT_EQ(frame.payload, medium);

  const std::string small = MakeClientFrame(WsOpcode::kText, "x", 0x55667788u, false, true);
  std::string buffer64 = small;
  ASSERT_EQ(DecodeFrame(&buffer64, &frame, &error), WsDecodeStatus::kOk) << error;
  EXPECT_EQ(frame.payload, "x");
}

// 协议错误必须**明确拒绝**，不能静默接受 —— 静默接受会让双方理解不一致
TEST(WsFrame, RejectsMissingMask) {
  // 手工造一个不带掩码的客户端帧
  std::string buffer;
  buffer += static_cast<char>(0x81u);
  buffer += static_cast<char>(0x03u);
  buffer += "abc";

  WsFrame frame;
  std::string error;
  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kProtocolError);
  EXPECT_FALSE(error.empty());
}

TEST(WsFrame, RejectsFragmentedFrame) {
  std::string buffer = MakeClientFrame(WsOpcode::kText, "part");
  buffer[0] = static_cast<char>(static_cast<unsigned char>(buffer[0]) & 0x7Fu);  // 清掉 FIN

  WsFrame frame;
  std::string error;
  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kProtocolError);
  EXPECT_NE(error.find("分片"), std::string::npos) << "错误信息应说明是分片：" << error;
}

TEST(WsFrame, RejectsBinaryFrame) {
  std::string buffer = MakeClientFrame(WsOpcode::kBinary, "bin");
  WsFrame frame;
  std::string error;
  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kProtocolError);
}

TEST(WsFrame, RejectsRsvBits) {
  std::string buffer = MakeClientFrame(WsOpcode::kText, "x");
  buffer[0] = static_cast<char>(static_cast<unsigned char>(buffer[0]) | 0x40u);  // RSV1
  WsFrame frame;
  std::string error;
  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kProtocolError);
}

TEST(WsFrame, RejectsOversizedFrame) {
  // 声明一个超大长度（不实际发送数据）—— 应该在读完头部就拒绝，而不是等数据
  std::string buffer;
  buffer += static_cast<char>(0x81u);
  buffer += static_cast<char>(0x80u | 127u);
  for (int i = 0; i < 8; ++i) buffer += static_cast<char>(0xFFu);
  buffer += std::string(4, '\0');  // 掩码键

  WsFrame frame;
  std::string error;
  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kTooLarge);
}

TEST(WsFrame, DecodeFailureLeavesBufferIntact) {
  // 半帧：不应消耗缓冲，调用方还要靠它继续攒数据
  const std::string whole = MakeClientFrame(WsOpcode::kText, "incomplete");
  std::string buffer = whole.substr(0, whole.size() - 3);
  const std::string before = buffer;

  WsFrame frame;
  std::string error;
  EXPECT_EQ(DecodeFrame(&buffer, &frame, &error), WsDecodeStatus::kIncomplete);
  EXPECT_EQ(buffer, before) << "不完整时不能消耗缓冲";
}

TEST(WsFrame, ServerFramesAreNotMasked) {
  const std::string encoded = EncodeText("hi");
  ASSERT_GE(encoded.size(), 2u);
  EXPECT_EQ(static_cast<unsigned char>(encoded[0]) & 0x80u, 0x80u) << "FIN 应为 1";
  EXPECT_EQ(static_cast<unsigned char>(encoded[0]) & 0x0Fu, 0x01u) << "opcode 应为文本";
  EXPECT_EQ(static_cast<unsigned char>(encoded[1]) & 0x80u, 0x00u) << "服务端帧不加掩码";
  EXPECT_EQ(encoded.substr(2), "hi");
}

TEST(WsFrame, RoundTripsServerEncoding) {
  // 服务端发出的帧用「服务端编码 + 手工解码」验证长度编码
  for (const std::size_t length : {0u, 1u, 125u, 126u, 300u, 70000u}) {
    const std::string payload(length, 'z');
    const std::string encoded = EncodeText(payload);

    // 手工解出长度字段，确认与编码规则一致
    const auto b1 = static_cast<unsigned char>(encoded[1]);
    std::size_t declared = b1 & 0x7Fu;
    std::size_t header = 2;
    if (declared == 126) {
      declared = (static_cast<std::size_t>(static_cast<unsigned char>(encoded[2])) << 8) |
                 static_cast<unsigned char>(encoded[3]);
      header = 4;
    } else if (declared == 127) {
      declared = 0;
      for (int i = 0; i < 8; ++i) {
        declared = (declared << 8) | static_cast<unsigned char>(encoded[2 + static_cast<std::size_t>(i)]);
      }
      header = 10;
    }
    EXPECT_EQ(declared, length) << "长度 " << length << " 的编码不正确";
    EXPECT_EQ(encoded.size(), header + length);
    EXPECT_EQ(encoded.substr(header), payload);
  }
}

TEST(WsFrame, CloseFrameCarriesStatusCode) {
  const std::string encoded = EncodeClose(1000);
  EXPECT_EQ(static_cast<unsigned char>(encoded[0]) & 0x0Fu, 0x08u) << "opcode 应为关闭";

  std::uint16_t code = 0;
  std::string reason;
  ASSERT_TRUE(ParseClosePayload(encoded.substr(2), &code, &reason));
  EXPECT_EQ(code, 1000u);
  EXPECT_TRUE(reason.empty());
}

// 关闭帧只有 1 字节 payload 是非法的（RFC 6455 5.5.1）
TEST(WsFrame, RejectsShortClosePayload) {
  std::uint16_t code = 0;
  std::string reason;
  EXPECT_FALSE(ParseClosePayload("x", &code, &reason));
  EXPECT_FALSE(ParseClosePayload("", &code, &reason));
}

// ---------------------------------------------------------------------------
// 握手请求解析
// ---------------------------------------------------------------------------

TEST(WsHandshake, ParsesValidUpgradeRequest) {
  const std::string raw =
      "GET /ws HTTP/1.1\r\n"
      "Host: 127.0.0.1:8765\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";

  const UpgradeRequest request = ParseUpgradeRequest(raw);
  EXPECT_TRUE(request.valid) << request.error;
  EXPECT_EQ(request.path, "/ws");
  EXPECT_EQ(request.sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
}

// 头名大小写不敏感（RFC 7230）
TEST(WsHandshake, HeaderNamesAreCaseInsensitive) {
  const std::string raw =
      "GET / HTTP/1.1\r\n"
      "UPGRADE: WebSocket\r\n"
      "connection: keep-alive, Upgrade\r\n"
      "sec-websocket-key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "\r\n";

  const UpgradeRequest request = ParseUpgradeRequest(raw);
  EXPECT_TRUE(request.valid) << request.error;
  EXPECT_EQ(request.sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
}

TEST(WsHandshake, RejectsMissingFields) {
  const std::string no_upgrade =
      "GET / HTTP/1.1\r\nConnection: Upgrade\r\nSec-WebSocket-Key: abc\r\n\r\n";
  EXPECT_FALSE(ParseUpgradeRequest(no_upgrade).valid);

  const std::string no_key =
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
  EXPECT_FALSE(ParseUpgradeRequest(no_key).valid);

  const std::string wrong_method =
      "POST / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: abc\r\n\r\n";
  EXPECT_FALSE(ParseUpgradeRequest(wrong_method).valid);

  const std::string truncated = "GET / HTTP/1.1\r\nUpgrade: websocket\r\n";
  EXPECT_FALSE(ParseUpgradeRequest(truncated).valid);
}

TEST(WsHandshake, UpgradeResponseHasRequiredHeaders) {
  const std::string response = BuildUpgradeResponse("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
  EXPECT_NE(response.find("HTTP/1.1 101"), std::string::npos);
  EXPECT_NE(response.find("Upgrade: websocket"), std::string::npos);
  EXPECT_NE(response.find("Connection: Upgrade"), std::string::npos);
  EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
  EXPECT_NE(response.find("\r\n\r\n"), std::string::npos);
}

TEST(WsHandshake, HttpErrorHasContentLength) {
  const std::string response = BuildHttpError(400, "Bad Request");
  EXPECT_NE(response.find("HTTP/1.1 400"), std::string::npos);
  EXPECT_NE(response.find("Content-Length:"), std::string::npos);
}

}  // namespace
}  // namespace ai2048::net
