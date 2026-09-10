# AI-2048

2048 的 AI 实验室。C++ 引擎在固定种子集上跑批，用可复现的分数证明某个算法改动是不是真的更强；
附带一个动画和音效到位、能在 Windows 桌面和 Android 手机上玩的前端。

> **状态：里程碑 1 完成**。C++ 规则引擎可用且确定性已验证
> （1000 局 `selfcheck` 逐字节一致，43 个单元测试全绿，规则与 Cirulli 原始算法
> 在全部 65536 种行状态上逐行对拍一致）。
> **AI 还没有**：`selfcheck` / `bench` 用的是刻意平凡的走子策略，
> 平均分 2309 是引擎基线而不是 AI 成绩。里程碑见 `docs/brief.md`。

## 它解决什么问题

网上多数 2048 AI 项目是"跑出来一个高分就发出来"：没有固定的种子集、没有改动前后的对照、
也没有回归检测。结果是**你无法判断一次算法改动到底是真变强了还是运气好**。

这个项目把"AI 到底有没有变强"变成一个可复现、可证伪的实验协议：

1. **固定种子集** —— 每个基准种子集有版本号，所有结果都带版本号，不同规则集下的分数禁止混排。
2. **对拍基线** —— 保留一个已知强度的 JS 原型作为基线，C++ 版每次大改都要与它同种子对拍。
3. **回归检测** —— 分数掉了要显式发现并记录，而不是当作噪声忽略。

## 目标强度

参考公开基准（1000 局，标准规则）：

| 路线 | 平均分 | 32768 到达率 | 65536 到达率 |
|---|---|---|---|
| 手写启发式 expectimax，depth 8（macroxue/2048-ai） | 711,769 | 80.5% | 3.5% |
| n-tuple + TD + expectimax 6-ply（TDL2048+） | 625,377 | 72% | 0.02% |

本项目的目标定在 **稳定 depth 6 一线（约 600k+ 平均分）**，而不是 depth 8 ——
因为在只有 CPU 的轻薄本上，depth 8 一局要 20 分钟以上，depth 4–6 才是性价比拐点。

## 技术选型

| 层 | 选择 |
|---|---|
| 引擎 | C++20，产出**可移植静态库**（不是可执行程序） |
| 构建 | CMake + Ninja（桌面）/ Gradle + CMake（Android） |
| 测试 | GoogleTest |
| 前端 | 原生 HTML / CSS / JS，无框架无构建 |
| 通信 | WebSocket + JSON（桌面）/ JNI（Android） |

引擎做成静态库而不是进程，是因为**手机 App 里起不了本地服务器**。
桌面服务、命令行、Android 三个消费者共用同一份规则与算法 ——
这样"AI 跑的就是真实规则"这条底线不会因为多一个平台而破掉。

## 目录结构

```
engine\               C++ 引擎，产出可移植静态库 libai2048
  include\ai2048\     公开头文件（引擎对外唯一接口面）
  src\core\           游戏规则：棋盘、移动、合并、生成、计分
  src\ai\             决策算法：搜索与评估
  src\bench\          跑批：随机种子、统计、报表
  src\net\            消费者①：WebSocket 服务（桌面）
  src\cli\            消费者②：命令行入口（跑批、自检）
tests\                与 engine\src 对应的单元测试
web\                  前端（浏览器里跑的那一份）
android\              消费者③：Android 工程 + JNI 薄封装
benchmarks\           基准种子集（可复现性资产，纳入版本管理）
docs\                 简报、协议定义、实验记录
```

## 构建与运行

> ⚠️ 以下命令是**目标形态**，尚未跑通 —— 代码还不存在。
> `CMake` 与 `clang-format` 不在 PATH，需要先用绝对路径或加 PATH。

```sh
# 一次性：让 CMake 与 clang-format 可用
$env:PATH = "D:\Codex Tools\CMake\cmake-3.31.6-windows-x86_64\bin;D:\Codex Tools\clang-format18\clang_format\data\bin;$env:PATH"

# 构建
cmake -S engine -B engine\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build engine\build

# 引擎自检：同种子同结果
engine\build\ai2048-cli.exe selfcheck --seeds benchmarks\seeds-v1.txt

# 起服务，然后打开 web\index.html
engine\build\ai2048-server.exe --port 8765
```

完整的验证命令与前端手工验收清单见 `AGENTS.md`。

## 开发环境

| 工具 | 版本 | 状态 |
|---|---|---|
| MinGW-w64 g++ | 13.1.0 | ✅ 实测通过 |
| CMake | 3.31.6 | ✅ 实测通过（不在 PATH） |
| Ninja | — | ✅ 实测通过 |
| clang-format | 18.1.8 | ✅ 实测通过（不在 PATH） |
| GoogleTest | v1.17.0 | ✅ 2/2 测试通过 |
| Android SDK / JDK | 17 / 21 | ✅ |
| Android NDK | — | ❌ 尚未安装（移动端前置条件） |

## 未确定的事项

见 `AGENTS.md` 末尾「待确认事项」。当前待定：Android UI 方案（WebView vs 原生）、
阶段二网络规模、音效来源、是否做暗色模式、许可证类型。
