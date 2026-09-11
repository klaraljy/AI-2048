#include "core/game.h"

#include <array>
#include <cassert>

namespace ai2048 {

namespace {

// 收集空格的下标（0..15，行优先）。顺序固定，保证"第 k 个空格"是确定的。
[[nodiscard]] std::array<int, kCellCount> CollectEmptyCells(std::uint64_t board,
                                                            int* count) noexcept {
  std::array<int, kCellCount> cells{};
  int n = 0;
  for (int index = 0; index < kCellCount; ++index) {
    if (GetExponent(board, index) == 0) {
      cells[static_cast<std::size_t>(n)] = index;
      ++n;
    }
  }
  *count = n;
  return cells;
}

/** 四角的下标，顺序固定（左上、右上、左下、右下）。 */
inline constexpr std::array<int, 4> kCornerIndices = {0, 3, 12, 15};

/** 收集**空的**角落下标。顺序与 kCornerIndices 一致，保证可复现。 */
[[nodiscard]] std::array<int, 4> CollectEmptyCorners(std::uint64_t board, int* count) noexcept {
  std::array<int, 4> corners{};
  int n = 0;
  for (const int index : kCornerIndices) {
    if (GetExponent(board, index) == 0) {
      corners[static_cast<std::size_t>(n)] = index;
      ++n;
    }
  }
  *count = n;
  return corners;
}

/**
 * 收集"与**当前有空位的最大方块**相邻"的空格下标。
 *
 * 规则按用户明确指示演进：原先只找**最大块**，它旁边没空位时偏置就直接关闭。
 * 但那样在残局几乎失效 —— 最大块往往被围死，而盘面上还有其它大块旁边有空位，
 * 那正是最该放新块的地方。
 *
 * 现在改为：**按等级从高到低**找第一个"四周有空位"的方块，在它的相邻空格里选。
 * 例如 2048 被围死、但 128 旁边有空，就放在 128 旁边。
 *
 * 多个同值方块时取**行优先第一个**（规则必须确定，否则同种子不可复现）。
 * 邻居顺序固定为「上、下、左、右」—— 它决定"取第 k 个"的结果。
 */
[[nodiscard]] std::array<int, kCellCount> CollectEmptyNextToLargestMovable(
    std::uint64_t board, int* count) noexcept {
  std::array<int, kCellCount> cells{};
  *count = 0;

  constexpr std::array<std::array<int, 2>, 4> kOffsets = {{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};

  // 棋盘最多 16 格，等级最多 kMaxExponent，直接逐级扫描即可，不需要额外数据结构。
  for (int exponent = MaxExponent(board); exponent >= 1; --exponent) {
    int row = -1;
    int col = -1;
    for (int index = 0; index < kCellCount; ++index) {
      if (GetExponent(board, index) == exponent) {
        row = index / kBoardSize;
        col = index % kBoardSize;
        break;  // 行优先第一个
      }
    }
    if (row < 0) continue;  // 盘面上没有这个等级

    int n = 0;
    for (const auto& offset : kOffsets) {
      const int r = row + offset[0];
      const int c = col + offset[1];
      if (r < 0 || r >= kBoardSize || c < 0 || c >= kBoardSize) continue;
      const int index = r * kBoardSize + c;
      if (GetExponent(board, index) == 0) {
        cells[static_cast<std::size_t>(n)] = index;
        ++n;
      }
    }
    if (n > 0) {
      *count = n;  // 这个等级旁边有空位 —— 就是它
      return cells;
    }
    // 否则继续往下一个等级找
  }

  return cells;
}

}  // namespace

const char* DifficultyName(Difficulty difficulty) noexcept {
  switch (difficulty) {
    case Difficulty::kEasy:
      return "easy";
    case Difficulty::kHard:
      return "hard";
    case Difficulty::kNormal:
    default:
      return "normal";
  }
}

Game::Game(std::uint64_t seed, Difficulty difficulty) noexcept
    : seed_(seed), rng_(seed), difficulty_(difficulty) {
  for (int i = 0; i < kInitialTiles; ++i) {
    static_cast<void>(SpawnRandomTile());
  }
  // 开局不判定终局：两个方块不可能把 16 格堵死。
}

void Game::SetBoardForTesting(std::uint64_t board) noexcept {
  board_ = board;
  score_ = 0;
  step_count_ = 0;
  game_over_ = false;
  reached_2048_ = false;
  saw_overflow_ = false;
}

SpawnRecord Game::SpawnRandomTile() noexcept {
  SpawnRecord record;
  int empty_count = 0;
  const std::array<int, kCellCount> empty_cells = CollectEmptyCells(board_, &empty_count);
  if (empty_count == 0) return record;  // exponent 保持 0，调用方据此判断"没生成"

  // 位置选择的随机数**恰好消耗一次**，无论走哪条分支。
  //
  // 这是刻意的：如果"偏向分支"少消耗或多消耗一次随机数，那么同一档难度下
  // 一次生成就会改变后续整局的随机流，不同分支之间再也没法比较。
  // 参考实现也是这个结构（game.js:160-164），两边保持一致才能对拍。
  int index = -1;
  if (difficulty_ == Difficulty::kEasy) {
    int corner_count = 0;
    const std::array<int, 4> corners = CollectEmptyCorners(board_, &corner_count);
    if (corner_count > 0 && rng_.Chance(kEasyCornerNumerator, kDifficultyDenominator)) {
      const int slot = static_cast<int>(rng_.NextBounded(static_cast<std::uint64_t>(corner_count)));
      index = corners[static_cast<std::size_t>(slot)];
    }
  } else if (difficulty_ == Difficulty::kHard) {
    int near_count = 0;
    const std::array<int, kCellCount> near_cells =
        CollectEmptyNextToLargestMovable(board_, &near_count);
    if (near_count > 0 && rng_.Chance(kHardNearMaxNumerator, kDifficultyDenominator)) {
      const int slot = static_cast<int>(rng_.NextBounded(static_cast<std::uint64_t>(near_count)));
      index = near_cells[static_cast<std::size_t>(slot)];
    }
  }

  if (index < 0) {
    // 全盘均匀：标准 2048，也是 kNormal 唯一走的分支，
    // 以及另两档"偏置没触发"或"没有可用偏置位置"时的退路。
    const int slot = static_cast<int>(rng_.NextBounded(static_cast<std::uint64_t>(empty_count)));
    index = empty_cells[static_cast<std::size_t>(slot)];
  }

  // 先决定数值再落子。这里的消耗顺序（先位置后数值）是**规则的一部分**：
  // 改动它会改变所有历史种子集的结果，必须同步提升规则集版本。
  const int exponent = rng_.Chance(kFourSpawnNumerator, kSpawnDenominator) ? 2 : 1;
  board_ = SetExponent(board_, index, exponent);

  record.row = index / kBoardSize;
  record.col = index % kBoardSize;
  record.exponent = exponent;
  return record;
}

StepResult Game::Step(Direction direction) noexcept {
  StepResult result;
  if (game_over_) return result;

  const MoveResult move = ApplyMove(board_, direction);
  if (!move.moved) return result;  // 不消耗随机数

  board_ = move.board;
  result.moved = true;
  result.moves = std::move(move.moves);
  result.overflow = move.overflow;
  result.score_gained = move.score_gained;

  score_ += move.score_gained;
  ++step_count_;
  saw_overflow_ = saw_overflow_ || move.overflow;

  // 规则顺序：先合并、再生成、最后判终局。
  //
  // **必须调用同一个 SpawnRandomTile**，不能在这里再写一份位置选择 ——
  // 之前就是两处各写一份，改难度时只改了一处，走子后的生成会退回均匀分布，
  // 难度只在开局生效。这种"两份实现悄悄分叉"是本项目反复吃亏的地方。
  const SpawnRecord spawn = SpawnRandomTile();
  if (spawn.exponent == 0) return result;  // 无空格（理论上不该发生）

  result.spawned = true;
  result.spawn = spawn;

  if (MaxExponent(board_) >= 11) {  // 2^11 = 2048
    reached_2048_ = true;
  }

  game_over_ = !HasLegalMove(board_);
  return result;
}

std::string Game::Serialize() const noexcept {
  std::string out;
  out.reserve(160);
  out += "seed=";
  out += std::to_string(seed_);
  out += " steps=";
  out += std::to_string(step_count_);
  out += " score=";
  out += std::to_string(score_);
  out += " max=";
  out += std::to_string(max_tile());
  out += " over=";
  out += game_over_ ? '1' : '0';
  out += " overflow=";
  out += saw_overflow_ ? '1' : '0';
  out += " board=";
  out += ToString(board_);
  return out;
}

}  // namespace ai2048
