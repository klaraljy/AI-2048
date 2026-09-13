# AI-2048

2048 的 AI 实验室。C++ 引擎在固定种子集上跑批，用可复现的分数证明某个算法改动是不是真的更强；
附带一个动画和音效到位、能在 Windows 桌面和 Android 手机上玩的前端。

> **状态：AI 与前端都已可用**（2026-09-13）。
> 引擎：C++20，规则与 Cirulli 原始算法在全部 65536 种行状态上逐行对拍一致，
> 1000 局 `selfcheck` 逐字节一致，**116 个 C++ 单元测试全绿**。
> AI：expectimax（max + chance）+ 手写评估函数 + 置换表 + 迭代加深 + 自适应深度。
> 前端：零依赖原生 HTML/CSS/JS，**11 个测试套件全绿**（含与 C++ 引擎逐位对拍）。
> Android：可安装的 debug APK（WebView 套 `web/`，离线可玩）。

## 它解决什么问题

网上多数 2048 AI 项目是"跑出来一个高分就发出来"：没有固定的种子集、没有改动前后的对照、
也没有回归检测。结果是**你无法判断一次算法改动到底是真变强了还是运气好**。

这个项目把"AI 到底有没有变强"变成一个可复现、可证伪的实验协议：

1. **固定种子集** —— 每个基准种子集有版本号，所有结果都带版本号，不同规则集下的分数禁止混排。
2. **对拍基线** —— 保留一个已知强度的 JS 原型作为基线，C++ 版每次大改都要与它同种子对拍。
3. **回归检测** —— 分数掉了要显式发现并记录，而不是当作噪声忽略。

本机每局分数的标准差约 15,000，100 局均值标准误约 1,500 —— 这意味着 3~4% 的差异
用两次独立跑批比大小**根本分辨不出来**。所以引擎带一个 `compare` 子命令：
A/B 跑同一批种子，报逐局差值的均值/标准差/标准误/配对 t 检验/符号检验 p 值。

## 目标强度

参考公开基准（1000 局，标准规则）：

| 路线 | 平均分 | 32768 到达率 | 65536 到达率 |
|---|---|---|---|
| 手写启发式 expectimax，depth 8（macroxue/2048-ai） | 711,769 | 80.5% | 3.5% |
| n-tuple + TD + expectimax 6-ply（TDL2048+） | 625,377 | 72% | 0.02% |

本项目的目标定在 **稳定 depth 6 一线（约 600k+ 平均分）**，而不是 depth 8 ——
因为在只有 CPU 的轻薄本上，depth 8 一局要 20 分钟以上，depth 4–6 才是性价比拐点。

### 当前实测（`seeds-v1`、标准难度、单线程）

界面上三档 AI 强度**实际达到的成绩**（5 局小样本，只用于给玩家一个量级感）：

| 档位 | 配置 | 平均分 |
|---|---|---|
| 入门 | depth 4 / 300 ms | 49,439 |
| 中等 | depth 6 / 800 ms | 56,917 |
| 最强 | depth 8 / 2500 ms | 65,353 |

难度默认**困难**（改的是新方块落点分布，**不是标准 2048**，所以分数不与公开基准可比）。
困难档下最强档约 59,712。

> ⚠️ **已经查明的天花板**：chance 节点的分支因子是 `min(空格数, 10) × 2`，
> 有效搜索深度被天然截断在 3~4 步，与 `base_depth` 无关。
> 实测把 `max_depth` 从 8 提到 14，10 局对拍**逐局完全相同** —— 加成从未生效。
> 所以再堆深度没有意义，瓶颈是分支因子。详见 `AGENTS.md`。

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

## 快速开始（桌面）

**双击 `start-ai2048.bat`**，或者用桌面上的 **AI-2048** 图标。

首次使用想装桌面图标的话：

```powershell
# 生成图标（已提交，一般不用重跑）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1

# 在桌面创建带图标的快捷方式
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install-shortcut.ps1
```

图标是 2×2 的 `2/0/4/8`，用游戏里 **32 号方块**的橙底白字（`#fe8b54` / `#fefcf7`）。
`.ico` 含 256/128/64/48/32/16 六个尺寸。

> **为什么需要快捷方式**：Windows 不能给 `.bat` 指定自定义图标 ——
> `.bat` 的图标来自文件类型，没有逐文件的图标位。只有 `.lnk` / `.exe` 能带图标。

## 构建与运行

