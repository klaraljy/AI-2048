#include "core/board.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <string>

namespace ai2048 {

namespace {

// ---------------------------------------------------------------------------
// 单行的滑动 + 合并
//
// input 是打包好的 4 格行，低 4 bit 是最左列。滑动方向统一"向左"，
// 向右只是把结果反排回去（合并顺序与方向无关，见 MergeOrderIsIndependentOfDirection 测试）。
// ---------------------------------------------------------------------------

struct RowResult {
  PackedRow row = 0;
  std::uint64_t gain = 0;  // 本次合并得到的分数（= 合并结果的数值）
  bool overflow = false;   // 在 kMaxExponent 处合并，饱和而非进位
};

// dst[src] = 原行第 src 格的牌最终落到哪个结果位（-1 = 该格原本是空的）
// consumed[src] = 这张牌是被并入的一方（即它在终点被吃掉）
//
// 按**来源**索引，不是按目标格。一次合并涉及两张来源牌，
// 若按目标格索引就只能记下其中一张，另一张会从轨迹里丢失。
struct SlotMap {
  std::array<int, kBoardSize> dst{};
  std::array<bool, kBoardSize> consumed{};
};

// 把原行的 4 格解成数组，并在滑动/合并过程中记录来源映射。
//
// 这是**唯一**实现规则的地方 —— 行查找表、方块轨迹、测试全都用它，
// 避免出现第二套规则（参考原型就是在这里长出四个版本的 moveBoard 的）。
//
// 步骤必须是「先压实、再合并」，不能边滑边找相邻对：
//   [2,0,2,0] 右移必须得到 [0,0,0,4]，而不是 [0,0,4,0]。
//   两者都有 4，但位置不同 —— 只有先过滤掉空格，两个 2 才会成为"相邻"。
//   这个顺序错误在满行输入（如 [2,2,4,4]）上完全看不出来。
RowResult SlideRow(PackedRow input, SlotMap* map) {
  std::array<int, kBoardSize> in{};
  for (int i = 0; i < kBoardSize; ++i) {
    in[i] = static_cast<int>((input >> (kBitsPerCell * i)) & 0xFu);
  }

  if (map != nullptr) {
    map->dst.fill(-1);
    map->consumed.fill(false);
  }

  // 步骤 1：压实 —— 去掉空格，记录每张牌的原位置。
  std::array<int, kBoardSize> values{};
  std::array<int, kBoardSize> origins{};
  int count = 0;
  for (int i = 0; i < kBoardSize; ++i) {
    if (in[i] == 0) continue;
    values[static_cast<std::size_t>(count)] = in[i];
    origins[static_cast<std::size_t>(count)] = i;
    ++count;
  }

  // 步骤 2：在压实后的序列上合并相邻等值对。
  RowResult result;
  int out = 0;
  int i = 0;
  while (i < count) {
    const int exponent = values[static_cast<std::size_t>(i)];
    const bool can_merge = (i + 1 < count) && (values[static_cast<std::size_t>(i + 1)] == exponent);

    int merged_exponent = exponent;
    if (can_merge) {
      if (exponent >= kMaxExponent) {
        // 已经到上限，合并无法进位。**不静默**：置 overflow 让调用方知道。
        result.overflow = true;
        merged_exponent = kMaxExponent;
      } else {
        merged_exponent = exponent + 1;
        result.gain += ExponentToValue(merged_exponent);
      }
    }

    result.row |= static_cast<PackedRow>(merged_exponent) << (kBitsPerCell * out);
    if (map != nullptr) {
      // 映射**按来源牌**记录，而不是按目标格。
      //
      // 每张原有的牌恰好产生一条轨迹：它滑到的格子，或者它被并入的格子。
      // 若按目标格记录，一次合并会有两张牌竞争同一个目标位，只能记下一条 ——
      // 另一张就会从轨迹里消失，前端的滑动动画会缺一块。
      //
      // consumed 标记的是**被吸收的那一张**（origins[i+1]），不是存活的那张。
      // 标反了会让两张牌都被标记为"消失"，合并后棋盘上就什么都不剩。
      map->dst[origins[static_cast<std::size_t>(i)]] = out;
      map->consumed[origins[static_cast<std::size_t>(i)]] = false;
      if (can_merge) {
        map->dst[origins[static_cast<std::size_t>(i + 1)]] = out;
        map->consumed[origins[static_cast<std::size_t>(i + 1)]] = true;
      }
    }

    ++out;
    i += can_merge ? 2 : 1;
  }

  return result;
}

[[nodiscard]] PackedRow ReverseRow(PackedRow row) noexcept {
  PackedRow reversed = 0;
  for (int i = 0; i < kBoardSize; ++i) {
    const int exponent = static_cast<int>((row >> (kBitsPerCell * i)) & 0xFu);
    reversed |= static_cast<PackedRow>(exponent) << (kBitsPerCell * (kBoardSize - 1 - i));
  }
  return reversed;
}

// ---------------------------------------------------------------------------
// 行查找表（65536 项，进程内一次性构建）
//
// 高 16 bit 存合并得分，低 16 bit 存结果行。一张表同时解决"走子"和"计分"。
// overflow 单独放一张字节表 —— 它只反映"这一行在**向左**处理时是否在
// kMaxExponent 处合并过"，与方向无关（合并是否发生只看牌的构成）。
// ---------------------------------------------------------------------------

struct RowTables {
  std::array<PackedRow, kRowStates> left{};
  std::array<PackedRow, kRowStates> right{};
  std::array<std::uint64_t, kRowStates> left_gain{};
  std::array<std::uint64_t, kRowStates> right_gain{};
  std::array<bool, kRowStates> overflow{};
};

[[nodiscard]] const RowTables& Tables() {
  static const RowTables tables = [] {
    RowTables t;
    for (std::uint32_t state = 0; state < kRowStates; ++state) {
      const auto row = static_cast<PackedRow>(state);
      const RowResult left = SlideRow(row, nullptr);
      const RowResult right = SlideRow(ReverseRow(row), nullptr);

      t.left[state] = left.row;
      // 右移 = 反排 -> 向左推 -> 再反排。**两次反排缺一不可。**
      //
      // 反排后向左推的结果落在**反排坐标系**里，必须再反排回来才是棋盘坐标。
      // 少这一次反排的后果很隐蔽 —— 方块数量与数值都对，只是位置整体颠倒：
      //   [1,0,0,0] 右移会得到 [1,0,0,0]（原地不动）
      //   [2,2,4,4] 右移会得到 [4,8,0,0] 而不是正确的 [0,0,4,8]
      //
      // 判据不是"哪种写法看着对"，而是与权威实现逐行穷举对拍：
      // Gabriele Cirulli 的 game_manager.js（右移从最右列开始遍历，
      // 靠近边缘的那一对先合并）。已验证全部 65536 种行完全一致。
      t.right[state] = ReverseRow(right.row);
      t.left_gain[state] = left.gain;
      t.right_gain[state] = right.gain;
      // 向左与向右的 overflow 必然一致：合并与否只取决于相邻两格是否相等，
      // 而"相邻"在反排下保持不变。
      t.overflow[state] = left.overflow;
    }
    return t;
  }();
  return tables;
}

// 某个方向的走子，是把行**向左推**，还是先反排再向左推。
//
// ⚠️ 这里必须同时包含 kRight 与 kDown。
//
// 垂直方向是在**转置坐标系**里处理的：转置棋盘的第 line 行 = 原棋盘的第 line 列。
// 于是"向右"和"向下"在各自的处理坐标系里都是"朝索引增大的一端推"，
// 都需要反排。漏掉 kDown 会让下移错用 left 表，结果与上移完全相同 ——
// 方向看着"能动"，只是往错误的一端走，非常不容易发现。
[[nodiscard]] constexpr bool ProcessesRowReversed(Direction direction) noexcept {
  return direction == Direction::kRight || direction == Direction::kDown;
}

// 一个方向的查表入口：结果行与得分分开取。
//
// ⚠️ 曾经把得分和结果行打包进一个 uint32（`gain << 16 | row`），
// 结果在两个 16384 合并时被静默截断：那一行得分正好是 65536 = 2^16，
// 塞不进 16 bit 的 gain 字段，于是**分数静默变成 0**。
// 逐行穷举对拍（65536 种行）只抓出这一个失败用例 —— 一个"看起来不可能"
// 的边界，靠人工构造测试基本想不到。
// 教训：不要把可能溢出的量裁进固定宽度的字段里。
struct RowEntryValue {
  PackedRow row = 0;
  std::uint64_t gain = 0;
};

[[nodiscard]] RowEntryValue RowEntry(Direction direction, PackedRow row) noexcept {
  const RowTables& tables = Tables();
  if (ProcessesRowReversed(direction)) {
    return RowEntryValue{tables.right[row], tables.right_gain[row]};
  }
  return RowEntryValue{tables.left[row], tables.left_gain[row]};
}

// 按方向把一行处理成结果行。**走子与方块轨迹共用这一个函数** ——
// 两处各写一份就是参考原型规则漂移的起点。
[[nodiscard]] PackedRow ResultRow(Direction direction, PackedRow row) noexcept {
  return RowEntry(direction, row).row;
}

// 在"处理顺序"下做合并映射：把该方向的结果行与来源映射一次算出来。
[[nodiscard]] RowResult SlideRowForDirection(Direction direction, PackedRow row_as_processed,
                                             SlotMap* map) {
  if (ProcessesRowReversed(direction)) {
    return SlideRow(ReverseRow(row_as_processed), map);
  }
  return SlideRow(row_as_processed, map);
}

[[nodiscard]] constexpr PackedRow ExtractRow(std::uint64_t board, int row) noexcept {
  return static_cast<PackedRow>((board >> (kBitsPerCell * kBoardSize * row)) & 0xFFFFu);
}

[[nodiscard]] constexpr std::uint64_t ReplaceRow(std::uint64_t board, int row,
                                                 PackedRow value) noexcept {
  const int shift = kBitsPerCell * kBoardSize * row;
  const std::uint64_t mask = std::uint64_t{0xFFFFu} << shift;
  return (board & ~mask) | (static_cast<std::uint64_t>(value) << shift);
}

// 转置 4x4 的 4-bit 格棋盘。
//
// 这里是**刻意写成直白形式**的：解包 -> 交换行列索引 -> 重新打包。
//
// 走过弯路：先试了"分块交换"（bit-matrix transpose）的位技巧写法，连错三次 ——
// 掩码看着合理，水平走子也照样全过（因为水平方向根本不经过转置），
// 但垂直方向会整盘错位。而**没有任何测试能区分"掩码写对"和"掩码看着对"**，
// 除非有暴力对拍。
//
// 结论：这一步不值得为常数优化冒正确性风险。转置只在垂直走子时每次调用两次，
// 相比搜索本身（后续会有几十万节点）完全可以忽略。
// 如果将来真的要优化，前提是 tests 里的暴力对拍用例先通过。
[[nodiscard]] std::uint64_t Transpose(std::uint64_t board) noexcept {
  std::uint64_t result = 0;
  for (int row = 0; row < kBoardSize; ++row) {
    for (int col = 0; col < kBoardSize; ++col) {
      const int exponent = GetExponent(board, row * kBoardSize + col);
      result = SetExponent(result, col * kBoardSize + row, exponent);
    }
  }
  return result;
}

// 原棋盘上的一个坐标。垂直方向处理时，转置坐标系里的 (r,c) 对应原棋盘的 (c,r)。
struct LineSlot {
  int row = 0;
  int col = 0;
};

// 在一条线上按方向还原坐标：把「处理顺序」的第 order 位换回原棋盘坐标。
//
// 水平方向：处理顺序对右移是反排的，所以第 order 位对应原棋盘的
//           (3 - order) 列 —— 这一点必须换算，否则轨迹里的「起始格」
//           会指向反排坐标，前端动画会把牌从错误的格子滑出来。
// 垂直方向：转置坐标系里第 line 行 = 原棋盘第 line 列，第 order 位对应第 order 行。
[[nodiscard]] LineSlot SlotForDirection(Direction direction, int line, int order) {
  switch (direction) {
    case Direction::kLeft:
      return {line, order};
    case Direction::kRight:
      return {line, kBoardSize - 1 - order};
    case Direction::kUp:
      return {order, line};
    case Direction::kDown:
      return {kBoardSize - 1 - order, line};
  }
  return {line, order};
}

// 只在 Debug 下生效的一致性校验。
//
// 不用裸 assert：Release 会把 assert 整个编译掉，参数就成了未使用变量，
// 构建会出警告。把条件包在这里，表达式在任何构建下都被求值一次，
// 未使用参数的问题也就消失了。
#define AI2048_DEBUG_CHECK(condition, message)                 \
  do {                                                         \
    const bool ai2048_debug_ok = static_cast<bool>(condition); \
    (void)ai2048_debug_ok;                                     \
    assert(ai2048_debug_ok && (message));                      \
  } while (false)

// 算出一次走子的方块轨迹。
//
// after 用于**一致性校验**：轨迹是从 before 重新推出来的，如果推出来的棋盘
// 与真实结果对不上，说明轨迹逻辑与走子逻辑已经漂移（参考原型最严重的缺陷
// 就是这个类型）。Debug 下直接断言失败，而不是让前端拿到错位的动画数据。
void AppendTrajectory(std::uint64_t before, Direction direction, std::uint64_t after,
                      std::vector<TileMove>* moves) {
  const bool horizontal = direction == Direction::kLeft || direction == Direction::kRight;

  // 与 ApplyMove 完全相同的坐标变换：垂直方向在转置坐标系里处理。
  const std::uint64_t working = horizontal ? before : Transpose(before);
  std::uint64_t reconstructed = 0;

  for (int line = 0; line < kBoardSize; ++line) {
    // 转置坐标系里的第 line 行，就是原棋盘的第 line 行（水平）或第 line 列（垂直）
    const PackedRow packed = ExtractRow(working, line);

    SlotMap map;
    const RowResult slid = SlideRowForDirection(direction, packed, &map);

    // 用与走子同一张表算结果行，写回重建棋盘供末尾校验
    reconstructed = ReplaceRow(reconstructed, line, ResultRow(direction, packed));

    for (int source = 0; source < kBoardSize; ++source) {
      const int dest = map.dst[source];
      if (dest < 0) continue;  // 这一格原本是空的

      const LineSlot from = SlotForDirection(direction, line, source);
      const LineSlot to = SlotForDirection(direction, line, dest);
      const int exponent_in = static_cast<int>((packed >> (kBitsPerCell * source)) & 0xFu);
      const int exponent_out = static_cast<int>((slid.row >> (kBitsPerCell * dest)) & 0xFu);

      TileMove move;
      move.from_row = from.row;
      move.from_col = from.col;
      move.to_row = to.row;
      move.to_col = to.col;
      // 被并入的那一块记的是**合并前**的值（前端要让它先滑过去再消失），
      // 其余记合并结果的指数。
      move.exponent = map.consumed[source] ? exponent_in : exponent_out;
      move.merged = map.consumed[source];
      moves->push_back(move);
    }
  }

  AI2048_DEBUG_CHECK((horizontal ? reconstructed : Transpose(reconstructed)) == after,
                     "方块轨迹与走子结果不一致：规则逻辑可能已经长出第二套实现");
}

}  // namespace

int ValueToExponent(std::uint64_t value) noexcept {
  if (value == 0) return 0;
  if ((value & (value - 1)) != 0) return -1;  // 不是 2 的幂
  int exponent = 0;
  while ((std::uint64_t{1} << exponent) < value) ++exponent;
  return exponent;
}

std::uint64_t EncodeBoard(const std::array<int, kCellCount>& exponents) noexcept {
  std::uint64_t board = 0;
  for (int i = 0; i < kCellCount; ++i) {
    board = SetExponent(board, i, exponents[static_cast<std::size_t>(i)]);
  }
  return board;
}

std::array<int, kCellCount> DecodeBoard(std::uint64_t board) noexcept {
  std::array<int, kCellCount> exponents{};
  for (int i = 0; i < kCellCount; ++i) {
    exponents[static_cast<std::size_t>(i)] = GetExponent(board, i);
  }
  return exponents;
}

const char* DirectionName(Direction direction) noexcept {
  switch (direction) {
    case Direction::kUp:
      return "up";
    case Direction::kDown:
      return "down";
    case Direction::kLeft:
      return "left";
    case Direction::kRight:
      return "right";
  }
  return "?";
}

MoveResult ApplyMove(std::uint64_t board, Direction direction) noexcept {
  const RowTables& tables = Tables();
  MoveResult result;
  result.board = board;

  const bool horizontal = direction == Direction::kLeft || direction == Direction::kRight;
  const std::uint64_t working = horizontal ? board : Transpose(board);

  std::uint64_t next = working;
  for (int row = 0; row < kBoardSize; ++row) {
    const PackedRow current = ExtractRow(working, row);
    const RowEntryValue entry = RowEntry(direction, current);
    result.score_gained += entry.gain;
    result.overflow = result.overflow || tables.overflow[current];
    next = ReplaceRow(next, row, entry.row);
  }

  result.board = horizontal ? next : Transpose(next);
  result.moved = result.board != board;

  if (result.moved) {
    AppendTrajectory(board, direction, result.board, &result.moves);
  } else {
    // 走不动就不算分，也不该报 overflow —— 上限处的合并如果没有真的发生，
    // 调用方不该看到这个标志。
    result.score_gained = 0;
    result.overflow = false;
  }

  return result;
}

bool HasLegalMove(std::uint64_t board) noexcept {
  if (CountEmptyCells(board) > 0) return true;

  const RowTables& tables = Tables();
  for (int row = 0; row < kBoardSize; ++row) {
    const PackedRow current = ExtractRow(board, row);
    if (tables.left[current] != current) return true;
    if (tables.right[current] != current) return true;
  }

  const std::uint64_t transposed = Transpose(board);
  for (int row = 0; row < kBoardSize; ++row) {
    const PackedRow current = ExtractRow(transposed, row);
    if (tables.left[current] != current) return true;
    if (tables.right[current] != current) return true;
  }

  return false;
}

std::optional<Direction> FindAnyLegalMove(std::uint64_t board) noexcept {
  constexpr std::array<Direction, 4> kOrder = {Direction::kLeft, Direction::kDown,
                                               Direction::kRight, Direction::kUp};
  for (const Direction direction : kOrder) {
    if (ApplyMove(board, direction).moved) return direction;
  }
  return std::nullopt;
}

int MaxExponent(std::uint64_t board) noexcept {
  int best = 0;
  for (int i = 0; i < kCellCount; ++i) {
    best = std::max(best, GetExponent(board, i));
  }
  return best;
}

int CountEmptyCells(std::uint64_t board) noexcept {
  int count = 0;
  for (int i = 0; i < kCellCount; ++i) {
    if (GetExponent(board, i) == 0) ++count;
  }
  return count;
}

std::string ToString(std::uint64_t board) noexcept {
  std::string out;
  for (int row = 0; row < kBoardSize; ++row) {
    if (row != 0) out += '\n';
    for (int col = 0; col < kBoardSize; ++col) {
      if (col != 0) out += ' ';
      const std::uint64_t value = ExponentToValue(GetExponent(board, row * kBoardSize + col));
      out += std::to_string(value);
    }
  }
  return out;
}

}  // namespace ai2048
