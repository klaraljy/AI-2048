# 引擎与前端之间的协议

> **状态：未定稿。** 这是待确认事项之一，实现前必须先把这份写完。

## 为什么这份文件重要

这是引擎与前端之间**唯一的契约**。它同时约束三种消费者：

| 消费者 | 传输 |
|---|---|
| 桌面浏览器前端 | WebSocket + JSON |
| Android App | JNI（不走 JSON，但语义必须一致） |
| 命令行跑批 | 直接调静态库，不走协议 |

Android 那条是关键：如果协议只在 JSON 层定义，JNI 侧就会长出第二套语义，
那就又回到了参考原型「规则漂移」的老问题（见 `../AGENTS.md`）。

## 已确定的部分

- 报文信封沿用参考原型的 `{ id, type, payload }` + 请求/响应配对，
  这样前端已有的 `AIWorkerClient.send` / `WorkerRPC.post` 两个函数可以直接改造成 transport。
- 前端只依赖一个抽象接口，不依赖 WebSocket 具体实现：

  ```js
  // web/js/ai-transport.js  唯一允许知道"怎么连引擎"的文件
  { configure(config), bestMove(state, options),
    evaluateRootDirection(board, direction, options) }
  ```

- `timeBudgetMs` 是**硬上限**：超时也必须返回已完成搜索中的最佳合法步，
  **不得返回 `null`、不得抛异常**。参考原型在这里有真实缺陷
  （4 个 root 方向全部超时后静默退化成反复走第一个方向），必须在新实现里修掉。

## 待定部分

- [ ] 报文的具体字段与命名
- [ ] `debugInfo` 的字段集（UI 面板要显示每步耗时、搜索深度、各方向评分）
- [ ] 错误与超时的表达方式
- [ ] 版本协商：引擎与前端协议版本不一致时怎么办
- [ ] JNI 侧如何做到与 JSON 侧语义一致（共用同一份 C++ 结构体？还是各自映射？）
