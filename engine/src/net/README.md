// 消费者①：WebSocket 服务，把引擎暴露给桌面浏览器前端。
//
// 这是**消费者**，不是引擎的一部分。引擎核心（src/core、src/ai）不得依赖这里。
//
// 待实现（里程碑 4）：
//   - WebSocket + JSON 报文，格式定稿于 docs/protocol.md
//   - 报文信封沿用参考原型的 {id, type, payload} + 请求响应配对
//   - 引擎未启动时前端必须能降级到 JS 基线运行，而不是白屏
//
// 详见 AGENTS.md 的「移动端／架构约束」与 docs/protocol.md。
