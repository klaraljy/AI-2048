#include "net/socket_server.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
// winsock2.h 必须在 windows.h 之前 —— 顺序反了会拉进 winsock.h 并产生大量冲突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ai2048::net {

namespace {

/** 发送遇到 WSAEWOULDBLOCK 时的最大重试次数（每次之间等 2ms）。 */
constexpr int kMaxSendRetries = 250;

#ifdef _WIN32
using RawSocket = SOCKET;
constexpr RawSocket kInvalidSocket = INVALID_SOCKET;

void CloseRaw(RawSocket socket) {
  if (socket != kInvalidSocket) closesocket(socket);
}

[[nodiscard]] std::string LastSocketError() {
  return "Winsock 错误码 " + std::to_string(WSAGetLastError());
}

/** Winsock 需要一次性初始化。用静态对象保证只做一次。 */
struct WinsockGuard {
  WinsockGuard() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockGuard() { WSACleanup(); }
};

void EnsureWinsock() { static WinsockGuard guard; }

#else
using RawSocket = int;
constexpr RawSocket kInvalidSocket = -1;

void CloseRaw(RawSocket socket) {
  if (socket >= 0) close(socket);
}

[[nodiscard]] std::string LastSocketError() { return std::string(std::strerror(errno)); }

void EnsureWinsock() {}

#endif

[[nodiscard]] RawSocket ToRaw(ConnectionId id) { return static_cast<RawSocket>(id); }
[[nodiscard]] ConnectionId ToId(RawSocket socket) { return static_cast<ConnectionId>(socket); }

/** 关掉 Nagle：本工具的报文很小，攒包只会增加延迟。 */
void DisableNagle(RawSocket socket) {
  const int flag = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag),
             static_cast<socklen_t>(sizeof(flag)));
}

/** 切换阻塞/非阻塞模式。 */
void SetBlocking(RawSocket socket, bool blocking) {
#ifdef _WIN32
  u_long mode = blocking ? 0UL : 1UL;
  ioctlsocket(socket, FIONBIO, &mode);
#else
  const int flags = fcntl(socket, F_GETFL, 0);
  if (flags < 0) return;
  fcntl(socket, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

}  // namespace

SocketServer::~SocketServer() { Close(); }

bool SocketServer::Listen(const std::string& host, std::uint16_t port, std::string* error) {
  EnsureWinsock();
  Close();

  const RawSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    *error = "创建 socket 失败：" + LastSocketError();
    return false;
  }

  // 允许立刻重用地址，避免重启时报 "Address already in use"
  const int reuse = 1;
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             static_cast<socklen_t>(sizeof(reuse)));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
    *error = "无法解析地址：" + host;
    CloseRaw(socket);
    return false;
  }

  if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    *error = "绑定 " + host + ":" + std::to_string(port) + " 失败：" + LastSocketError();
    CloseRaw(socket);
    return false;
  }

  if (::listen(socket, 16) != 0) {
    *error = "listen 失败：" + LastSocketError();
    CloseRaw(socket);
    return false;
  }

  // 监听 socket 必须是非阻塞的。
  //
  // AcceptPending 用 for(;;) 把待接受连接一次取空，靠 accept() 返回
  // "暂时没有"来结束循环。阻塞 socket 上不存在这个返回值 ——
  // select 说可读只保证**至少**有一个连接，取完最后一个之后
  // 下一次 accept() 会永久阻塞，整个事件循环停摆（这是实测到的真实故障）。
  SetBlocking(socket, false);

  // 传 0 时系统会分配端口，要把实际端口读回来
  sockaddr_in bound{};
  socklen_t bound_length = sizeof(bound);
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    port_ = ntohs(bound.sin_port);
  } else {
    port_ = port;
  }

  listen_socket_ = ToId(socket);
  return true;
}

void SocketServer::Close() {
  for (const ConnectionId id : connections_) {
    CloseRaw(ToRaw(id));
  }
  connections_.clear();

  if (listen_socket_ != kInvalidConnection) {
    CloseRaw(ToRaw(listen_socket_));
    listen_socket_ = kInvalidConnection;
  }
  port_ = 0;
}

void SocketServer::SetCallbacks(OnOpen on_open, OnData on_data, OnClose on_close) {
  on_open_ = std::move(on_open);
  on_data_ = std::move(on_data);
  on_close_ = std::move(on_close);
}

