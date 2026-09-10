// 游戏规则：位棋盘、走子、合并。
//
// 编码：整个 4x4 棋盘装进一个 uint64_t，每格 4 bit 存**指数**而不是数值。
//   空 = 0，2 = 1，4 = 2，...，32768 = 15。
// 取指数而不是数值，是为了让"合并 = 指数加一"，并让整行正好 16 bit
// —— 于是行查找表天然就是 65536 项。这也是 nneonneo / TDL2048 / macroxue
// 的共同做法。
//
// ⚠️ 4 bit 的硬上限是 32768。见 kMaxExponent 的说明。

#ifndef AI2048_CORE_BOARD_H_
#define AI2048_CORE_BOARD_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ai2048 {

// 标准 2048 是固定 4x4。棋盘尺寸不做参数化 —— 变体棋盘（5x5 等）属于
// docs/brief.md 里「以后可能做」的部分，现在参数化只会让位运算逻辑复杂化。
inline constexpr int kBoardSize = 4;
inline constexpr int kCellCount = kBoardSize * kBoardSize;  // 16
inline constexpr int kBitsPerCell = 4;

// 每格的指数上限。4 bit 能表示 0..15，所以最大瓦片是 2^15 = 32768。
//
// 这个上限是**有意的取舍**：主流 C++ 实现都用 4 bit，因为 16 格 x 4 bit 正好
// 塞满一个寄存器。代价是到不了 65536，而实测最强的 AI（macroxue，depth 8）
// 的 65536 到达率也只有 3.5%，所以够用。
//
// 关键约束：**越界时不得静默 clamp**。参考原型就是这么错的 ——
// 它把 65536 悄悄写成 32768，于是"AI 模拟的规则"与"现实规则"静默错位。
// 这里改成显式计数并暴露出来，由调用方决定怎么处理。
inline constexpr int kMaxExponent = 15;

// 单行（4 格）打包成一个 uint16_t，低 4 bit 是最左列。
using PackedRow = std::uint16_t;
inline constexpr std::uint32_t kRowStates = 1u << (kBitsPerCell * kBoardSize);  // 65536

// 取第 index 格（0..15，行优先）的指数。
[[nodiscard]] constexpr int GetExponent(std::uint64_t board, int index) noexcept {
  return static_cast<int>((board >> (kBitsPerCell * index)) & 0xFu);
}

// 返回把第 index 格设为给定指数后的棋盘。
[[nodiscard]] constexpr std::uint64_t SetExponent(std::uint64_t board, int index,
                                                  int exponent) noexcept {
  const int shift = kBitsPerCell * index;
  const std::uint64_t mask = std::uint64_t{0xFu} << shift;
  return (board & ~mask) | ((static_cast<std::uint64_t>(exponent) & 0xFu) << shift);
}

// 指数 -> 瓦片数值（0 -> 0）。
[[nodiscard]] constexpr std::uint64_t ExponentToValue(int exponent) noexcept {
  return exponent <= 0 ? 0u : (std::uint64_t{1} << exponent);
}

// 瓦片数值 -> 指数。数值必须是 0 或 2 的幂；否则返回 -1。
[[nodiscard]] int ValueToExponent(std::uint64_t value) noexcept;

// 从一个 4x4 的指数数组打包成棋盘。越界指数不做校验，由调用方负责。
[[nodiscard]] std::uint64_t EncodeBoard(const std::array<int, kCellCount>& exponents) noexcept;

// 解包成 4x4 的指数数组（行优先）。
[[nodiscard]] std::array<int, kCellCount> DecodeBoard(std::uint64_t board) noexcept;

enum class Direction : std::uint8_t { kUp = 0, kDown = 1, kLeft = 2, kRight = 3 };

[[nodiscard]] const char* DirectionName(Direction direction) noexcept;

// 一个格子在一次走子中的移动记录。前端做滑动动画靠的就是这个。
struct TileMove {
  int from_row = 0;
  int from_col = 0;
  int to_row = 0;
  int to_col = 0;
  int exponent = 0;     // 移动后的指数（被合并的那块记的是合并结果）
  bool merged = false;  // true 表示这块是"被并入"的那一块，前端应让它消失
};

// 一次走子的完整结果。
struct MoveResult {
  bool moved = false;              // false = 这个方向走不动，调用方不应消费随机数
  std::uint64_t board = 0;         // 走子后的棋盘（**尚未**生成新方块）
  std::uint64_t score_gained = 0;  // 本次合并获得的分数
  std::vector<TileMove> moves;     // 方块轨迹，供前端动画使用
  bool overflow = false;           // 见 kMaxExponent：上限处的合并会饱和而不是进位
};

// 对给定棋盘执行一次走子。不生成新方块、不改分数 —— 那是 Game 的职责。
//
// 注意：这里**不做合法性判断以外的任何随机操作**，所以同一个棋盘 + 同一个方向
// 永远得到同一个结果。确定性就建立在这上面。
[[nodiscard]] MoveResult ApplyMove(std::uint64_t board, Direction direction) noexcept;

// 是否还存在合法走子（即是否未终局）。
[[nodiscard]] bool HasLegalMove(std::uint64_t board) noexcept;

// 至少返回一个合法方向；若无合法走子则返回 std::nullopt。
[[nodiscard]] std::optional<Direction> FindAnyLegalMove(std::uint64_t board) noexcept;

// 棋盘上最大的瓦片指数。
[[nodiscard]] int MaxExponent(std::uint64_t board) noexcept;

// 空格数量。
[[nodiscard]] int CountEmptyCells(std::uint64_t board) noexcept;

// 人类可读的棋盘，4 行 x 4 列，便于 CLI 与测试输出。
[[nodiscard]] std::string ToString(std::uint64_t board) noexcept;

}  // namespace ai2048

#endif  // AI2048_CORE_BOARD_H_
