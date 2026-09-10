# AI-2048 — 项目规则

> 本文件只写**这个项目特有**的东西。
> 项目目录、命名、临时文件、Git/GitHub 流程、代码风格基线等通用规则见全局
> `E:\dsh\home\AGENTS.md`。
>
> 项目定义（范围、非目标、里程碑）见 `docs/brief.md`。
> 本文件管的是**约束**：什么事不许做、什么东西必须怎么验证。

## 用途与适用范围

2048 的 AI 实验室：C++ 引擎在固定种子集上跑批、调参、对比算法强度，
用可复现的分数证明每次算法改动是不是真的更强。人类可玩的前端是附属能力。

适用范围：`engine\`（C++）、`web\`（前端）、`android\`、`benchmarks\`（基准数据）。
不约束 `E:\DeepSeekProjects\_tmp\AI-2048\` 下的临时产物。

## 已确认决策

| # | 决策 |
|---|---|
| 1 | **主线是 AI 实验室**；人类可玩是附属能力，不为它做升级系统 |
| 2 | **引擎做成可移植静态库**，不是可执行程序；桌面 WebSocket 服务只是它的消费者之一，Android 经 JNI 走同一个库 |
| 3 | **引擎规则是标准 2048**（90% 出 2 / 10% 出 4、新块均匀落空格，单次移动内合并出的块不参与二次合并）；「难度」不再改生成规则，改为：撤销次数、是否给提示、AI 让子步数 |
| 4 | **AI 分两阶段：先 expectimax，再 n-tuple + TD**，顺序不许颠倒 |
| 5 | **不走 macroxue 超大预计算查找表路线**（27GB 内存 / 18GB 磁盘，本机不可能） |
| 6 | 目标定位稳定 **depth 6** 一线（约 600k+ 平均分），不是 depth 8 |
| 7 | **Android 优先**，iOS 暂不做（本机是 Windows，编译 iOS 需要 macOS + Xcode） |
| 8 | 手机端范围：**玩 + 看 AI 自动玩**；跑批 / 调参只做桌面 |
| 9 | 前端是**一等交付物**：方块滑动/合并动画 + 音效 + 触屏可玩 |
| 10 | **C++20** |
| 11 | 测试用 **GoogleTest**（已实测 `git ls-remote` 可达，可用 `FetchContent`） |

## 技术栈

| 层 | 选择 | 版本 | 状态 |
|---|---|---|---|
| 引擎语言 | C++ | **C++20** | 已确认 |
| 引擎形态 | 可移植静态库（+ 薄 CLI / 服务 / JNI 封装） | — | 已确认 |
| 编译器（桌面） | MinGW-w64 g++ | 13.1.0 ✔ | 已确认 |
| 编译器（Android） | NDK 的 clang | ⚠️ NDK 尚未安装 | 待安装 |
| 构建（桌面） | CMake + Ninja | CMake 3.31.6 / Ninja ✔ | 已确认 |
| 构建（Android） | Gradle + CMake（NDK 工具链） | ⚠️ 待确认 | 待确认 |
| 测试 | GoogleTest | **v1.17.0**（`FetchContent` 锁定 tag） | ✅ 已实测通过 |
| 格式化 | clang-format | 18.1.8 ✔ | 已确认 |
| 前端（桌面） | 原生 HTML / CSS / JS（无框架无构建） | — | 已确认 |
| 通信（桌面） | WebSocket + JSON | 待定义 | 已确认方向 |
| 通信（Android） | JNI | — | 已确认 |

> **CMake 与 clang-format 不在 PATH**，需要用绝对路径或临时加 PATH：
> CMake `D:\Codex Tools\CMake\cmake-3.31.6-windows-x86_64\bin`、
> clang-format `D:\Codex Tools\clang-format18\clang_format\data\bin`（18.1.8，**已实测可用**）。
>
> ⚠️ **不要用 `D:\Codex Tools\clang-format18\bin\clang-format.exe`** ——
> 那个 108KB 的 wrapper 静默失败（`--version` 都 exit 1、无输出），实测过。
> 这是环境事实，不是本项目的选择。

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

四条边界，比目录本身重要：

- `engine\src\core\` 是**规则引擎**。改动它等于换了一个游戏：必须同步升规则集版本并重建基准种子集。
- `web\` **不得内联 AI 逻辑**。它只负责渲染、输入、调协议。AI 决策一律来自引擎。
- **传输层必须可换。** 前端只依赖一个抽象接口，不依赖 `WebSocket` 具体实现：

  ```js
  // web\js\ai-transport.js   唯一允许知道"怎么连引擎"的文件
  { configure(config), bestMove(state, options),
    evaluateRootDirection(board, direction, options) }
  ```

  这样「JS Worker / WebSocket / Android bridge」三种实现可对换而主流程与 UI 零改动。
  目的不是洁癖：**引擎连不上时，前端仍然能跑**（降级到 JS 基线），而不是白屏。
- `benchmarks\` 是**可复现性资产**，不是缓存。改了要说明原因并记录在 `docs\`。

## 移动端（Android）

### 架构约束（这条决定目录结构，比功能更重要）

手机 App 里**起不了本地服务器进程**。所以引擎必须是静态库，三个消费者共用：

```
        engine\  (C++ 静态库 libai2048)
              ├──► src\net\     桌面：WebSocket 服务器 → 浏览器前端
              ├──► android\     Android：JNI 薄封装 → Kotlin / 前端
              └──► src\cli\     命令行：跑批、自检
