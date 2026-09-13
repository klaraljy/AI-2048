// Android 端的 JNI 薄封装：把 C++ 引擎暴露给 WebView 里的前端。
//
// ## 为什么要有这一层
//
// 手机上没有本地服务器可连（APK 里起不了 WebSocket 服务），所以前端会降级到
// 内置的 JS 贪心 AI —— 那不是"同一个 AI"，规则一致但**棋力差一大截**。
// 用户要的是"AI 同步"，也就是手机上也用**同一份 C++ 引擎**。
//
// ## 硬性约束（见 AGENTS.md「移动端」，不得违反）
//
//   - 引擎是**可移植静态库** `ai2048_core`，Android 只是它的消费者之一。
//   - **不得**为 Android 复制第二套规则或第二套 AI —— 那正是参考原型出现
//     「规则漂移」的原因。所以这里只做「参数翻译 + 调用」，不含任何算法。
//   - 手机端强度用**时间预算**（`timeBudgetMs`）而不是固定深度，让同一份代码
//     在手机上自然降级到较浅的深度。
//
// ## 与 WebSocket 那条路的一致性
//
// 语义完全照抄 engine/src/net/protocol.cpp：同样的 SearchConfig 字段、
// 同一个 SearchBestMove 调用、同样在**一局之内跨步复用**置换表。
// 两条路唯一的区别是传输方式（JNI 调用 vs WebSocket 报文）。

#include <jni.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

// 引擎头文件的真实路径（内部头在 src/ 下，加 -I engine/src 后按子目录包含）：
//   src/core/board.h      Direction / EncodeBoard / DirectionName
//   src/core/game.h       Difficulty
//   src/ai/search.h       SearchConfig / SearchBestMove / TranspositionTable
//   include/ai2048/ai2048.h  公开接口面：VersionString / RulesetVersion
// ⚠️ 我第一版写成 "ai/board.h"，编不过 —— board.h 在 core/ 下，不在 ai/ 下。
#include "ai2048/ai2048.h"
#include "ai/search.h"
#include "core/board.h"
#include "core/game.h"

namespace {

using ai2048::Difficulty;
using ai2048::Direction;
using ai2048::SearchConfig;
using ai2048::SearchResult;
using ai2048::TranspositionTable;

/** 一局的状态：配置 + 置换表。表要在整局内复用，不能每步清（见 search.h）。 */
struct Session {
  SearchConfig config;
  TranspositionTable table;
};

/** 从 jlong 还原 Session；0 表示还没 Init。 */
Session* AsSession(jlong handle) {
  return reinterpret_cast<Session*>(static_cast<intptr_t>(handle));
}

Difficulty ParseDifficulty(const std::string& name) {
  if (name == "easy") return Difficulty::kEasy;
  if (name == "hard") return Difficulty::kHard;
  return Difficulty::kNormal;
}

/**
 * 方向名 -> 枚举。
 *
 * ⚠️ 引擎没有对外暴露这个转换：`DirectionFromName` 是
 * engine/src/net/protocol.cpp 里的**内部**函数（方向名是协议层的事，
 * 引擎核心只管枚举）。所以这里自己写一份，四个字符串比较而已。
 *
 * 与 protocol.cpp 的那份必须保持一致，否则两条路会对同一个方向名给出不同结果。
 * 那边用小写比较，这里也是。
 */
std::optional<Direction> ParseDirection(const std::string& name) {
  if (name == "up") return Direction::kUp;
  if (name == "down") return Direction::kDown;
  if (name == "left") return Direction::kLeft;
  if (name == "right") return Direction::kRight;
  return std::nullopt;
}

}  // namespace

