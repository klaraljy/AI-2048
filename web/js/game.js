// 前端本地规则引擎。
//
// **必须与 C++ 引擎（engine/src/core/board.cpp + game.cpp）规则完全一致。**
// 这不是"另写一套" —— 它是引擎连不上时的降级实现，两者跑同一个种子必须得到
// 同一局。为此：
//   - 合并规则：先压实、再合并；单次移动内合并出的块不参与二次合并
//     （[2,0,2,0] 右移必须得到 [0,0,0,4]，不是 [0,0,4,0]）
//   - 生成顺序：先决定位置、再决定取值（这个顺序是规则的一部分）
//   - 90% 出 2、10% 出 4；新块落在**随机空格**
//   - 非法走子**不消耗随机数**（否则 replay 失效）
//
// 一致性由 tests/parity.test.mjs 与 C++ 的 selfcheck 输出对拍验证。

import { Rng } from './rng.js';

export const SIZE = 4;
export const CELLS = SIZE * SIZE;

/** 新方块是 4 的概率（10%）。这是**规则**，不是难度。 */
const FOUR_NUMERATOR = 1;
const SPAWN_DENOMINATOR = 10;
const INITIAL_TILES = 2;

export const DIRECTION = {
  up: 'up',
  down: 'down',
  left: 'left',
  right: 'right',
};

/**
 * 把 4 个格（0 = 空，否则是指数）往索引 0 方向压实并合并。
 * @param {number[]} line 长度 4 的指数数组
 * @returns {{line: number[], gained: number, slots: Array<{to: number, from: number[], merged: boolean}>}}
 *   slots 按"结果位"给出：每个结果位由哪些来源位合并而来。
 */
function slideLine(line) {
  // 步骤 1：压实，记录每张牌的原位置
  const values = [];
  const origins = [];
  for (let i = 0; i < SIZE; i++) {
    if (line[i] !== 0) {
      values.push(line[i]);
      origins.push(i);
    }
  }

  // 步骤 2：在压实后的序列上合并相邻等值对
  const outLine = [0, 0, 0, 0];
  const slots = [];
  let gained = 0;
  let out = 0;
  let i = 0;
  while (i < values.length) {
    const exponent = values[i];
    const canMerge = i + 1 < values.length && values[i + 1] === exponent;

    let mergedExponent = exponent;
    let overflow = false;
    if (canMerge) {
      if (exponent >= 15) {
        // 已经到上限，合并无法进位。显式报告而不是静默 clamp
        // （参考原型就是静默 clamp 的，那会让 AI 模拟的规则与现实规则错位）。
        mergedExponent = 15;
        overflow = true;
      } else {
        mergedExponent = exponent + 1;
        gained += 2 ** mergedExponent;
      }
    }

    outLine[out] = mergedExponent;
    slots.push({
      to: out,
      from: canMerge ? [origins[i], origins[i + 1]] : [origins[i]],
      merged: canMerge,
      overflow,
    });

    out += 1;
    i += canMerge ? 2 : 1;
  }

  return { line: outLine, gained, slots, overflow: slots.some((s) => s.overflow) };
}

/** 一个方向的"处理顺序"：第 order 位对应物理上的哪一行/列。 */
function slotFor(direction, line, order) {
  switch (direction) {
    case DIRECTION.left:
      return { row: line, col: order };
    case DIRECTION.right:
      return { row: line, col: SIZE - 1 - order };
    case DIRECTION.up:
      return { row: order, col: line };
    case DIRECTION.down:
      return { row: SIZE - 1 - order, col: line };
    default:
      return { row: line, col: order };
  }
}

/**
 * 对指数棋盘执行一次走子。不生成新方块、不改分数 —— 那是 Game 的职责。
 *
 * 关键：`slotFor(direction, line, order)` 已经返回**按处理顺序**的物理坐标
 * （比如 right 的第 0 位就是最右列）。所以这里只需要：
 *   取线 → 压实合并（向 order=0 那端推）→ 写回 → 换算坐标
 * **不要再对 line 反排** —— 那会与 slotFor 的方向映射抵消，让四个方向变成同一个。
 *
 * @param {number[][]} board 4x4 指数
 * @param {string} direction
 * @returns {{board: number[][], moved: boolean, gained: number, moves: Array, overflow: boolean}}
 *   moves 是方块轨迹（已换算回棋盘坐标）：
 *   {fromRow, fromCol, toRow, toCol, exponent, merged}
 *   合并时两条记录落在同一目标格，merged=true 的那条是**被吸收**的牌。
 */
export function applyMove(board, direction) {
  const next = board.map((row) => row.slice());
  const moves = [];
  let gained = 0;
  let overflow = false;

  for (let line = 0; line < SIZE; line++) {
    // 按处理顺序取线
    const packed = [0, 0, 0, 0];
    for (let order = 0; order < SIZE; order++) {
      const slot = slotFor(direction, line, order);
      packed[order] = board[slot.row][slot.col];
    }

    const result = slideLine(packed);
    gained += result.gained;
    overflow = overflow || result.overflow;

    for (const slot of result.slots) {
      // slot.to / slot.from 已经是 packed（= 处理顺序）里的下标，
      // 而 slotFor 正是按处理顺序取坐标，所以直接传即可。
      const dest = slotFor(direction, line, slot.to);
      const exponentOut = result.line[slot.to];

      const primary = slotFor(direction, line, slot.from[0]);
      moves.push({
        fromRow: primary.row,
        fromCol: primary.col,
        toRow: dest.row,
        toCol: dest.col,
        exponent: exponentOut,
        merged: false,
      });

      // 被吸收的那张：滑到同一格然后消失，记**合并前**的值
      if (slot.merged) {
        const absorbedIndex = slot.from[1];
        const absorbed = slotFor(direction, line, absorbedIndex);
        moves.push({
          fromRow: absorbed.row,
          fromCol: absorbed.col,
          toRow: dest.row,
          toCol: dest.col,
          exponent: packed[absorbedIndex],
          merged: true,
        });
      }
    }

    // 写回棋盘
    for (let order = 0; order < SIZE; order++) {
      const slot = slotFor(direction, line, order);
      next[slot.row][slot.col] = result.line[order];
    }
  }

  // 判断是否真的动了
  let moved = false;
  for (let r = 0; r < SIZE && !moved; r++) {
    for (let c = 0; c < SIZE; c++) {
      if (next[r][c] !== board[r][c]) {
        moved = true;
        break;
      }
    }
  }

  if (!moved) {
    // 走不动就不算分，也不该报 overflow
    return { board, moved: false, gained: 0, moves: [], overflow: false };
  }
  return { board: next, moved: true, gained, moves, overflow };
}