void SocketServer::AcceptPending() {
  for (;;) {
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    const RawSocket client =
        accept(ToRaw(listen_socket_), reinterpret_cast<sockaddr*>(&peer), &peer_length);

    if (client == kInvalidSocket) {
      // 监听 socket 是**非阻塞**的（见 Listen），所以"队列里没有更多连接了"
      // 表现为 WSAEWOULDBLOCK / EAGAIN 而不是阻塞等待。
      //
      // 这里必须真的判断错误码，不能像以前那样"失败就返回"：
      // 阻塞 socket 上 accept() 没有"暂时没有"这个返回，第三次调用会
      // **永久阻塞**，整个事件循环就此停摆 —— 这正是之前的真实故障
      // （select 说可读只保证至少有一个连接，不保证恰好一个）。
#ifdef _WIN32
      const int code = WSAGetLastError();
      if (code == WSAEWOULDBLOCK) return;
#else
      if (errno == EWOULDBLOCK || errno == EAGAIN) return;
#endif
      // 其它错误（如 ECONNABORTED）不该拖垮监听：跳过这一个，继续服务。
      continue;
    }

    DisableNagle(client);
    // **不要**把新连接改回阻塞。
    //
    // 这是本项目最容易犯的一个错误，而且已经犯过两次：select 说"可读"
    // 只保证**有数据可读**，不保证"下一次读会立刻返回 0 或错误"。
    // 阻塞 socket 上，DrainReadable 的 for(;;) 在读完现有数据后，
    // 第二次 recv 会一直等下去 —— 整个单线程事件循环就此停摆。
    //
    // 非阻塞之后，循环由 WSAEWOULDBLOCK / EAGAIN 正常终止。
    SetBlocking(client, false);
    const ConnectionId id = ToId(client);
    connections_.push_back(id);
    if (on_open_) on_open_(id);
  }
}

void SocketServer::DrainReadable(ConnectionId id) {
  char buffer[8192];
  for (;;) {
    const int received = recv(ToRaw(id), buffer, static_cast<int>(sizeof(buffer)), 0);
    if (received > 0) {
      if (on_data_ && !on_data_(id, buffer, static_cast<std::size_t>(received))) {
        CloseConnection(id);
        return;
      }
      continue;
    }
    if (received == 0) {
      // 对端正常关闭
      CloseConnection(id);
      return;
    }
    // 出错：EWOULDBLOCK 表示这一轮读完了，其它错误则关闭
#ifdef _WIN32
    const int code = WSAGetLastError();
    if (code == WSAEWOULDBLOCK) return;
#else
    if (errno == EWOULDBLOCK || errno == EAGAIN) return;
#endif
    CloseConnection(id);
    return;
  }
}

void SocketServer::ForgetConnection(ConnectionId id) {
  const auto it = std::find(connections_.begin(), connections_.end(), id);
  if (it != connections_.end()) connections_.erase(it);
}

void SocketServer::CloseConnection(ConnectionId id) {
  if (id == kInvalidConnection) return;
  CloseRaw(ToRaw(id));
  ForgetConnection(id);
  if (on_close_) on_close_(id);
}

bool SocketServer::Send(ConnectionId id, const std::string& data) {
  std::size_t sent_total = 0;
  int retries = 0;
  while (sent_total < data.size()) {
    const int sent = send(ToRaw(id), data.data() + sent_total,
                          static_cast<int>(data.size() - sent_total), 0);
    if (sent > 0) {
      sent_total += static_cast<std::size_t>(sent);
      continue;
    }

    // 连接是**非阻塞**的，所以 send 也可能报"暂时写不出去"（发送缓冲区满）。
    // 这不等于失败：报文体只有几百字节，而回环的发送缓冲区远大于此，
    // 正常不会发生。这里等一小会儿重试有限次 —— 丢掉一条响应会让
    // 前端永久等一个不会来的回包，比多等几毫秒糟得多。
    const bool would_block =
#ifdef _WIN32
        (sent < 0 && WSAGetLastError() == WSAEWOULDBLOCK);
#else
        (sent < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
#endif
    if (!would_block) return false;
    if (++retries > kMaxSendRetries) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

void SocketServer::Run(const std::function<bool()>& stop, int timeout_ms) {
  if (!listening()) return;

  while (!stop()) {
    fd_set read_set;
    FD_ZERO(&read_set);

    const ConnectionId listener = listen_socket_;
    FD_SET(ToRaw(listener), &read_set);
#ifdef _WIN32
    // Windows 的 select 忽略第一个参数
    const int max_fd = 0;
#else
    int max_fd = static_cast<int>(ToRaw(listener));
#endif

    for (const ConnectionId id : connections_) {
      FD_SET(ToRaw(id), &read_set);
#ifndef _WIN32
      max_fd = std::max(max_fd, static_cast<int>(ToRaw(id)));
#endif
    }

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    const int ready = select(max_fd + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready < 0) {
#ifdef _WIN32
      if (WSAGetLastError() == WSAEINTR) continue;
#else
      if (errno == EINTR) continue;
#endif
      // select 失败且不是被信号打断：没有更好的恢复手段，退出让上层决定
      return;
    }
    if (ready == 0) continue;  // 超时，回去问 stop

    if (FD_ISSET(ToRaw(listener), &read_set)) {
      AcceptPending();
    }

    // 注意：DrainReadable 可能改动 connections_（关闭连接），
    // 所以先按值拷一份再遍历。
    const std::vector<ConnectionId> snapshot = connections_;
    for (const ConnectionId id : snapshot) {
      if (FD_ISSET(ToRaw(id), &read_set)) {
        DrainReadable(id);
      }
    }
  }
}

}  // namespace ai2048::net