```

**引擎核心绝不能依赖网络、文件系统或线程模型。** 它只接受棋盘、返回决策；
跑批、服务、UI 都是外挂在它上面的消费者。

### 平台分工

| 能力 | 桌面（Windows） | Android |
|---|---|---|
| 人类可玩 | ✅ | ✅ |
| AI 自动演示 | ✅ | ✅ |
| 跑批 / 统计 / 调参 | ✅ 主力 | ❌ 不做 |
| 基准种子集与结果留档 | ✅ | ❌ |
| AI 强度 | depth 6+，带时间预算 | ⚠️ **必须能降级** |

### 手机端 AI 强度必须可调

手机 CPU 单核性能低于本机，且受热降频与耗电约束。**引擎必须支持「按时间预算决策」
而不是「按固定深度决策」**，这样同一份代码在手机上自然降级到 depth 3–5 而不是卡死。
UI 上要能选「快 / 均衡 / 强」三档，本质是给不同的 `timeBudgetMs`。

**验收要求：手机端在"均衡"档下每步不超过 200ms，且不引起可感知的机身发热。**

### 构建链

- 已具备：Android SDK（`ANDROID_HOME` 已设）、JDK 17 / 21、`D:\Codex Tools\Android`、`gradle-home`
- 需要新增：**Android NDK**（编译 C++ 引擎为各 ABI 的 `.so`）。
  **安装前先列一遍 `D:\Codex Tools` 确认没有现成的。**
- `ANDROID_HOME` 与 `ANDROID_SDK_ROOT` 是全局环境变量，**不得为本项目修改**；
  项目专属 SDK / NDK 路径走项目内配置

## 硬件约束（已实测环境事实）

```
AMD Ryzen 5 5500U  ——  Zen 2 架构，6 核 12 线程，2.1 GHz 基准
15.3 GB 可用内存（规划按 16GB 算，且不要假设全部可用）
AVX2     ✅ 可用
AVX-512  ❌ 不支持
BMI2     ⚠️ 指令存在，但 Zen 2 上 PEXT 很慢
```

内存可行性：

| 路线 | 内存需求 | 本机可行性 |
|---|---|---|
| macroxue 超大预计算查找表 | **27GB / 18GB 磁盘** | ❌ 不可能，不要按这条设计 |
| Jaskowski 级多阶段 n-tuple（2⁴ 阶段，1.07e9 参数） | 4.3GB (fp32) / 2.1GB (fp16) | ⚠️ 勉强，训练会吃满 16GB |
| 单阶段 67M 参数 n-tuple | 268MB / 134MB | ✅ 可行 |
| 小网络 17×4-tuple（860,625 参数） | 3.4MB / 1.7MB | ✅ 轻松，且是阶段二验收基线 |
| 位棋盘 + 行查找表 + expectimax depth 6–8 | 几十 MB | ✅ 本机主力目标 |

**推论：**

- **目标定在稳定 `depth 6`，不是 depth 8。** 实测 depth 3→5→8 是 493k→661k→712k 分，
  但速度是 6461→594→17 moves/s。轻薄本上 depth 8 一局 20 分钟以上。
- **无 GPU 可用。** 集成 GPU 对 LUT 密集负载无帮助；n-tuple 瓶颈是**内存访问**，不是算力。
- **`PEXT` 要特别注意**：n-tuple 索引最优雅的写法是 `_pext_u64(board, mask)`，
  但 AMD Zen 1/Zen 2 上 PEXT 极慢（TDL2048 专门提供 `BMI2=no` 开关）。
  **本机是 Zen 2，所以阶段二必须准备「按 tuple 形态模板特化索引」的回退路径。**
  先用基准实测 PEXT vs 手写特化的差距，再决定用哪个 —— 不要凭报告下结论。

编译选项：`-O3 -mtune=native`，**不要用 `-march=native`**（TDL2048 明确说明后者更慢）。
AVX2 可以放心用。

### 跑批规模（受 6 核 + 16GB 约束）

- depth 5 约 594 moves/s（单线程量级），一局约上千步 → **每局几秒**
- **日常种子集 100 局** ÷ 6 核 → 分钟级，适合每次改动都跑
- **验收种子集 1000 局** → 十几分钟到半小时，只在里程碑验收时跑

⚠️ **跑批的随机数必须来自引擎自己的 PRNG，且并行不能改变结果** ——
即第 N 局的种子由种子集决定，与它跑在哪个线程无关。这是确定性的必要条件。

## 安装与启动

```sh
# 一次性：让 CMake 与 clang-format 可用（当前不在 PATH）
$env:PATH = "D:\Codex Tools\CMake\cmake-3.31.6-windows-x86_64\bin;D:\Codex Tools\clang-format18\clang_format\data\bin;$env:PATH"

