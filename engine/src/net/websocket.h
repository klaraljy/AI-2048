// WebSocket 服务端的编解码（RFC 6455 的**极小子集**）。
//
// 本项目的协议只用得到：文本帧、不分片、无扩展、无 TLS、payload 几十字节。
// 所以这里刻意**不实现**：二进制帧、分片、压缩扩展、TLS、子协议协商。
// 遇到这些一律按协议错误关闭连接，而不是静默忽略 —— 静默忽略会让双方理解不一致。
//
// 分帧有两个容易出错的地方，这里显式处理：
//   1. **TCP 会把帧切开**：一次 recv 可能只拿到半帧，也可能拿到一帧半。
//      所以必须有缓冲与状态机，不能假设"一次读到一个完整帧"。
//   2. **客户端发来的帧必须带掩码**（RFC 6455 5.1），服务端发出去的不带。
//      漏掉 unmask 会导致所有请求体都是乱码。

#ifndef AI2048_NET_WEBSOCKET_H_
#define AI2048_NET_WEBSOCKET_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ai2048::net {

enum class WsOpcode : std::uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

// 从字节流里解出一帧的结果。
enum class WsDecodeStatus {
  kIncomplete,  // 数据还不够，等更多字节
  kOk,          // 解出一帧
  kProtocolError,
  kTooLarge,
};

struct WsFrame {
  WsOpcode opcode = WsOpcode::kText;
  bool fin = true;
  std::string payload;
};

// 单帧 payload 上限。本地工具够用，同时防止异常输入撑爆内存。
inline constexpr std::size_t kMaxFramePayload = 1u << 20;  // 1 MiB

/**
 * 从 buffer 头部尝试解出一帧。
 *
 * 成功时把该帧消耗的字节从 buffer 前面移除（调用方不需要自己切片）。
 * 失败时 buffer 保持不变，便于调用方等待更多数据。
 *
 * @param buffer 字节缓冲，会被就地修改
 * @param frame 解出的帧
 * @param error 失败原因（给日志用）
 */
[[nodiscard]] WsDecodeStatus DecodeFrame(std::string* buffer, WsFrame* frame, std::string* error);

/**
 * 编码一帧（服务端 -> 客户端）。服务端发出的帧**不加掩码**。
 */
[[nodiscard]] std::string EncodeFrame(WsOpcode opcode, std::string_view payload);

/** 编码一个文本帧。 */
[[nodiscard]] std::string EncodeText(std::string_view payload);

/** 编码一个关闭帧（带 2 字节状态码）。 */
[[nodiscard]] std::string EncodeClose(std::uint16_t code);

/** 把关闭帧的 payload 解成状态码与原因。格式不对时返回 false。 */
[[nodiscard]] bool ParseClosePayload(std::string_view payload, std::uint16_t* code,
                                     std::string* reason);

// ---------------------------------------------------------------------------
// 握手
// ---------------------------------------------------------------------------

// WebSocket 的魔术 GUID（RFC 6455 4.2.2），拼错就会静默握手失败。
inline constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/** 由客户端的 Sec-WebSocket-Key 算出 Sec-WebSocket-Accept。 */
[[nodiscard]] std::string ComputeAcceptKey(std::string_view sec_websocket_key);

struct UpgradeRequest {
  bool valid = false;
  std::string sec_websocket_key;
  std::string path;
  std::string error;
};

/**
 * 解析 HTTP 升级请求，并取出握手需要的字段。
 * 只认最小必要集合（GET + Upgrade: websocket + Sec-WebSocket-Key）。
 */
[[nodiscard]] UpgradeRequest ParseUpgradeRequest(std::string_view raw_request);

/** 生成 101 Switching Protocols 响应。 */
[[nodiscard]] std::string BuildUpgradeResponse(std::string_view accept_key);

/** 生成一个简单的 HTTP 错误响应（握手失败时用）。 */
[[nodiscard]] std::string BuildHttpError(int status, std::string_view reason);

}  // namespace ai2048::net

#endif  // AI2048_NET_WEBSOCKET_H_
