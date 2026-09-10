// 协议层：握手、分帧、报文分发、调用引擎。
//
// 这是 WebSocket 服务端的"大脑"，socket 收发在 socket_server.cpp。
//
// 契约（与前端 web/js/transport.js 严格对应）：
//
//   请求  {"id":N,"type":"configure","payload":{"config":{...}}}
//         {"id":N,"type":"best-move","payload":{"state":{"board":[[...]]},"options":{...}}}
//   响应  {"id":N,"type":"result","payload":{...}}
//         {"id":N,"type":"error","payload":{"message":"..."}}
//
//   best-move 的 payload 形如 {"move":"up","debugInfo":{...}}，
//   **无合法走子时 move 为 null**。其余任何情况都必须给出一个合法方向 ——
//   前端把 null 当成"AI 无步可走"并停止演示（见 AGENTS.md 的硬实时预算一条）。
//
// 每个连接持有**自己的置换表**，跨步复用。这不是可选的优化：
// 早期实现每步新建一张表，单局白花 98% 的时间在分配清零上。

#ifndef AI2048_NET_PROTOCOL_H_
#define AI2048_NET_PROTOCOL_H_

#include <cstdint>
#include <map>
#include <string>

#include "ai/search.h"
#include "net/socket_server.h"

namespace ai2048::net {

/**
 * 一个连接的会话状态。
 *
 * 生命周期：握手（累积 HTTP 头）→ 已升级（来回帧）。
 * 两个阶段共用一个缓冲区，因为在同一次 recv 里
 * "握手请求 + 第一帧"是完全可能的（客户端经常一起发）。
 */
struct Session {
  enum class Phase { kHandshake, kOpen };

  Phase phase = Phase::kHandshake;
  std::string buffer;  // 握手阶段是 HTTP 文本，之后是帧字节
  bool close_after_send = false;

  // 配置与搜索状态。每连接一份 —— 不同客户端可以跑不同深度。
  ai2048::SearchConfig config;
  ai2048::TranspositionTable table{ai2048::TranspositionTable::kDefaultCapacity, false};

  // 统计，便于排查"引擎是不是在正常工作"
  std::uint64_t requests = 0;
  std::uint64_t errors = 0;
};

/** 协议处理器：把 socket 事件翻译成引擎调用与响应。 */
class ProtocolHandler {
 public:
  ProtocolHandler(SocketServer* server, std::string* log_prefix);

  /** 连上时的回调。 */
  void OnOpen(ConnectionId id);

  /** 收到数据。返回 false 表示应关闭连接。 */
  [[nodiscard]] bool OnData(ConnectionId id, const char* data, std::size_t length);

  /** 断开时的回调。 */
  void OnClose(ConnectionId id);

  /** 已建立的会话数（用于日志）。 */
  [[nodiscard]] std::size_t session_count() const noexcept { return sessions_.size(); }

 private:
  [[nodiscard]] bool HandleHandshake(ConnectionId id, Session* session);
  void HandleFrames(ConnectionId id, Session* session);
  void HandleMessage(ConnectionId id, Session* session, const std::string& text);

  void SendResult(ConnectionId id, std::int64_t request_id, const std::string& payload_json);
  void SendError(ConnectionId id, std::int64_t request_id, const std::string& message);

  SocketServer* server_;
  std::map<ConnectionId, Session> sessions_;
};

}  // namespace ai2048::net

#endif  // AI2048_NET_PROTOCOL_H_