# 配置与构建
cmake -S engine -B engine\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build engine\build

# 引擎自检：同种子同结果（M1 的验收命令）
engine\build\ai2048-cli.exe selfcheck --seeds benchmarks\seeds-v1.txt

# 跑批
engine\build\ai2048-cli.exe bench --seeds benchmarks\seeds-v1.txt --out docs\results\

# 起服务（默认 127.0.0.1:8765，默认深度 8）
engine\build\ai2048-server.exe
engine\build\ai2048-server.exe --port 8765 --depth 8

# 前端：ES module 在 file:// 下会被拦，必须走 HTTP。另开一个终端：
npx serve web
# 浏览器打开它给出的地址即可
```

> **双击也能跑** `ai2048-server.exe`：用默认端口与深度直接启动。
>
> ⚠️ 服务端**没有任何鉴权**，默认只监听回环地址。把它改到外部地址等于把 CPU 交出去。

### 引擎端口不是游戏页面

`ai2048-server` 是**纯 WebSocket 服务**，只用来说协议、给 AI 决策，
**不返回网页**。在浏览器里直接打开 `http://127.0.0.1:8765/` 是打不开游戏的。

为了不让这变成"服务是不是坏了"的困惑，服务端对**没带 `Upgrade` 头**的
普通 HTTP 请求返回一个 HTML 说明页（含实际端口与启动前端的命令），
而不是 400 —— 400 只在终端可见，浏览器里是一片空白。

- 说明页里的引擎地址**从实际监听端口生成**，不写死 8765（用户可改 `--port`）。
- 带 `Upgrade` 头但取值不对的，**仍然如实报 400 并记日志** ——
  那是真的握手失败，不该被说明页掩盖。
- `HasUpgradeHeader()` 只看头名在不在，逐行查找而不是在全文中搜字符串。

### 中文显示与控制台编码

Windows 控制台默认用本地代码页（简中系统是 GBK / 936）解读程序输出，
而本项目源码与输出都是 UTF-8 —— 于是双击运行时 `已启动` 会显示成 `锛堣鍒欓泦`。
**这不是程序输出错了，是两边编码没谈拢。**

两个入口（`ai2048-cli` / `ai2048-server`）都在 `main` 开头调用
`ai2048::EnableUtf8Console()`，把控制台代码页切成 UTF-8（65001）后再输出。

- **不加"仅当 stdout 是控制台才切"的判断。** 曾经加过，是错的：
  那样会让"双击时正常"和"重定向后字节正确"变成两条分叉路径，
  而其中一条永远不会被测试覆盖到（测试子进程的 stdout 恰好就是管道，
  于是测试查了个寂寞）。实测表明被重定向时设置它**没有副作用** ——
  写出的字节本来就与控制台代码页无关。
