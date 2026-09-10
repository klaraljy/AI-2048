// 命令行入口：跑批、自检。属于「消费者②」，见 AGENTS.md 的目录结构说明。
//
// 目前只有版本输出 —— 里程碑 1（规则引擎 + 确定性）尚未开始。
// 这里的子命令名（selfcheck / bench）是 AGENTS.md 里已经写定的目标形态。

#include <cstring>
#include <iostream>
#include <string_view>

#include "ai2048/ai2048.h"

namespace {

void PrintUsage() {
  std::cout << "ai2048-cli " << ai2048::VersionString() << "\n"
            << "用法:\n"
            << "  ai2048-cli version          输出版本与规则集版本\n"
            << "  ai2048-cli selfcheck        同种子重跑两次，校验结果逐字节一致（未实现）\n"
            << "  ai2048-cli bench            在种子集上跑批并输出统计（未实现）\n";
}

// 尚未实现的子命令。返回 2 以区别于「参数错误」。
int NotImplemented(std::string_view command) {
  std::cerr << "ai2048-cli: 子命令 '" << command << "' 尚未实现。\n"
            << "当前只有脚手架，引擎核心见 docs/brief.md 的里程碑 1。\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view command = (argc > 1) ? argv[1] : "";

  if (command.empty() || command == "-h" || command == "--help") {
    PrintUsage();
    return 0;
  }

  if (command == "version") {
    std::cout << "ai2048 " << ai2048::VersionString() << " (ruleset " << ai2048::RulesetVersion()
              << ")\n";
    return 0;
  }

  if (command == "selfcheck" || command == "bench") {
    return NotImplemented(command);
  }

  std::cerr << "ai2048-cli: 未知子命令 '" << command << "'\n\n";
  PrintUsage();
  return 1;
}
