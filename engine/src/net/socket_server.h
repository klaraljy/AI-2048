// 跨平台 TCP 监听：Windows（Winsock2）与 POSIX 的最小公共子集。
//
// 为什么不用 poll()：**MinGW 不提供 <poll.h>**（实测），
// 而 select() 两边都有。本地工具连接数很少，select 的 1024 上限不是问题。
//
// 这个类只做三件事：监听、接受连接、按可读性分发。
// 协议解析在 protocol.cpp，引擎调用在那里 —— 这里不碰 AI。

#ifndef AI2048_NET_SOCKET_SERVER_H_
#define AI2048_NET_SOCKET_SERVER_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ai2048::net {

/** 连接句柄。就是文件描述符 / SOCKET，但不暴露平台类型。 */
using ConnectionId = std::intptr_t;
inline constexpr ConnectionId kInvalidConnection = -1;

class SocketServer {
 public:
  SocketServer() = default;
  ~SocketServer();
  SocketServer(const SocketServer&) = delete;
  SocketServer& operator=(const SocketServer&) = delete;

  /**
   * 开始监听。
   * @param host 绑定地址，如 "127.0.0.1"
   * @param port 端口
   * @param error 失败原因
   */
  [[nodiscard]] bool Listen(const std::string& host, std::uint16_t port, std::string* error);

  /** 停止监听并关闭所有连接。可重复调用。 */
  void Close();

  [[nodiscard]] bool listening() const noexcept { return listen_socket_ != kInvalidConnection; }

  /** 实际监听的端口（传 0 让系统分配时用得上）。 */
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  /** 收到数据时回调。返回 false 表示应关闭该连接。 */
  using OnData = std::function<bool(ConnectionId id, const char* data, std::size_t length)>;
  /** 连接建立 / 断开时回调。 */
  using OnOpen = std::function<void(ConnectionId id)>;
  using OnClose = std::function<void(ConnectionId id)>;

  /**
   * 跑一轮事件循环，直到 stop 返回 true 或出错。
   *
   * 内部用 select() 等待可读事件，**不忙等** —— 空转会把 CPU 吃满。
   *
   * @param stop 每轮问一次；返回 true 就退出
   * @param timeout_ms select 的超时，决定询问 stop 的频率
   */
  void Run(const std::function<bool()>& stop, int timeout_ms = 200);

  /** 向连接写数据。返回是否全部写出。 */
  [[nodiscard]] bool Send(ConnectionId id, const std::string& data);

  /** 主动关闭一个连接。 */
  void CloseConnection(ConnectionId id);

  void SetCallbacks(OnOpen on_open, OnData on_data, OnClose on_close);

 private:
  void AcceptPending();
  void DrainReadable(ConnectionId id);
  void ForgetConnection(ConnectionId id);

  ConnectionId listen_socket_ = kInvalidConnection;
  std::uint16_t port_ = 0;
  std::vector<ConnectionId> connections_;

  OnOpen on_open_;
  OnData on_data_;
  OnClose on_close_;
};

}  // namespace ai2048::net

#endif  // AI2048_NET_SOCKET_SERVER_H_
