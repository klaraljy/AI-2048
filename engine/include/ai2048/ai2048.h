// AI-2048 引擎的公开接口面。
//
// 这是引擎对外唯一允许被引用的头文件目录。三个消费者
// （engine/src/net 的 WebSocket 服务、engine/src/cli 的命令行、android 的 JNI 封装）
// 都只通过这里的接口使用引擎；engine/src 下的其它头文件都是内部实现。
//
// 设计约束（见 AGENTS.md「移动端」）：引擎核心不得依赖网络、文件系统或线程模型。

#ifndef AI2048_AI2048_H_
#define AI2048_AI2048_H_

#include <string_view>

namespace ai2048 {

// 引擎版本字符串，形如 "0.0.1"。
[[nodiscard]] std::string_view VersionString() noexcept;

// 规则集版本号。
//
// 规则实现的任何变更都必须同时提升这个号并重建基准种子集 ——
// 不同规则集下跑出来的分数禁止混排对比。见 AGENTS.md「规则即契约」。
[[nodiscard]] int RulesetVersion() noexcept;

}  // namespace ai2048

#endif  // AI2048_AI2048_H_