/** 是否还存在合法走子。 */
export function hasLegalMove(board) {
  for (let r = 0; r < SIZE; r++) {
    for (let c = 0; c < SIZE; c++) {
      if (board[r][c] === 0) return true;
      if (c + 1 < SIZE && board[r][c] === board[r][c + 1]) return true;
      if (r + 1 < SIZE && board[r][c] === board[r + 1][c]) return true;
    }
  }
  return false;
}

export function countEmptyCells(board) {
  let n = 0;
  for (let r = 0; r < SIZE; r++) {
    for (let c = 0; c < SIZE; c++) {
      if (board[r][c] === 0) n += 1;
    }
  }
  return n;
}

export function maxTile(board) {
  let best = 0;
  for (let r = 0; r < SIZE; r++) {
    for (let c = 0; c < SIZE; c++) {
      best = Math.max(best, board[r][c]);
    }
  }
  return best === 0 ? 0 : 2 ** best;
}

function emptyBoard() {
  return Array.from({ length: SIZE }, () => new Array(SIZE).fill(0));
}

/** 一局 2048。确定性：同种子 + 同一串方向 → 完全一致的结果。 */
export class Game {
  /** @param {number|bigint|string} seed */
  constructor(seed) {
    this.seed = seed;
    this._rng = new Rng(seed);
    this.board = emptyBoard();
    this.score = 0;
    this.stepCount = 0;
    this.gameOver = false;
    this.reached2048 = false;
    this.sawOverflow = false;
    this.lastMove = null;
    this._history = [];

    for (let i = 0; i < INITIAL_TILES; i++) this._spawn();
  }

  /** 生成一个新方块。顺序（先位置、后取值）是规则的一部分，不要改。 */
  _spawn() {
    const empties = [];
    for (let r = 0; r < SIZE; r++) {
      for (let c = 0; c < SIZE; c++) {
        if (this.board[r][c] === 0) empties.push([r, c]);
      }
    }
    if (empties.length === 0) return null;

    const [row, col] = empties[this._rng.nextBounded(empties.length)];
    const exponent = this._rng.chance(FOUR_NUMERATOR, SPAWN_DENOMINATOR) ? 2 : 1;
    this.board[row][col] = exponent;
    return { row, col, exponent };
  }

  /**
   * 走一步。
   * @returns {{moved: boolean, spawned: boolean, spawn: object|null, moves: Array, gained: number, overflow: boolean}}
   *   moved=false 时**没有任何状态变化，也不消耗随机数**。
   */
  step(direction) {
    if (this.gameOver) {
      return { moved: false, spawned: false, spawn: null, moves: [], gained: 0, overflow: false };
    }

    const result = applyMove(this.board, direction);
    if (!result.moved) {
      return { moved: false, spawned: false, spawn: null, moves: [], gained: 0, overflow: false };
    }

    // 撤销点：走子**之前**的状态
    this._history.push({
      board: this.board.map((row) => row.slice()),
      score: this.score,
      stepCount: this.stepCount,
      reached2048: this.reached2048,
      lastMove: this.lastMove,
    });
    if (this._history.length > 200) this._history.shift();

    this.board = result.board;
    this.score += result.gained;
    this.stepCount += 1;
    this.lastMove = direction;
    this.sawOverflow = this.sawOverflow || result.overflow;

    const spawn = this._spawn();
    if (maxTile(this.board) >= 2048) this.reached2048 = true;
    this.gameOver = !hasLegalMove(this.board);

    return {
      moved: true,
      spawned: spawn !== null,
      spawn,
      moves: result.moves,
      gained: result.gained,
      overflow: result.overflow,
    };
  }

  /** 撤销一步。返回是否成功。 */
  undo() {
    const previous = this._history.pop();
    if (!previous) return false;
    this.board = previous.board;
    this.score = previous.score;
    this.stepCount = previous.stepCount;
    this.reached2048 = previous.reached2048;
    this.lastMove = previous.lastMove;
    this.gameOver = false;
    return true;
  }

  get canUndo() {
    return this._history.length > 0;
  }

  /** 规范化的状态字符串。用于与 C++ 版 selfcheck 输出逐字节对拍。 */
  serialize() {
    const rows = [];
    for (let r = 0; r < SIZE; r++) {
      rows.push(
        board_values(this.board)[r]
          .map((v) => String(v))
          .join(' ')
      );
    }
    return `seed=${this.seed} steps=${this.stepCount} score=${this.score} max=${maxTile(
      this.board
    )} over=${this.gameOver ? 1 : 0} overflow=${this.sawOverflow ? 1 : 0} board=${rows.join('\n')}`;
  }
}

/** 指数棋盘 → 数值棋盘。 */
export function board_values(board) {
  return board.map((row) => row.map((v) => (v === 0 ? 0 : 2 ** v)));
}
