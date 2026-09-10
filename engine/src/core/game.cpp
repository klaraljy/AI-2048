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

}  // namespace

Game::Game(std::uint64_t seed) noexcept : seed_(seed), rng_(seed) {
  for (int i = 0; i < kInitialTiles; ++i) {
    SpawnRandomTile();
  }
  // 开局不判定终局：两个方块不可能把 16 格堵死。
}

void Game::SpawnRandomTile() noexcept {
  int empty_count = 0;
  const std::array<int, kCellCount> empty_cells = CollectEmptyCells(board_, &empty_count);
  if (empty_count == 0) return;

  const int slot = static_cast<int>(rng_.NextBounded(static_cast<std::uint64_t>(empty_count)));
  const int index = empty_cells[static_cast<std::size_t>(slot)];

  // 先决定数值再落子。这里的消耗顺序（先位置后数值）是**规则的一部分**：
  // 改动它会改变所有历史种子集的结果，必须同步提升规则集版本。
  const int exponent = rng_.Chance(kFourSpawnNumerator, kSpawnDenominator) ? 2 : 1;
  board_ = SetExponent(board_, index, exponent);
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
  int empty_index = 0;
  const std::array<int, kCellCount> empty_cells = CollectEmptyCells(board_, &empty_index);
  // 走子成功意味着棋盘发生了变化，因此必然还有空格可放新块。
  assert(empty_index > 0 && "走子成功了却没有空格 —— ApplyMove 与终局判定已经不一致");

  const int slot = static_cast<int>(rng_.NextBounded(static_cast<std::uint64_t>(empty_index)));
  const int index = empty_cells[static_cast<std::size_t>(slot)];
  const int exponent = rng_.Chance(kFourSpawnNumerator, kSpawnDenominator) ? 2 : 1;
  board_ = SetExponent(board_, index, exponent);

  result.spawned = true;
  result.spawn.row = index / kBoardSize;
  result.spawn.col = index % kBoardSize;
  result.spawn.exponent = exponent;

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
