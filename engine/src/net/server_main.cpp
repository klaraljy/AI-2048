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
#include <windows.h>
#include <winsock2.h>
#endif

#include <memory>

#include "ai/learn/search_bridge.h"
#include "ai2048/ai2048.h"
#include "core/console.h"
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
  /** 训练好的权重路径。空 = 用手写启发式（默认）。 */
  std::string net_file;
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
    } else if (arg == "--net-file" && has_next) {
      options->net_file = argv[++i];
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
            << "                    [--net-file <训练权重>]\n"
            << "\n"
            << "  --net-file 用训练好的 n-tuple 权重做叶子评估，替代手写启发式。\n"
            << "             不传就是手写启发式（与历史基准一致）。\n"
            << "\n"
            << "前端连接方式：http://<前端地址>/?engine=ws://<host>:<port>\n"
            << "\n"
            << "⚠️ 默认只监听回环地址。本服务没有任何鉴权，\n"
            << "   改 --host 到外部地址等于把 CPU 交出去。\n";
}

}  // namespace

int main(int argc, char** argv) {
  // 双击运行时控制台默认是 GBK，中文会显示成乱码。必须在任何输出之前切到 UTF-8。
  // 输出被重定向时这个调用是空操作，字节保持原样（测试脚本按 UTF-8 读）。
  ai2048::EnableUtf8Console();

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

  // 先加载权重再起服务：加载失败就**直接退出**，不要起一个"看起来在跑、
  // 其实用的是另一套评估"的服务 —— 那种静默降级比启动失败难查得多。
  std::string net_error;
  auto loaded_network = ai2048::learn::LoadNetworkFromFile(options.net_file, &net_error);
  if (!loaded_network.has_value()) {
    std::cerr << "加载权重失败：" << net_error << "\n";
    return 1;
  }
  // 提到循环外，生命周期覆盖整个服务运行期 ——
  // leaf_evaluator 的 context 是指向它的裸指针，它必须先于服务析构。
  const std::shared_ptr<ai2048::learn::ValueNetwork> network = *loaded_network;

  ai2048::net::SocketServer server;
  std::string error;
  if (!server.Listen(options.host, options.port, &error)) {
    std::cerr << "启动失败：" << error << "\n";
    return 1;
  }

  std::string log_prefix;
  // 会话级的默认配置：深度与网络在这里统一注入。
  // 之前这两个都没接上 —— --depth 只被打印出来，实际搜索用的是
  // SearchConfig 的默认值（8），是"参数看着生效、其实没生效"的典型。
  ai2048::SearchConfig defaults;
  defaults.base_depth = options.depth;
  ai2048::learn::AttachNetwork(network, &defaults);

  ai2048::net::ProtocolHandler handler(&server, &log_prefix, defaults);
  server.SetCallbacks([&handler](ai2048::net::ConnectionId id) { handler.OnOpen(id); },
                      [&handler](ai2048::net::ConnectionId id, const char* data,
                                 std::size_t length) { return handler.OnData(id, data, length); },
                      [&handler](ai2048::net::ConnectionId id) { handler.OnClose(id); });

  std::cout << "ai2048-server " << ai2048::VersionString() << "（规则集 "
            << ai2048::RulesetVersion() << "）已启动\n";
  std::cout << "  监听    ws://" << options.host << ":" << server.port() << "\n";
  std::cout << "  默认深度 " << options.depth << "\n";
  if (network) {
    std::cout << "  叶子评估 学习权重 " << options.net_file << "（" << network->tuple_count()
              << " tuple，" << network->parameter_count() << " 参数）\n";
  } else {
    std::cout << "  叶子评估 手写启发式（未传 --net-file）\n";
  }
  std::cout << "  前端连 http://<前端地址>/?engine=ws://" << options.host << ":" << server.port()
            << "\n";
  std::cout << "  按 Ctrl+C 停止\n";

  server.Run([] { return g_stop.load(); });

  std::cout << "\n正在停止…\n";
  server.Close();
  std::cout << "已停止\n";
  return 0;
}
