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
  src/core/game.cpp
  src/core/rng.cpp
  src/core/version.cpp
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
# 消费者②：命令行入口（跑批、自检）
# ---------------------------------------------------------------------------
add_executable(ai2048-cli src/cli/main.cpp)
target_link_libraries(ai2048-cli PRIVATE ai2048_core)
ai2048_apply_project_flags(ai2048-cli)

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
  )
  target_link_libraries(ai2048_tests
    PRIVATE ai2048_core GTest::gtest_main)
  ai2048_apply_project_flags(ai2048_tests)

  include(GoogleTest)
  gtest_discover_tests(ai2048_tests)
endif()