- 防回归有两层测试，缺一不可：
  - `ConsoleCodepage.*`（C++）：起一个**真正带控制台**的子进程，
    先设成 936 复现双击状态，再验子进程把它切到了 65001。
  - `server.test.mjs` 的字节断言：验输出**原始字节**含 `E5 B7 B2`（"已"）。
    只断言字符串是不够的 —— GBK 误读产生的乱码在字节层面仍是合法 UTF-8，
    字符串断言完全捕不到。

## 格式化与静态检查

```powershell
# 取源文件列表（clang-format 自己不会展开 ** 通配符，PowerShell 也不展开）
$cf = (Get-ChildItem engine\src, engine\tests, engine\include -Recurse -Include *.cpp,*.h).FullName

# 格式化
clang-format -i --style=file $cf

# 检查（0 = 通过）
clang-format --dry-run --Werror --style=file $cf
```

> 实测通过：`.clang-format` 为 Google 风格 + C++20 + 100 列。

## 测试与验证

**两条工作流，验证方式不同，都要各自跑通。**

### 工作流 A：C++ 引擎

> **里程碑 1 已完成，以下命令全部实测通过：**
> `clang-format` exit 0；Release 与 Debug 构建均**无警告**；
> `ctest` **43/43 通过**；`selfcheck` 在 100 局与 1000 局上均逐字节一致；
> `bench` 1000 局耗时 0.05 s（12 线程）。
>
> 规则正确性的主要依据不是手写期望值，而是与 **Gabriele Cirulli 原始实现**
> （`game_manager.js`）在**全部 65536 种单行状态**上逐行对拍 —— 见
> `BoardRowExhaustive.MatchesAuthoritativeReferenceOnAllRows`。
> 开发中它抓出了三个真实缺陷：得分打包静默截断、右移少一次反排、
> 以及 `SlideRow` 未先压实就合并。

```powershell
$cf = (Get-ChildItem engine\src, engine\tests, engine\include -Recurse -Include *.cpp,*.h).FullName
clang-format --dry-run --Werror --style=file $cf      # 格式
cmake -S engine -B engine\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build engine\build
ctest --test-dir engine\build --output-on-failure      # 需 -DAI2048_BUILD_TESTS=ON
engine\build\ai2048-cli.exe selfcheck --seeds benchmarks\seeds-v1.txt
engine\build\ai2048-cli.exe bench --seeds benchmarks\seeds-v1.txt --tag <本次改动>
```

**单元测试需要显式打开**（GoogleTest 走 `FetchContent`，要联网），
且**用独立的构建目录** `engine\build-tests`，避免每次跑测试都重配主构建：

```powershell
cmake -S engine -B engine\build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DAI2048_BUILD_TESTS=ON
cmake --build engine\build-tests
ctest --test-dir engine\build-tests --output-on-failure
```

实测数据：首次配置要联网拉 GoogleTest，**耗时约 5.5 分钟**（333.7 秒）；
之后增量构建约 1 分钟。**离线环境下这一步会失败**，此时只跑不带测试的构建。

> ⚠️ `.gitignore` 里必须用 `build*/` 而不是 `build/` —— 本项目有两个构建目录，
> 只写 `build/` 会漏掉 `build-tests/`，把 GoogleTest 的整个检出（约 7 MB）提交进去。

改动 AI 或规则时，**额外**要给出与上一版的对照表（见「项目专属限制」第一条）。

### 工作流 B：前端

```sh
# ES module 在 file:// 下会被拦，必须走 HTTP。任选一个静态服务器
npx serve web
# 或者用引擎自带的 server（里程碑 4 之后）
engine\build\ai2048-server.exe --port 8765
```

**测试**（Node 直跑，零依赖，不需要浏览器）：

```sh
node tests\run-all.mjs         # 一次跑完全部六套（推荐）
node tests\parity.test.mjs     # 规则一致性：与 C++ 引擎穷举对拍（262144 条 + 30 局）
node tests\json-parity.test.mjs # 自写 JSON 解析器：与 JSON.parse 对拍
node tests\renderer.test.mjs   # 渲染/动画：headless 跑真实 renderer
node tests\input.test.mjs      # 输入：键位、滑动、输入锁
node tests\degraded.test.mjs   # 降级路径：无 WebAudio、连不上引擎
node tests\server.test.mjs     # 服务端端到端：真起 ai2048-server.exe 进程
```

