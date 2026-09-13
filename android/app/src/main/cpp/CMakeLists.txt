# Android 原生库：把 C++ 引擎编成 .so，经 JNI 暴露给 WebView。
#
# ## 这一层刻意做得很薄
#
# 只有「参数翻译 + 调用 SearchBestMove」，**不含任何算法**。
# 见 AGENTS.md「移动端」的架构约束：不得为 Android 复制第二套规则或第二套 AI，
# 那正是参考原型出现「规则漂移」的原因。
#
# ## 为什么要把 engine 以子目录方式加进来（而不是复制一份源码）
#
# 直接 add_subdirectory 引用**同一份** engine/ 源码，手机与桌面编的是同一批
# 文件、同一份评估函数。复制源码就迟早会分家，而且分家不会报错。
#
# AI2048_DESKTOP_CONSUMERS=OFF：CLI 与 WebSocket 服务是进程，Android 不需要
# 也编不过（依赖 socket / 命令行）。引擎核心 ai2048_core 不受影响。
cmake_minimum_required(VERSION 3.22)
project(ai2048_jni CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 引擎源码目录由 Gradle 传进来（见 app/build.gradle 的 -DAI2048_ENGINE_DIR）。
# 不写死相对路径：Gradle 的构建目录与源码目录不一定在同一层。
if(NOT DEFINED AI2048_ENGINE_DIR)
  message(FATAL_ERROR "必须传 -DAI2048_ENGINE_DIR=<engine 源码目录>")
endif()

set(AI2048_DESKTOP_CONSUMERS OFF CACHE BOOL "" FORCE)
add_subdirectory(${AI2048_ENGINE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/engine)

add_library(ai2048_jni SHARED jni_bridge.cpp)

# 引擎内部头文件是相对 src/ 包含的（如 "core/board.h"），所以要把 src 加进来。
# PUBLIC 头文件目录 engine/include 由 ai2048_core 自己带过来。
target_include_directories(ai2048_jni PRIVATE ${AI2048_ENGINE_DIR}/src)

target_link_libraries(ai2048_jni PRIVATE ai2048_core)

# 手机上不要 -mtune=native：那是给本机 x86 调的，交叉编译到 ARM 无意义，
# 而且 Android 的 clang 会直接报未知参数。
#
# ⚠️ 不要加 -fno-exceptions / -fno-rtti：引擎用到 std::optional、std::string、
# std::array 这些，关掉异常与 RTTI 未必编得过，而这点体积收益不值得冒险。
# 真需要减体积时先量 libai2048_jni.so 的实际大小再决定。
target_compile_options(ai2048_jni PRIVATE -O3)
