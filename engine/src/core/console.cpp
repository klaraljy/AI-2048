#include "core/console.h"

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ai2048 {

void EnableUtf8Console() {
#ifdef _WIN32
  // 无条件设置，**不要**加"仅当 stdout 是控制台"的判断。
  //
  // 曾经加过那个判断，结果是错的：
  //   - 它让"双击时中文正常"和"重定向后字节正确"变成两条分叉路径，
  //     而其中一条（非控制台时）永远不会被任何测试覆盖到 ——
  //     测试的子进程 stdout 恰好就是管道，于是测试查了个寂寞。
  //   - 实测表明即使 stdout 被重定向，设置控制台代码页仍然是有意义的：
  //     控制台本身（用户看的那个窗口）会跟着变。Chrome 的 UTF-8 模式
  //     就是这么做的（它需要跑到系统已有的控制台里改代码页）。
  //
  // 对管道/文件而言这个调用没有副作用：写出去的字节仍是 UTF-8，
  // 与控制台代码页无关，测试脚本按 UTF-8 读即可。
  static_cast<void>(SetConsoleOutputCP(CP_UTF8));
  static_cast<void>(SetConsoleCP(CP_UTF8));
#endif
}

}  // namespace ai2048