> `server.test.mjs` 是本项目**唯一能验证 WebSocket 服务端是否真的可用**的手段
> （42 项断言：RFC 6455 握手、configure、best-move、错误路径、多客户端并发、
> 断线重连）。它的客户端是 `tests\ws-client.mjs` —— 刻意自己实现，
> 握手用 Node 内置 `crypto` 算 SHA-1，与服务端自写的 SHA-1 是**两套独立实现**，
> 两边能握上手才说明都符合标准。若共用一份代码，就变成自己跟自己对。
>
> ⚠️ **不要用 `Start-Process` 起短命客户端进程手工验证服务端。** 连接会在
> cmdlet 退出时被拆掉，`select` 自然报 0，看起来像服务端坏了 —— 实测被这个
> 误导了很久。验证一律写成常驻测试脚本。

> `parity.test.mjs` 与 `server.test.mjs` 都要先构建引擎
> （前者调用 `ai2048-cli.exe trace` / `move` 取真值，后者要 `ai2048-server.exe`）。
> 找不到可执行文件时**直接失败**，不静默跳过 —— 跳过等于没有验证。
>
> `renderer.test.mjs` / `input.test.mjs` 用 `tests\dom-stub.mjs`（一个极小的 DOM 桩）
> 在 Node 里跑真实模块。它覆盖**逻辑与状态一致性**，不覆盖视觉。

**视觉与手感只能人工验收。** 每次前端改动都要实际走一遍下面这份清单
并把结果写进报告 —— 不许写"应该可以了"：

- [ ] 键盘四方向各走一次：方块从旧位置**平滑滑到**新位置，不停顿、不跳变
- [ ] 有一次合并发生：两块滑到一起后**弹一下**，数值更新，分数浮字 `+N` 出现后飘散消失
- [ ] 新生成的方块有缩放淡入，且位置与逻辑格子一致（不是出现在别处再跳过来）
- [ ] 连续快速按方向键 10 次：不卡死、无重影、无"逻辑已推进但画面没跟上"的错位
- [ ] 触屏（或浏览器设备模拟成触屏）滑动能操作；短距离拖动被当作点击而不走子
- [ ] 音效：**首次点击/按键之后**才有声音（autoplay 解锁）；合并音随方块变大而升高；`M` 与按钮都能静音且刷新后保持
- [ ] 撤销：棋盘与分数都回退一步，按钮在无历史时禁用，`U` 与 `Backspace` 等效
- [ ] 刷新页面后最高分仍在（localStorage 生效）
- [ ] 走到终局：遮罩出现、"撤销一步"与"再来一局"都可用
- [ ] 走到 2048：庆祝面板出现并在约 1.4 秒后自动收起，**不阻断继续玩**
- [ ] 开一次 `prefers-reduced-motion`（浏览器模拟）后：动画被跳过，但方块位置**立即到位**（不能停在旧位置）
- [ ] 用 `?engine=ws://127.0.0.1:8765` 打开但引擎没起：页面正常可用，**顶部出现降级提示**，不是白屏
- [ ] 窗口从窄拉到宽（或旋转屏幕）：方块跟着重排，不错位、不重叠

> 前四项与"降级提示"这几条，逻辑部分已由 `tests\renderer.test.mjs` 与
> `tests\input.test.mjs` 覆盖；**但"平滑""弹一下""飘散"这些观感只能人工看**。

## 项目专属限制

- **改算法必须用分数说话。** 禁止用「感觉变强了」或单局最高分声称改进。
  任何 AI 改动都要给出基准种子集上的平均分、达到 2048/4096/8192 的比例，
  以及改动前后的对照。跑不了就如实说明跑不了。
