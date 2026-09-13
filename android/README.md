# Android App

> **状态：可产出可安装的 debug APK（约 2.3 MB）。** WebView 套 `web/`，
> 前端资源打包进 APK，**完全离线可玩**。引擎（走子 AI）**未编进 APK** ——
> 见下面「AI 在手机上的现状」。

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

## 状态

**已能产出可安装的 debug APK（2.1 MB）。** 采用 **WebView 套 `web/`** 方案，
前端资源打包进 APK，**完全离线可玩**。

未做的部分：引擎（走子 AI）**没有**编进 APK —— 见下面「AI 在手机上的现状」。

## 构建

Gradle 与 JDK 都在全局工具目录里，**不需要下载任何东西**：

```powershell
$env:GRADLE_USER_HOME = "D:\Codex Tools\gradle-home"
$env:JAVA_HOME        = "D:\Codex Tools\jdk-21"
$env:ANDROID_HOME     = "D:\Codex Tools\Android"

$gradle = "D:\Codex Tools\gradle-home\wrapper\dists\gradle-8.11.1-all\2qik7nd48slq1ooc2496ixf4i\gradle-8.11.1\bin\gradle.bat"
& $gradle -p android assembleDebug --no-daemon --console=plain
```

产物：`android\app\build\outputs\apk\debug\app-debug.apk`

> 那个 `2qik7nd48slq1ooc2496ixf4i` 是 Gradle wrapper 的哈希目录名。
> 换机器时别照抄 —— 用 `gradle wrapper` 生成 wrapper，或直接装 Gradle。

### ⚠️ 同步前端资源（改完 `web/` 必做）

APK 里用的是 `android/app/src/main/assets/web/`，它是 `web/` 的**副本**。
用脚本同步并校验（推荐）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\sync-android-assets.ps1
```

脚本会复制 + **逐个文件比对大小**，不一致就报错退出。手敲的话：

```powershell
Copy-Item web\* -Destination android\app\src\main\assets\web -Recurse -Force
Remove-Item android\app\src\main\assets\web\README.md
```

**忘了同步的后果是：桌面版改了、APK 里还是旧的，而且没有任何报错。**
（实测踩到过：APK 里的 `style.css` 停在 11869 字节，而 `web/` 已经 33 KB，
打出来的包装的还是几轮之前的界面。`web/README.md` 是开发文档，不进 APK。）

### 构建踩过的两个坑（都已修，记下来省得重踩）

1. **XML 注释里不能出现连续两个减号。** 我在 `styles.xml` 的注释里写了
   CSS 变量名（那两个减号开头的），AGP 直接报
   `注释中不允许出现字符串 "--"`。注释里改用文字描述。
2. **Kotlin 标准库重复。** `androidx.webkit 1.12.1` 传递依赖了
   `kotlin-stdlib-jdk7/jdk8 1.6.21`，而 kotlin 1.8 起已把这两个的内容
   合并进主 stdlib，于是 classpath 上出现重复类，构建失败：
   `Duplicate class kotlin.io.path.PathsKt found in modules ...`。
   修法是在 `app/build.gradle` 里 `exclude` 掉那两个旧 artifact。

### 版本组合（本机缓存里已有的，别随手升级）

| 组件 | 版本 | 说明 |
|---|---|---|
| Gradle | 8.11.1 | 已在 `gradle-home` 缓存里 |
| AGP | 8.7.2 | 配 Gradle 8.11.1 |
| JDK | 21 | `D:\Codex Tools\jdk-21` |
| compileSdk / targetSdk | 35 | `platforms/android-35` 已装 |
| minSdk | 24 | Android 7.0 |

升级 AGP 会触发几百 MB 的依赖下载，而本项目对版本没有特殊要求 ——
要升之前先确认 `gradle-home\caches` 里有对应版本。

## AI 在手机上的现状

**APK 里没有引擎。** 前端在连不上引擎时会自己降级到内置的本地 AI，
并在界面上显示提示条，所以玩是能玩的，只是 AI 强度是「降级档」。

要把引擎编进去，需要 **NDK + JNI 薄封装**：

```
engine/  (静态库 ai2048_core)
      └──► android/   JNI 薄封装 → Kotlin / Java
```

架构约束（**不得违反**）：
- 引擎是**可移植静态库**，Android 只是它的消费者之一。
- **不得**为 Android 复制第二套规则或第二套 AI —— 那正是参考原型
  出现「规则漂移」的原因。要快就优化那一份实现。
- 引擎核心不得依赖网络、文件系统或线程模型（`ai2048_core` 已满足，
  见 `engine/CMakeLists.txt` 的库分层说明）。
- **手机端 AI 强度用「时间预算」而不是「固定深度」**，这样同一份代码
  在手机上自然降级。UI 的「快 / 均衡 / 强」三档本质是给不同的 `timeBudgetMs`。
  **验收要求：均衡档下每步不超过 200ms，且不引起可感知的机身发热。**

### NDK

**已装好** `ndk;29.0.14206865`（`D:\Codex Tools\Android\ndk\29.0.14206865`）。
**只有 JNI 那条路线才需要它** —— 当前的 WebView 方案是纯 Java，不依赖 NDK。

## 环境

已具备：Android SDK（`ANDROID_HOME` 已设）、JDK 17 / 21、`D:\Codex Tools\Android`、`gradle-home`。
`ANDROID_HOME` 与 `ANDROID_SDK_ROOT` 是全局变量，**不得为本项目修改**。

## 待决定

- [ ] UI 方案最终定稿：当前是 WebView 套 `web/`（已能出包）。
      要不要换原生 Kotlin + Compose —— 手感更好，但动画要写两遍，
      而且会和前端产生**两套需要同步维护的界面**。
- [ ] 引擎的 JNI 接口定稿（语义必须与 `../docs/protocol.md` 一致）
