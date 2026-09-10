// WebSocket 服务入口 —— 消费者①：把引擎暴露给浏览器前端。
//
// 用法：
//   ai2048-server [--host 127.0.0.1] [--port 8765] [--depth 8]
//
// 前端这样连：
//   http://127.0.0.1:8080/?engine=ws://127.0.0.1:8765
//
// 安全边界：只监听回环地址（除非显式改 --host）。
// 这是个本地工具，**没有任何鉴权**，暴露到公网等于把 CPU 交出去。

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
// winsock2.h 必须先于 windows.h（socket_server.cpp 里同理）。
// 这里只要 windows.h 拿 SetConsoleCtrlHandler，所以先包含 winsock2 再包含它。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

#include "ai2048/ai2048.h"
#include "net/protocol.h"
#include "net/socket_server.h"

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI ConsoleHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    g_stop.store(true);
    return TRUE;
  }
  return FALSE;
}
#else
extern "C" void HandleSignal(int /*signal*/) { g_stop.store(true); }
#endif

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8765;
  int depth = 8;
};

[[nodiscard]] bool ParseOptions(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_next = (i + 1) < argc;
    if (arg == "--host" && has_next) {
      options->host = argv[++i];
    } else if (arg == "--port" && has_next) {
      options->port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--depth" && has_next) {
      options->depth = std::atoi(argv[++i]);
    } else if (arg == "-h" || arg == "--help") {
      return false;
    } else {
      std::cerr << "未知选项: " << arg << "\n";
      return false;
    }
  }
  return true;
}

void PrintUsage() {
  std::cout << "ai2048-server " << ai2048::VersionString() << " (ruleset "
            << ai2048::RulesetVersion() << ")\n"
            << "用法: ai2048-server [--host 127.0.0.1] [--port 8765] [--depth 8]\n"
            << "\n"
            << "前端连接方式：http://<前端地址>/?engine=ws://<host>:<port>\n"
            << "\n"
            << "⚠️ 默认只监听回环地址。本服务没有任何鉴权，\n"
            << "   改 --host 到外部地址等于把 CPU 交出去。\n";
}

}  // namespace

int main(int argc, char** argv) {
  // 关掉 stdout 的全缓冲。
  //
  // 默认情况下 stdout 接终端是行缓冲（每行都出），但一旦重定向到管道或文件
  // 就变成全缓冲 —— 这时启动信息、每个请求的日志都卡在缓冲区里，
  // 直到攒满 4KB 或进程退出才吐出来。表现就是"服务端一个字都不打印"，
  // 出问题时完全没法排查（测试脚本想等"已启动"也等不到）。
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    PrintUsage();
    return 1;
  }

#ifdef _WIN32
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#else
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
#endif

  ai2048::net::SocketServer server;
  std::string error;
  if (!server.Listen(options.host, options.port, &error)) {
    std::cerr << "启动失败：" << error << "\n";
    return 1;
  }

  std::string log_prefix;
  ai2048::net::ProtocolHandler handler(&server, &log_prefix);
  server.SetCallbacks(
      [&handler](ai2048::net::ConnectionId id) { handler.OnOpen(id); },
      [&handler](ai2048::net::ConnectionId id, const char* data, std::size_t length) {
        return handler.OnData(id, data, length);
      },
      [&handler](ai2048::net::ConnectionId id) { handler.OnClose(id); });

  std::cout << "ai2048-server " << ai2048::VersionString() << "（规则集 "
            << ai2048::RulesetVersion() << "）已启动\n";
  std::cout << "  监听    ws://" << options.host << ":" << server.port() << "\n";
  std::cout << "  默认深度 " << options.depth << "\n";
  std::cout << "  前端连 http://<前端地址>/?engine=ws://" << options.host << ":"
            << server.port() << "\n";
  std::cout << "  按 Ctrl+C 停止\n";

  server.Run([] { return g_stop.load(); });

  std::cout << "\n正在停止…\n";
  server.Close();
  std::cout << "已停止\n";
  return 0;
}
