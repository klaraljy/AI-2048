# Android App

> **状态：可产出可安装的 debug APK（约 3.0 MB）。** WebView 套 `web/`，
> 前端资源打包进 APK，**C++ 引擎经 JNI 一并编入**，完全离线可玩。

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

## AI：C++ 引擎已经编进 APK（JNI）

```
engine/  (静态库 ai2048_core)
      └──► android/app/src/main/cpp/jni_bridge.cpp   JNI 薄封装
                └──► NativeEngine.java → window.AI2048Native
                        └──► web/js/transport.js 的 NativeTransport
```

前端按 **原生 > WebSocket > 本地 JS** 的顺序挑后端；探测不到原生就降级，
所以某个 ABI 缺 `.so` 时页面仍然能玩。

架构约束（**不得违反**）：
- 引擎是**可移植静态库**，Android 只是它的消费者之一。
- **不得**为 Android 复制第二套规则或第二套 AI —— 那正是参考原型
  出现「规则漂移」的原因。要快就优化那一份实现。
- JNI 那层**只做参数翻译**，不含任何算法；语义照抄
  `engine/src/net/protocol.cpp`（同样的 SearchConfig 字段、同样在一局内跨步复用置换表）。
- 手机端 AI 强度用**时间预算**而不是固定深度。

### 一致性自检（改了引擎或 JNI 就跑一次）

```powershell
# 桌面的参考结果
engine\build\ai2048-cli.exe selftest --depth 8 --difficulty hard
# 真机上：window.AI2048Native.selfTest(8, "hard")  应当得到同一个串
```

两边跑**同一组写死的局面**（见 `nativeSelfTest` 与 `RunSelfTest`，改一处要改另一处），
结果必须逐条一致。这条专门抓"参数悄悄传错"：棋盘编码顺序、深度单位、难度名、
`last_move` —— 那类错不崩溃，只让 AI 变弱，而手机上分辨不出来。

### 构建环境上的三个坑（都实测踩过）

1. **必须钉住 `ndkVersion`**（`app/build.gradle`）。不写时 AGP 用它自己的默认版本
   （27.0.12077973）并去"安装"——实测触发约 1 GB 下载，而本机已经装了 29。
2. **CMake 要用 `android/local.properties` 里的 `cmake.dir` 指到本机安装**。
   AGP 默认去 Android SDK 的 `cmake/` 目录找，找不到就联网下载；本机网络下这一步
   **卡住不动**（`.temp` 几分钟只有 0.5 MB）。指到 `D:\Codex Tools\CMake\cmake-3.31.6-*`
   就正常了。这个文件是机器相关的，已被 gitignore。
3. **`-DAI2048_ENGINE_DIR` 用 `rootProject.projectDir` 推导，不要手数 `../` 层数**。
   我数错过两次（得到 `E:\DeepSeekProjects\engine` 和 `E:\engine`），
   而 CMake 只会说"目录不存在"，不会告诉你差了几层。

### NDK

**已装** `ndk;29.0.14206865`（`D:\Codex Tools\Android\ndk\29.0.14206865`），
`app/build.gradle` 里钉的就是它。

## 环境

已具备：Android SDK（`ANDROID_HOME` 已设）、JDK 17 / 21、`D:\Codex Tools\Android`、`gradle-home`。
`ANDROID_HOME` 与 `ANDROID_SDK_ROOT` 是全局变量，**不得为本项目修改**。

## 待决定

- [ ] UI 方案最终定稿：当前是 WebView 套 `web/`（已能出包）。
      要不要换原生 Kotlin + Compose —— 手感更好，但动画要写两遍，
      而且会和前端产生**两套需要同步维护的界面**。
- [ ] 引擎的 JNI 接口定稿（语义必须与 `../docs/protocol.md` 一致）
