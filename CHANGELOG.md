# CHANGELOG

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的结构，
版本号在第一次正式发布前不做语义化承诺。

## [未发布]

### 新增

- 项目脚手架：目录结构（`engine\` / `web\` / `android\` / `benchmarks\` / `docs\`）、
  `.gitignore`、`.gitattributes`（强制 LF）、`.clang-format`、
  `README.md`、`AGENTS.md`（项目规则）、`docs\brief.md`（项目简报）、
  `docs\protocol.md`、`docs\baseline-notes.md`
- `engine\`：可移植静态库骨架（`ai2048_core`）+ 命令行入口 `ai2048-cli`
  - `include\ai2048\ai2048.h` 公开接口面：版本与**规则集版本**（契约版本）
  - `ai2048-cli version` 可用；`selfcheck` / `bench` 已定义但未实现（退出码 2）

### 已验证

- 工具链实测通过：g++ 13.1.0 / CMake 3.31.6 / Ninja / clang-format 18.1.8
- `cmake` 配置与构建 exit 0；`clang-format --dry-run --Werror` exit 0
- 单元测试 2/2 通过（GoogleTest v1.17.0，经 `FetchContent` 拉取，首次配置约 5.5 分钟）

### 修复

- `.gitignore` 的构建目录规则由 `build/` 改为 `build*/`：本项目用
  `engine\build` 与 `engine\build-tests` 两个目录，原规则会漏掉后者，
  导致 GoogleTest 的整个检出（约 7 MB）被纳入版本管理

> 尚无任何游戏逻辑。规则引擎见 `docs\brief.md` 的里程碑 1。
