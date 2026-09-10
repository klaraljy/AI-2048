// 控制台编码测试：验证**双击运行时中文不乱码**。
//
// 这是与 server.test.mjs 里那条字节断言不同的一层：
//   - 字节断言验的是"程序写出的字节是 UTF-8"
//   - 本测试验的是"程序启动时把控制台代码页切成了 UTF-8"
// 两者是不同故障。用户双击运行看到 "锛堣鍒欓泦" 就出在后者：
// 字节没错，但控制台按 GBK(936) 解读。
//
// 做法：起一个**真正带控制台**的子进程（不重定向任何句柄），
// 先把当前控制台代码页设成 936 复现双击状态，再让子进程跑起来，
// 然后查子进程是否把控制台切到了 65001。
//
// 子进程与本测试共用同一个控制台，所以测完必须恢复原代码页 ——
// 否则会把用户/CI 的控制台留在非预期状态上。

#include <gtest/gtest.h>

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ai2048 {
namespace {

#ifdef _WIN32

/** 退出时把控制台输出代码页恢复原值。 */
class ConsoleCodepageGuard {
 public:
  ConsoleCodepageGuard() : original_(GetConsoleOutputCP()) {}
  ~ConsoleCodepageGuard() { SetConsoleOutputCP(original_); }
  ConsoleCodepageGuard(const ConsoleCodepageGuard&) = delete;
  ConsoleCodepageGuard& operator=(const ConsoleCodepageGuard&) = delete;

  [[nodiscard]] UINT original() const noexcept { return original_; }

 private:
  UINT original_;
};

/**
 * 定位 engine\build\ai2048-server.exe。
 *
 * 测试可执行文件在 engine\build-tests\，服务端在 engine\build\ ——
 * 用自身路径推出对侧目录，避免写死绝对路径。
 */
[[nodiscard]] std::wstring FindServerExe() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD written = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  if (written == 0) return {};

  const std::wstring path(buffer, written);
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};

  std::wstring directory = path.substr(0, slash);
  const std::wstring tests_suffix = L"build-tests";
  const std::size_t found = directory.rfind(tests_suffix);
  if (found != std::wstring::npos) {
    directory.replace(found, tests_suffix.size(), L"build");
  }
  return directory + L"\\ai2048-server.exe";
}

/** 起服务端，等它启动后杀掉。子进程继承本控制台，与双击运行时一致。 */
[[nodiscard]] bool RunServerBriefly(const std::wstring& exe, const std::wstring& args) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};

  std::wstring command = L"\"" + exe + L"\" " + args;
  // CreateProcessW 可能就地修改命令行缓冲区，用可写副本。
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  // bInheritHandles = TRUE 且不设 STARTF_USESTDHANDLES：
  // 子进程直接用同一个控制台，这才等价于双击。
  const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &startup, &process);
  if (created == 0) return false;

  // 给它时间跑完启动（含 SetConsoleOutputCP）。
  Sleep(1200);

  TerminateProcess(process.hProcess, 0);
  WaitForSingleObject(process.hProcess, 3000);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

TEST(ConsoleCodepage, ServerSwitchesConsoleToUtf8) {
  const std::wstring exe = FindServerExe();
  ASSERT_FALSE(exe.empty()) << "无法定位 ai2048-server.exe";
  ASSERT_TRUE(GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES)
      << "找不到 " << std::string(exe.begin(), exe.end()) << "，请先构建 engine\\build";

  const ConsoleCodepageGuard guard;

  // 复现双击状态：简中系统控制台默认就是 936。
  ASSERT_NE(SetConsoleOutputCP(936), 0) << "无法把控制台代码页设为 936";

  // 用一个不冲突的端口，避免与开发者本地开着的服务端撞车。
  ASSERT_TRUE(RunServerBriefly(exe, L"--port 15399"));

  EXPECT_EQ(GetConsoleOutputCP(), 65001U)
      << "服务端没有把控制台切到 UTF-8 —— 双击运行时中文会显示成乱码";
}

TEST(ConsoleCodepage, CliSwitchesConsoleToUtf8) {
  std::wstring exe = FindServerExe();
  ASSERT_FALSE(exe.empty());
  const std::size_t slash = exe.find_last_of(L'\\');
  exe = exe.substr(0, slash + 1) + L"ai2048-cli.exe";
  ASSERT_TRUE(GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES)
      << "找不到 ai2048-cli.exe，请先构建 engine\\build";

  const ConsoleCodepageGuard guard;
  ASSERT_NE(SetConsoleOutputCP(936), 0);

  ASSERT_TRUE(RunServerBriefly(exe, L"version"));

  EXPECT_EQ(GetConsoleOutputCP(), 65001U) << "命令行入口没有把控制台切到 UTF-8";
}

#else

TEST(ConsoleCodepage, NoOpOnNonWindows) { GTEST_SKIP() << "控制台代码页是 Windows 专有概念"; }

#endif

}  // namespace
}  // namespace ai2048
