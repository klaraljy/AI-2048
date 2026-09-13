cmake_minimum_required(VERSION 3.20)

project(ai2048
  VERSION 0.0.1
  DESCRIPTION "AI-2048 engine: portable 2048 rules + game-playing AI"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 标准与通用编译选项
#
# 注意：按 TDL2048 的结论使用 -mtune=native 而不是 -march=native（后者更慢）。
# AVX2 可用；本机是 AMD Zen 2，不支持 AVX-512，不要写 AVX-512 路径。
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(AI2048_BUILD_TESTS "Build unit tests (requires network for GoogleTest)" OFF)
option(AI2048_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# 桌面消费者（CLI 与 WebSocket 服务）默认构建。
#
# Android 上必须关掉（`-DAI2048_DESKTOP_CONSUMERS=OFF`）：那两个是**进程**，
# 依赖 socket、线程与命令行，在 Android 的 JNI 库里既编不过也没意义 ——
# 手机端要的是静态库 + JNI 薄封装。
# 引擎核心 ai2048_core 不受这个开关影响，三个消费者共用它。
option(AI2048_DESKTOP_CONSUMERS "Build desktop-only consumers (CLI, WebSocket server)" ON)

function(ai2048_apply_project_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(AI2048_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
      target_compile_options(${target} PRIVATE -O3)
    endif()
    if(AI2048_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# 引擎核心：可移植静态库
#
# 这是本项目的唯一核心。它**不得**依赖网络、文件系统或线程模型 ——
# 三个消费者（桌面 WebSocket 服务 / 命令行 / Android JNI）共用它。
# 详见 AGENTS.md「移动端」一节。
# ---------------------------------------------------------------------------
add_library(ai2048_core STATIC
  src/ai/evaluate.cpp
  src/ai/search.cpp
  src/core/board.cpp
  src/core/console.cpp
  src/core/game.cpp
  src/core/rng.cpp
  src/core/version.cpp
  # 握手用的纯计算：sha1.cpp 不碰网络、不开 socket，放在 core 里不违反
  # "核心不得依赖网络"（那是给 src/net 的服务端说的）。
  src/net/json.cpp
  src/net/sha1.cpp
  src/net/websocket.cpp
)

target_include_directories(ai2048_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  # INTERNAL：消费者（CLI / 服务 / 测试）需要 core/*.h 这类内部头，
  # 但用 PUBLIC 会把这些路径传染给消费者的下游；INTERNAL 正是这个语义。
  INTERNAL ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 版本字符串从 project(VERSION ...) 注入，避免两处各写一份而漂移。
target_compile_definitions(ai2048_core
  PRIVATE AI2048_VERSION_STRING="${PROJECT_VERSION}")

ai2048_apply_project_flags(ai2048_core)

# ---------------------------------------------------------------------------
# n-tuple 价值网络 + TD 学习（"换评估函数"路线）
#
# **为什么单独一个库，而不是留在 ai2048_core 里**
#
# core 上面那条注释写着"不得依赖网络、文件系统或线程模型"，三个消费者共用它。
# TD 训练的并行化需要 std::thread，塞进 core 就违反了那条约束。
#
# 拆开之后约束是**真的**成立，而不是靠一句注释撑着：
#   - core     ：无线程、无文件系统，Android / 服务端 / CLI 都能用
#   - net_eval ：n-tuple 网络的**推断**（纯查表，无线程）
#   - train    ：TD 训练，有线程，只有 CLI 链接它
#
# 为什么推断要单独一层而不是并进 core：core 是给 Android 的最小核心，
# 不该被价值网络的文件 IO（Save/Load）拖上。但又不能并进 train ——
# **服务端要用推断却不需要训练**，它不该为了跑推断而链接 std::thread。
# 这一层就是那个缺口，而且这些文件是纯查表 + 文件读写，无线程。
# ---------------------------------------------------------------------------
add_library(ai2048_net_eval STATIC
  src/ai/learn/value_network.cpp
)
target_link_libraries(ai2048_net_eval PUBLIC ai2048_core)
ai2048_apply_project_flags(ai2048_net_eval)

# ---------------------------------------------------------------------------
# TD 训练
# ---------------------------------------------------------------------------
add_library(ai2048_train STATIC
  src/ai/learn/trainer.cpp
)
target_link_libraries(ai2048_train PUBLIC ai2048_net_eval)

# 线程支持：MinGW-w64 的 thread model 是 posix，链接需要显式带上。
# find_package 会在各平台上挑对（Windows 上通常是空的，Linux 上是 -pthread）。
find_package(Threads REQUIRED)
target_link_libraries(ai2048_train PUBLIC Threads::Threads)

ai2048_apply_project_flags(ai2048_train)

# ---------------------------------------------------------------------------
# 消费者②：命令行入口（跑批、自检、训练）
#
# 链接 ai2048_train 是**刻意的**：训练子命令需要线程，跑批/自检不需要。
# ai2048_train 已经把 ai2048_core 传递过来（PUBLIC），所以不用重复链接。
#
# 与下面那个 WebSocket 服务同属**桌面消费者**，Android 下整段跳过
# （见文件开头 AI2048_DESKTOP_CONSUMERS 的说明）。
# ---------------------------------------------------------------------------
if(AI2048_DESKTOP_CONSUMERS)
add_executable(ai2048-cli src/cli/main.cpp)
target_link_libraries(ai2048-cli PRIVATE ai2048_train)
ai2048_apply_project_flags(ai2048-cli)

# ---------------------------------------------------------------------------
# 消费者①：WebSocket 服务（把引擎暴露给浏览器前端）
#
# 这是**消费者**，不是引擎的一部分 —— 引擎核心不依赖它。
# 只有这里可以碰 socket、线程、文件系统。
# ---------------------------------------------------------------------------
add_executable(ai2048-server
  src/net/protocol.cpp
  src/net/server_main.cpp
  src/net/socket_server.cpp
)
# 链接 ai2048_net_eval（不是 ai2048_train）：服务端要跑**推断**，
# 但不需要训练 —— 不该为了推断而链接 std::thread。
target_link_libraries(ai2048-server PRIVATE ai2048_net_eval)

# Windows 的 socket 在 ws2_32 里；POSIX 平台 socket 属于 libc，无需额外链接。
# 注意 MinGW 不提供 <poll.h>，实现用的是 select()（见 socket_server.cpp）。
if(WIN32)
  target_link_libraries(ai2048-server PRIVATE ws2_32)
endif()

# MSVC 下需要定义 _WIN32_WINNT 才能拿到 inet_pton 等较新的 API；
# MinGW 默认已足够新，不加也不影响。
if(MSVC)
  target_compile_definitions(ai2048-server PRIVATE _WIN32_WINNT=0x0601)
endif()

ai2048_apply_project_flags(ai2048-server)
endif()  # AI2048_DESKTOP_CONSUMERS

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if(AI2048_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(ai2048_tests
    tests/board_test.cpp
    tests/console_test.cpp
    tests/difficulty_test.cpp
    tests/evaluate_test.cpp
    tests/net_test.cpp
  )
  target_link_libraries(ai2048_tests
    PRIVATE ai2048_core GTest::gtest_main)
  ai2048_apply_project_flags(ai2048_tests)

  include(GoogleTest)
  gtest_discover_tests(ai2048_tests)
endif()
