# Android App

> **状态：空。** 尚未开始。前置条件是先装 Android NDK（见下）。

## 定位

附属平台：**玩 + 看 AI 自动玩**。跑批、统计、调参只做桌面，不搬手机。

## 架构约束

引擎是**可移植静态库** `engine/`，Android 只是它的消费者之一，经 **JNI 薄封装**调用。
不得为 Android 复制第二套规则或第二套 AI —— 那正是参考原型出现「规则漂移」的原因。

```
engine/  (静态库 libai2048)
      └──► android/   JNI 薄封装 → Kotlin / 前端
```

引擎核心不得依赖网络、文件系统或线程模型。

## 手机端 AI 强度必须可调

手机 CPU 单核性能低于开发机，且受热降频与耗电约束。
**引擎用「时间预算」而不是「固定深度」决策**，这样同一份代码在手机上自然降级到 depth 3–5。
UI 提供「快 / 均衡 / 强」三档，本质是给不同的 `timeBudgetMs`。

**验收要求：均衡档下每步不超过 200ms，且不引起可感知的机身发热。**

## 前置条件

- [ ] **安装 Android NDK** —— 尚未安装。安装前先列一遍 `D:\Codex Tools` 确认没有现成的
- [ ] 决定 UI 方案：WebView 套 `web/`（只维护一份前端，但手感差）
      还是原生 Kotlin + Compose（手感好，但动画要写两遍）
- [ ] 引擎的 JNI 接口定稿（语义必须与 `../docs/protocol.md` 一致）

## 环境

已具备：Android SDK（`ANDROID_HOME` 已设）、JDK 17 / 21、`D:\Codex Tools\Android`、`gradle-home`。
`ANDROID_HOME` 与 `ANDROID_SDK_ROOT` 是全局变量，**不得为本项目修改**。