- **`src\core\` 的改动是重型改动。** 必须同时更新规则集版本、重建种子集基线、
  重跑全部对照实验。不允许为了「让 AI 分数好看」而放宽规则。
- **前端不得代替引擎判断合法性。** 能不能移动、是否终局，一律由引擎回答。
- **不用非标准生成规则作弊。** 被参考的 JS 原型里「难度」会按概率把新块塞进四角
  （简单难度 70%），那是**已废弃的做法**，不得带进新引擎。
- **单步决策是硬实时预算。** `timeBudgetMs` 是上限而非建议：**超时也必须返回
  已完成搜索中的最佳合法步**，不得返回 `null`、不得抛异常。
  原型在这里有真实缺陷 —— 4 个 root 方向全部超时后返回 `legal: true` 但分数为
  `-Infinity`，稳定排序后**静默退化成反复走第一个方向**，且无 fallback。
  新引擎必须显式定义超时后的降级路径（例如回落到上一深度的结果）。
- **位宽是"够用"还是"通用"，要一次定死。** 主流 C++ 实现（nneonneo / TDL2048 / macroxue）
  都用 **4 bit/格** 存 exponent，`uint64_t` 正好装下 4×4 棋盘 —— 这是有意的设计。
  它的天花板是 32768，而实测最强 AI 的 65536 到达率也只有 3.5%，所以够用。
  被参考的原型问题不在 4 bit，在于**越界时静默 clamp**：编码 65536 被悄悄写成 32768，
  两个 32768 相撞也合不出 65536，于是「AI 模拟的规则」与「现实规则」静默错位。
  **必须显式定义上限常量并在编码时断言**，越界要报错而不是 clamp。
- **参考行为要"照抄"还是"改对"，必须逐条记录。** 例如 JS 原型的 `tableScore`
  把空格在行、列上各计一次，等于空格有效权重被放大到约 2×260 + 360 ≈ 880
  （残局 1240），而不是配置里写的 360。当 C++ 版分数低于 JS 版时，
  必须先排除「是不是把 bug 一起改了」这个解释。这类差异统一记进 `docs\baseline-notes.md`。
- 基准结果留档在 `docs\results\`，跑批产物不进 `_tmp`（那是可复现性资产）。
- **网络 I/O 的 socket 一律非阻塞 —— 包括监听 socket 和 accept 出来的连接。**
  这是本项目已经**实际踩中两次**的故障，代价是服务端表面上"端口在监听、
  进程活着、却对任何请求都不响应"，极难从现象反推：

  1. `accept()`：`select` 报告监听 socket 可读，**只保证至少有一个**待接受连接，
     不保证"取完最后一个后下一次 accept 会失败"。阻塞 socket 上不存在
     "暂时没有连接"这个返回值，`for(;;)` 取空队列后第 N 次 `accept` 会**永久阻塞**，
     整个单线程事件循环停摆。必须靠非阻塞下的 `WSAEWOULDBLOCK` / `EAGAIN` 来终止循环。
  2. `recv()`：同理，`select` 说可读只保证"有数据"，不保证下一次读会返回 0 或出错。
     `while (recv(...) > 0)` 在读完现有数据后会阻塞在第二次 `recv`。
     客户端只发一个握手请求（不带跟帧）时**必然**触发。

  排查方法记在案：日志插桩一度被 stdout/stderr 缓冲吞掉，看起来"什么都没发生"。
  可靠的取证方式是**写文件 + 逐行夹逼**，而不是靠控制台输出。
  另外 `Start-Process -NoNewWindow` 起短命客户端进程做手工验证会误导 ——
  连接在 cmdlet 退出时就被拆掉，`select` 当然报 0，让人误判服务端坏了。
  这类验证要写成**常驻的测试脚本**（`tests\server.test.mjs`），不要手敲一次性命令。
- **禁止把已序列化的 JSON 字符串当值再塞进 JSON。** `Value(Serialize(x))` 会得到
  `"debugInfo":"{\"aiType\":...}"` —— 双重序列化，C++ 侧完全看不出来，
  前端 `JSON.parse` 后拿到的是字符串，所有 `debugInfo.xxx` 都是 `undefined`。
  要嵌入已序列化的结构，必须**反解析成 `Value` 再 Set**（见 `protocol.cpp` 的 `EmbedJson`）。
  协议层的新字段一律加一条集成断言，只靠 C++ 单测发现不了这类错误。

## 依赖策略

**本项目允许使用第三方依赖。** 全局 `AGENTS.md` 第 7 节的要求是
「C++ 项目优先使用项目已有的构建系统与依赖管理方案」，并在新增前做一次评估 ——
**不是禁止引入**。GoogleTest 从里程碑 1 起就是依赖（`FetchContent` 锁定 v1.17.0）。

> ⚠️ **曾经有一次错误陈述**：里程碑 4 规划时我声称"项目不允许引入第三方依赖"，
> 并据此打算把 JSON 解析也手写一遍。那是我把**前端零依赖**（用户为交互量小
> 而做的产品选择）误当成了全局规则。这条记录保留下来，避免重犯。

新增依赖前按全局规则逐条评估，并把结论写在这里：

| 依赖 | 版本 | 锁定方式 | 为什么需要 | 评估结论 |
|---|---|---|---|---|
| GoogleTest | v1.17.0 | `FetchContent` + tag | 单元测试 | 需要联网获取；无等效内置方案 |
| nlohmann/json | v3.12.0 | `FetchContent` + tag | 协议的 JSON 收发 | 单头、零依赖、MIT；手写 JSON 在转义/数字格式/嵌套上极易出错，不值得 |

**为什么 WebSocket 服务端不引库**（这是刻意的例外，不是遗漏）：
候选都不合适 —— IXWebSocket 要 zlib、Boost.Beast 要整套 Boost、
websocketpp 要 Boost.Asio 且需构建、`pinwhell/wspp` 的 README 取回 404 无法核实。

而本项目的协议只用 RFC 6455 的**极小子集**：文本帧、不分片、无扩展、
payload 几十字节、无 TLS。服务端握手 + 分帧约 400 行，且**可以用 Node 的 `ws`
客户端做真实互通测试**（不是自己跟自己测）。这比拉几百 MB 的 Boost 更划算。

依赖相关的硬要求：

- 版本必须**锁定到 tag**，不用分支、不用浮动版本
- 引入前先查是否已有等效依赖，避免同一件事两个库
- 项目专属依赖**不得装进 `D:\Codex Tools`**
- 大依赖（Boost 这类）要先用分数或体积说明理由，不得默认引入
- **命令行参数只用 ASCII。** Windows 的 `argv` 走的是 ANSI 代码页而不是 UTF-8，
  传中文会变成乱码。`bench --tag` 会被写进归档文件名，所以 CLI 直接拒绝非 ASCII 的 tag。
  中文说明写在 `docs\` 里，不用命令行传。
  项目自身输出的中文（`std::cout`）不受影响 —— 那是 UTF-8 字节流，
  只是 PowerShell 控制台按 GBK 解码才显示成乱码。

## AI 算法

分两阶段，**顺序不许颠倒**：

1. **阶段一：expectimax + 手写启发式**（位棋盘 + 行查找表 + 置换表 + 根并行 + 概率剪枝）。
   目标 600k+ 平均分 / 32768 高到达率。
2. **阶段二：n-tuple 网络 + 时序差分学习**，学出的评估函数再塞回 expectimax 做 3–6 ply
   —— 这是当前 SOTA 的组合形态，不是二选一。

**为什么必须先做阶段一**：TD 实现错了不会报错，只会让分数低一点，
而没有可信基线就无法区分「bug」和「正常波动」。
阶段二的第一个验收目标是**复现 Szubert 2014 在小网络上的约 100k 平均分 / 97% 胜率**，
复现成功才允许放大到 67M 参数级。

### 算法侧硬约束

- **expectimax 不能用 alpha-beta**（没有 min 节点）。只能按**累计概率阈值**砍分支。
- **不得用游戏真实分数作为启发式**：会过度鼓励立刻合并。用 空格 / 单调性 / 平滑度 / 合并潜力。
- **单调性项要按 tile 等级加权**，让大数字的不单调被重罚 —— 这是强启发式的关键细节。
- **AI 与游戏引擎共享同一套走子实现。** 不允许 AI 侧再写一份"更快的走子"：
  本项目的价值建立在"AI 跑的就是真实规则"之上，规则漂移是最严重的缺陷类型。
  要快就优化那一份实现（查表、SIMD），不是复制一份。
- **训练产物（权重文件）纳入版本管理前先看体积**；超大权重走 `.gitignore` +
  在 `docs\` 说明获取/再生成方式，不得静默提交几百 MB。

## 待确认事项

| # | 问题 | 为什么重要 | 谁来定 |
|---|---|---|---|
| 1 | 基准种子集的生成方式与内容（日常 100 局 + 验收 1000 局） | 决定单次验证耗时，进而决定迭代速度 | 用户 |
| 2 | 前端代码处置：从原型搬进 `web\` 改造，还是重写 | 决定新仓库是否自包含 | 用户 |
| 3 | 跑批入口：只做命令行，还是也要前端按钮 | 决定引擎是否必须支持无头模式 | 用户 |
| 4 | WebSocket 报文格式定稿 | 引擎与前端之间唯一的契约 | 待 `docs\protocol.md` |
| 5 | **Android UI 走 WebView 套 `web\`，还是原生 Kotlin + Compose** | WebView 只维护一份前端但手感差；原生手感好但动画要写两遍 | 用户 |
| 6 | 阶段二 n-tuple 网络规模：单阶段 67M 参数，还是冒险上 2⁴ 阶段 1.07e9 参数 | 后者要 2–4GB 内存，本机 16GB 下训练风险高 | 用户 |
| 7 | 音效来源：WebAudio 现场合成（零资源）还是音频文件 | 影响仓库体积与版权 | 用户 |
| 8 | 是否需要暗色模式 | 影响配色体系设计，后补成本高 | 用户 |
| 9 | `LICENSE` 类型 | 许可证确定前不创建空文件，不阻塞其他工作 | 用户 |
| 10 | Android NDK 安装位置与版本 | 移动端的前置条件 | 用户 |

### 已确认（2026-09-10）

- **平台配比：Windows 70% / Android 30%。** 精力以 Windows 为主，Android 同步跟进。
- **依赖安全策略放宽：** 不再因"第三方不确定是否安全"而一律排除成熟库。
  允许参考 GitHub 上的源码，但**判断某个依赖是否安全这一关仍然要过**，
  不能因为"现在允许了"就随手引入。引入前仍按「依赖策略」一节记录评估理由。

## 与参考实现（JS 原型）的关系

参考原型：`F:\AI编程\2048-ai2\2048-ai`（纯静态，无构建工具、无 `package.json`、
无测试、无 `.git`、`README.md` 是 0 字节）。它**不是**本项目的一部分，
唯一作用是参考与对照基线。

已逐文件确认的处置（19 个文件全读过）：

| 原型文件 | 处置 | 理由 |
|---|---|---|
| `ai-core.js` + `bitboard.js` + `heuristic-table.js`（1276 行） | **保留为对照基线** | 纯计算、零 DOM、零外部依赖，可在 Node 里跑批量。不改造它就无法回答「C++ 版是不是真的更强」 |
| `game.js`（249 行） | 只作规则参考 | 规则写死 4×4、`Math.random()` 未播种、`move()` 不返回方块轨迹、无 undo、无持久化 |
| `config.js`（87 行） | 参考权重数值 | 其中 `searchMode` 无人读取，`minDepth/maxDepth/iterativeDeepening` 在实战并行路径失效 |
| `index.html`（169 行）+ `css\style.css`（459 行） | 可作视觉基线 | 无依赖纯静态；2…2048 是经典 Gabriele Cirulli 调色板，配色可留 |
| `renderer.js`（205 行） | **必须重写** | 每步 `innerHTML = ""` 后重建 16 个节点，方块没有身份也就没有"上一帧位置"，滑动动画在这一层不可能实现 |
| `input.js`（47 行） | 重写 | 只有键盘；触屏设备上**完全无法操作棋局** |
| `ai-client.js` / `parallel-ai-client.js` / `*-worker.js` | 只作协议参考 | 报文信封 `{id, type, payload}` + Map 关联 Promise 可直接映射成 WebSocket 请求/响应 |
| `tuner.js`（162 行） | 只作思路参考 | 随机变异 + 精英保留，非 CMA-ES；**且其评估环境的规则与 `game.js` 不一致**，结论不可信 |
| `worker-pool.js`（373 行） | **禁止照搬** | 内含第二套游戏规则 `LocalGameSimulator`，与 `game.js` 已发生规则漂移 |
| `ai.js`（824 行） | **不要搬** | 旧版实现，全项目无任何 import，是死代码 |
| `utils.js` + `debug.js`（61 行） | 不要搬 | 同样无人引用 |

**规则重复是那个原型最大的结构性问题**：`moveBoard` 有 4 份几乎逐行相同的实现
（`game.js` / `ai.js` / `worker-pool.js` 三份数组版 + `bitboard.js` 一份查表版），
而规则漂移**已经真实发生**。新项目的对应约束见上文「规则即契约」。