extern "C" {

/**
 * 新建一局：分配 Session 并设好配置。返回的句柄交给 Java 侧保存，
 * 一局结束时用 NativeDispose 释放。
 *
 * @param base_depth    搜索深度，单位是**树层数**（偶数）。见 search.h 的说明。
 * @param time_budget_ms 每步的时间上限；0 = 不限时（跑批用，手机上别用）。
 * @param difficulty    "easy" / "normal" / "hard"，决定 AI 的世界模型。
 */
JNIEXPORT jlong JNICALL
Java_com_ai2048_app_NativeEngine_nativeInit(JNIEnv* env, jclass, jint base_depth, jint time_budget_ms,
                                            jstring difficulty) {
  auto* session = new Session();

  // 与 protocol.cpp 的 ApplyConfig 保持同样的取值范围，避免两条路行为分叉。
  session->config.base_depth = base_depth < 2 ? 2 : (base_depth > 20 ? 20 : base_depth);
  session->config.time_budget_ms = time_budget_ms < 0 ? 0 : time_budget_ms;

  if (difficulty != nullptr) {
    const char* chars = env->GetStringUTFChars(difficulty, nullptr);
    if (chars != nullptr) {
      session->config.difficulty = ParseDifficulty(std::string(chars));
      env->ReleaseStringUTFChars(difficulty, chars);
    }
  }

  return static_cast<jlong>(reinterpret_cast<intptr_t>(session));
}

/** 释放一局。句柄失效后不得再用。 */
JNIEXPORT void JNICALL Java_com_ai2048_app_NativeEngine_nativeDispose(JNIEnv*, jclass, jlong handle) {
  delete AsSession(handle);
}

/**
 * 算一步。board 是长度 16 的指数数组（0 = 空格，1 = 2，2 = 4 …），
 * 顺序是 row * 4 + col —— 与 web/js/game.js 的表示一致。
 *
 * @param last_move 上一步方向名，nullptr 表示没有（用于抑制左右横跳）。
 * @return 方向名 "up"/"down"/"left"/"right"；**没有合法走子时返回 nullptr**。
 *         调用方把 nullptr 当成"AI 无步可走"。
 */
JNIEXPORT jstring JNICALL Java_com_ai2048_app_NativeEngine_nativeBestMove(JNIEnv* env, jclass,
                                                                         jlong handle,
                                                                         jintArray board,
                                                                         jstring last_move) {
  Session* session = AsSession(handle);
  if (session == nullptr) return nullptr;

  const jsize length = env->GetArrayLength(board);
  if (length != ai2048::kCellCount) return nullptr;

  jint cells[ai2048::kCellCount];
  env->GetIntArrayRegion(board, 0, ai2048::kCellCount, cells);

  std::array<int, ai2048::kCellCount> exponents{};
  for (int i = 0; i < ai2048::kCellCount; ++i) exponents[i] = cells[i];

  const std::uint64_t packed = ai2048::EncodeBoard(exponents);

  std::optional<Direction> last;
  if (last_move != nullptr) {
    const char* chars = env->GetStringUTFChars(last_move, nullptr);
    if (chars != nullptr) {
      last = ParseDirection(std::string(chars));
      env->ReleaseStringUTFChars(last_move, chars);
    }
  }

  const SearchResult result = ai2048::SearchBestMove(packed, session->config, &session->table, last);
  if (!result.move.has_value()) return nullptr;

  return env->NewStringUTF(std::string(ai2048::DirectionName(*result.move)).c_str());
}

/** 清空置换表与搜索状态（新开一局时调）。 */
JNIEXPORT void JNICALL Java_com_ai2048_app_NativeEngine_nativeNewGame(JNIEnv*, jclass, jlong handle) {
  Session* session = AsSession(handle);
  if (session != nullptr) session->table.Reset();
}

/** 引擎版本，形如 "0.0.1"。用于在前端显示"引擎已接上"。 */
JNIEXPORT jstring JNICALL Java_com_ai2048_app_NativeEngine_nativeVersion(JNIEnv* env, jclass) {
  return env->NewStringUTF(std::string(ai2048::VersionString()).c_str());
}

/** 规则集版本号。前端可以拿它校验"手机上的规则与桌面一致"。 */
JNIEXPORT jint JNICALL Java_com_ai2048_app_NativeEngine_nativeRulesetVersion(JNIEnv*, jclass) {
  return ai2048::RulesetVersion();
}

/**
 * 自检：把 native 侧对给定棋盘的决策算出来，供 Java 侧与前端对拍。
 *
 * 存在的理由：JNI 这一层是**新的**，而"新的一层"最容易出的错就是参数传错
 * （深度单位、难度名、棋盘编码顺序）—— 那类错不会崩，只会让 AI 悄悄变弱。
 * 有了这个入口，测试脚本可以让 Java 侧算出决策再和已知答案比。
 *
 * 不传 last_move、不用置换表，所以同一局面**必定得到同一结果**。
 */
JNIEXPORT jstring JNICALL Java_com_ai2048_app_NativeEngine_nativeSelfCheckMove(JNIEnv* env, jclass,
                                                                              jintArray board,
                                                                              jint base_depth,
                                                                              jstring difficulty) {
  const jsize length = env->GetArrayLength(board);
  if (length != ai2048::kCellCount) return nullptr;

  jint cells[ai2048::kCellCount];
  env->GetIntArrayRegion(board, 0, ai2048::kCellCount, cells);

  std::array<int, ai2048::kCellCount> exponents{};
  for (int i = 0; i < ai2048::kCellCount; ++i) exponents[i] = cells[i];

  SearchConfig config;
  config.base_depth = base_depth;
  config.time_budget_ms = 0;  // 不限时，保证确定性

  if (difficulty != nullptr) {
    const char* chars = env->GetStringUTFChars(difficulty, nullptr);
    if (chars != nullptr) {
      config.difficulty = ParseDifficulty(std::string(chars));
      env->ReleaseStringUTFChars(difficulty, chars);
    }
  }

  const SearchResult result =
      ai2048::SearchBestMove(ai2048::EncodeBoard(exponents), config, nullptr, std::nullopt);
  if (!result.move.has_value()) return nullptr;

  return env->NewStringUTF(std::string(ai2048::DirectionName(*result.move)).c_str());
}

/**
 * 自检：在一组**写死**的局面上跑真实决策，返回结果供比对。
 *
 * ## 为什么需要它
 *
 * JNI 这一层是新加的，而"新加的一层"最容易出的错不是崩溃，是**参数悄悄传错**：
 * 棋盘编码顺序、深度单位（树层 vs 玩家步）、难度名拼错、last_move 反了……
 * 这类错不会报错，只会让 AI 变弱，而"变弱"在手机上是看不出来的。
 *
 * 所以这里跑一组确定性局面（不限时、不用置换表），把**完全相同**的输入喂给
 * 桌面引擎（ai2048-cli）跑一遍，两边的方向必须逐条一致 ——
 * 那条命令记在 android/README.md 里。
 *
 * 局面刻意选中局而非开局：开局几个方向评价接近，容易因为浮点差异翻盘；
 * 中局有明确的强弱之分，两边算错就会分道扬镳。
 *
 * @return 形如 "up,left,down" 的结果串；出错时返回 nullptr
 */
JNIEXPORT jstring JNICALL Java_com_ai2048_app_NativeEngine_nativeSelfTest(JNIEnv* env, jclass,
                                                                         jint base_depth,
                                                                         jstring difficulty) {
  // 三个中局局面（指数表示：0 空格、1=2、2=4、3=8、4=16、5=32）
  const int kBoards[3][ai2048::kCellCount] = {
      {5, 4, 3, 2, 4, 3, 2, 1, 3, 2, 1, 0, 2, 1, 0, 0},
      {4, 4, 3, 2, 3, 2, 2, 1, 2, 1, 1, 0, 1, 0, 0, 0},
      {6, 5, 4, 3, 5, 4, 3, 2, 4, 3, 2, 1, 3, 2, 1, 0},
  };

  Difficulty parsed = Difficulty::kNormal;
  if (difficulty != nullptr) {
    const char* chars = env->GetStringUTFChars(difficulty, nullptr);
    if (chars != nullptr) {
      parsed = ParseDifficulty(std::string(chars));
      env->ReleaseStringUTFChars(difficulty, chars);
    }
  }

  SearchConfig config;
  config.base_depth = base_depth;
  config.time_budget_ms = 0;  // 不限时 —— 否则结果依赖机器速度，没法比对
  config.difficulty = parsed;

  std::string out;
  for (const auto& cells : kBoards) {
    std::array<int, ai2048::kCellCount> exponents{};
    for (int i = 0; i < ai2048::kCellCount; ++i) exponents[i] = cells[i];

    // 每次都用全新的表：局面之间不许互相影响，否则比对不可复现
    TranspositionTable table;
    const SearchResult result =
        ai2048::SearchBestMove(ai2048::EncodeBoard(exponents), config, &table, std::nullopt);

    if (!out.empty()) out += ",";
    out += result.move.has_value() ? ai2048::DirectionName(*result.move) : "none";
  }

  return env->NewStringUTF(out.c_str());
}

}  // extern "C"
