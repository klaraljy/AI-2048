# CHANGELOG

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的结构，
版本号在第一次正式发布前不做语义化承诺。

## [未发布]

### 里程碑 1 — C++ 规则引擎 + 确定性（完成）

新增：

- `core/board`：64-bit 位棋盘（4 bit/格 存指数）+ 65536 项行查找表；四方向走子、
  合并、计分、终局判定、**方块移动轨迹**（前端滑动动画的数据来源）
- `core/rng`：xoshiro256** + splitmix64 播种 + Lemire 拒绝采样。
  不依赖 `<random>` —— 标准库不保证具体算法，换版本就可能让历史分数全部失效
- `core/game`：状态机。**非法走子不消耗随机数**（replay 正确性的前提）
- `core/version`：`RulesetVersion()` —— 规则即契约，改动规则必须提升它
- `ai2048-cli`：`play` / `selfcheck` / `bench` 三个子命令
- `benchmarks/seeds-v1.txt`（100 局）与 `seeds-1000-v1.txt`（1000 局）
- `docs/results/README.md`：基线归档与结果记录格式

### 已验证

- 单元测试 **43/43 通过**（Release 与 Debug 均无编译警告）
- `selfcheck`：100 局与 1000 局**全部逐字节一致**
- 规则正确性：与 Gabriele Cirulli 原始 `game_manager.js` 在**全部 65536 种单行状态**
  上逐行对拍一致（左右两个方向，含得分）
- 基线（平凡策略，非 AI）：1000 局平均分 2309，平均 208 步，0.05 s / 12 线程

### 修复

开发过程中由上述穷举对拍与测试抓出的三个真实缺陷：

- **得分被静默截断**：结果行与得分曾打包进一个 `uint32`（`gain << 16 | row`），
  而 `[16384,16384,16384,16384]` 这一行的得分是 65536 = 2^16，塞不进 16 bit，
  分数静默变成 0。改成两个独立数组。穷举对拍只抓出这一个失败用例 ——
  靠人工构造测试几乎不可能想到
- **右移少一次反排**：`Slide(ReverseRow(x))` 的结果落在反排坐标系里，必须再反排回来。
  少了它，`[1,0,0,0]` 右移会原地不动，而满行输入恰好不受影响
- **`SlideRow` 未先压实就找相邻对**：`[2,0,2,0]` 右移会得到 `[0,0,4,0]` 而不是
  `[0,0,0,4]`。满行输入同样看不出来
- **`ProcessesRowReversed` 漏了 `kDown`**：垂直方向在转置坐标系里处理，
  "向下"同样需要反排；漏掉会让下移错用 `left` 表，方向看着能动、只是往错误的一端走
- **轨迹按目标格索引**：一次合并有两张牌竞争同一个目标位，只能记下一条，
  另一张从轨迹里消失。改成按来源牌索引

### 其它

- `.gitignore` 的构建目录规则由 `build/` 改为 `build*/`：本项目用
  `engine\build` 与 `engine\build-tests` 两个目录，原规则会漏掉后者
- CLI 拒绝非 ASCII 的 `--tag`：Windows 的 `argv` 走 ANSI 代码页而非 UTF-8，
  中文会变乱码并写进归档文件名

### 脚手架（上一阶段）

<details>
<summary>初始脚手架</summary>

- 目录结构、`.gitignore`、`.gitattributes`（强制 LF）、`.clang-format`、
  `README.md`、`AGENTS.md`、`docs\brief.md`、`docs\protocol.md`、`docs\baseline-notes.md`

</details>