```sh
# 一次性：让 CMake 与 clang-format 可用（都不在 PATH）
$env:PATH = "D:\Codex Tools\CMake\cmake-3.31.6-windows-x86_64\bin;D:\Codex Tools\clang-format18\clang_format\data\bin;$env:PATH"

# 构建
cmake -S engine -B engine\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build engine\build

# 单元测试（116 个）
cmake -S engine -B engine\build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DAI2048_BUILD_TESTS=ON
cmake --build engine\build-tests
ctest --test-dir engine\build-tests

# 前端与端到端测试（11 个套件）
node tests\run-all.mjs

# 引擎自检：同种子同结果
engine\build\ai2048-cli.exe selfcheck --seeds benchmarks\seeds-v1.txt

# 跑一批基线
engine\build\ai2048-cli.exe bench --seeds benchmarks\seeds-v1.txt --depth 6 --tag my-run

# 起服务（前端会连它做 AI 决策）
engine\build\ai2048-server.exe --port 8765
```

> ⚠️ **改完代码要重建 `engine\build`** —— 启动器硬编码用那个目录的 exe，
> 只重建 `build-dev` 的话启动器跑的还是旧版本，而且日志表面看不出来。
> 详见 `AGENTS.md` 的「三个构建目录」。

完整的验证命令与前端手工验收清单见 `AGENTS.md`。

## 安卓

Android 侧产出可安装的 debug APK：**WebView 套 `web/`**，前端资源打包进 APK，
**完全离线可玩**。

```powershell
$env:GRADLE_USER_HOME = "D:\Codex Tools\gradle-home"
$env:JAVA_HOME        = "D:\Codex Tools\jdk-21"
$env:ANDROID_HOME     = "D:\Codex Tools\Android"
gradle -p android assembleDebug
```

产物在 `android\app\build\outputs\apk\debug\app-debug.apk`（约 2.3 MB）。

### ⚠️ 改完 `web/` 必须同步资源，否则 APK 里还是旧界面

APK 用的是 `android/app/src/main/assets/web/`，它是 `web/` 的**副本**：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\sync-android-assets.ps1
```

脚本会复制并**逐个文件比对大小**，不一致就报错退出。

**忘了同步的后果是：桌面版改了、APK 里还是旧的，而且没有任何报错。**
（这条踩过：APK 里的 `style.css` 曾停在 11869 字节，而 `web/` 已经 33 KB。）

### AI 在手机上的现状：**引擎没有编进 APK**

APK 里只有前端。前端连不上引擎时会自动降级到**内置的 JS 本地 AI**
（1 层贪心，权重是旧的），并显示提示条 —— 所以玩是能玩的，但 AI 明显弱一档。

要把 C++ 引擎编进去，路线是 **NDK + JNI 薄封装**（NDK 29.0.14206865 已装好）：

```
engine/  (静态库 ai2048_core，不依赖网络/文件系统/线程模型)
      └──► android/   JNI 薄封装 → Java/Kotlin
```

架构约束（不得违反）：
- 引擎是**可移植静态库**，Android 只是它的消费者之一；
- **不得**为 Android 复制第二套规则或第二套 AI —— 那正是参考原型出现
  「规则漂移」的原因；
- 手机端 AI 强度用**时间预算**而不是固定深度，让同一份代码在手机上自然降级。
  **验收要求：均衡档下每步不超过 200ms，且不引起可感知的机身发热。**

详见 `android/README.md`。

## 开发环境

| 工具 | 版本 | 状态 |
|---|---|---|
| MinGW-w64 g++ | 13.1.0 | ✅ 实测通过 |
| CMake | 3.31.6 | ✅ 实测通过（不在 PATH） |
| Ninja | — | ✅ 实测通过 |
| clang-format | 18.1.8 | ✅ 实测通过（不在 PATH） |
| GoogleTest | v1.17.0 | ✅ 116 个测试全绿 |
| Android SDK / JDK | 35 / 17 / 21 | ✅ APK 可构建 |
| Android NDK | 29.0.14206865 | ✅ 已装（JNI 路线可用，尚未接线） |

## 未确定的事项

见 `AGENTS.md` 末尾「待确认事项」。当前待定：

- **手机端 AI**：APK 目前只有前端 + JS 降级 AI；要不要走 NDK + JNI 把 C++ 引擎编进去
  （NDK 已装好，路线与约束见 `android/README.md`）；
- **Android UI 方案**：当前是 WebView 套 `web/`，要不要换原生 Kotlin + Compose
  （手感更好，但动画要写两遍，且会产生两套需要同步维护的界面）；
- 阶段二网络规模、音效来源、是否做暗色模式、许可证类型。
