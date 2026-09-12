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
//   - 难度改变**位置分布**（见 spawnIndexFor），不改取值概率
//
// 一致性由 tests/parity.test.mjs 与 C++ 的 selfcheck 输出对拍验证。

import { Rng } from './rng.js';

export const SIZE = 4;
export const CELLS = SIZE * SIZE;

/**
 * 新方块是 4 的概率**随难度变化**（简单 10% / 中等 15% / 困难 20%），
 * 所以分母与阈值都定义在下面的难度一节里，这里只留开局方块数。
 *
 * ⚠️ 2026-09-12 的规则修订：推翻了先前"难度只改位置、不改取值概率"的决定。
 * 三档的期望生成值只差 2.2 / 2.3 / 2.4（差 9%），而归因问题已由
 * "分数不跨难度比较 + 跑批必须带难度标记"兜住。
 */
const INITIAL_TILES = 2;

// ---------------------------------------------------------------------------
// 难度：改变新方块出现的**位置分布**
//
// 参数必须与引擎的 core/game.h 完全一致，否则同一难度下前端与引擎会分叉。
//
//   easy   80% 按「安全分」加权 + 20% 全盘均匀
//   normal 全盘均匀（标准 2048）
//   hard   75% 按「不利分」加权 + 25% 全盘均匀
//
// 位置分布不再是"以 n% 概率精确跳到某个位置"，而是加权随机 ——
// 加权保留了尾部概率，所以困难档不会表现得像在针对玩家。
// ---------------------------------------------------------------------------
export const DIFFICULTY_EASY = 'easy';
export const DIFFICULTY_NORMAL = 'normal';
export const DIFFICULTY_HARD = 'hard';

/** 加权分支的占比（/1000），必须与引擎的 kEasyWeightedShare / kHardWeightedShare 一致。 */
const EASY_WEIGHTED_SHARE = 800;
const HARD_WEIGHTED_SHARE = 750;
/** 数值分母：出 4 的概率按难度取 100 / 150 / 200（即 10% / 15% / 20%）。 */
const SPAWN_VALUE_DENOMINATOR = 1000;
const FOUR_SPAWN_EASY = 100;
const FOUR_SPAWN_NORMAL = 150;
const FOUR_SPAWN_HARD = 200;
/** 评分参与指数前的饱和上限，必须与引擎的 kScoreSaturation 一致。 */
const SCORE_SATURATION = 600;

// ---------------------------------------------------------------------------
// 权重表 = round(exp(score/100 × 0.35) × 256)，score 取 0..600。
//
// ⚠️ **必须用查表，不能在 JS 里现算 Math.exp。**
// 本项目的核心承诺是"同种子逐字节一致"，而 exp() 的浮点结果不保证跨语言、
// 跨编译器逐位相同 —— 一旦差一个 ulp，取整后就可能差 1，加权抽样的落点
// 随即可变，整局对局分叉。
//
// 这张表是从引擎的 C++ 实现（core/game.cpp 的 WeightTable）**导出**的，
// 所以两边逐位相同。改引擎的 strength 或饱和上限时，必须重新导出本表：
//   tools/export-weight-table.ps1
// ---------------------------------------------------------------------------
const WEIGHT_TABLE = [
  256,257,258,259,260,261,261,262,263,264,265,266,267,268,269,270,
  271,272,273,274,275,276,276,277,278,279,280,281,282,283,284,285,
  286,287,288,289,290,291,292,293,294,296,297,298,299,300,301,302,
  303,304,305,306,307,308,309,310,311,313,314,315,316,317,318,319,
  320,321,323,324,325,326,327,328,329,331,332,333,334,335,336,338,
  339,340,341,342,343,345,346,347,348,350,351,352,353,354,356,357,
  358,359,361,362,363,365,366,367,368,370,371,372,374,375,376,378,
  379,380,382,383,384,386,387,388,390,391,392,394,395,397,398,399,
  401,402,404,405,406,408,409,411,412,414,415,416,418,419,421,422,
  424,425,427,428,430,431,433,434,436,437,439,440,442,443,445,447,
  448,450,451,453,454,456,458,459,461,463,464,466,467,469,471,472,
  474,476,477,479,481,482,484,486,487,489,491,493,494,496,498,500,
  501,503,505,507,508,510,512,514,516,517,519,521,523,525,526,528,
  530,532,534,536,538,540,541,543,545,547,549,551,553,555,557,559,
  561,563,565,567,569,571,573,575,577,579,581,583,585,587,589,591,
  593,595,597,599,601,603,606,608,610,612,614,616,618,621,623,625,
  627,629,632,634,636,638,640,643,645,647,649,652,654,656,659,661,
  663,666,668,670,673,675,677,680,682,684,687,689,692,694,697,699,
  701,704,706,709,711,714,716,719,721,724,726,729,732,734,737,739,
  742,744,747,750,752,755,758,760,763,766,768,771,774,776,779,782,
  785,787,790,793,796,798,801,804,807,810,813,815,818,821,824,827,
  830,833,836,839,841,844,847,850,853,856,859,862,865,868,871,875,
  878,881,884,887,890,893,896,899,903,906,909,912,915,918,922,925,
  928,931,935,938,941,945,948,951,954,958,961,965,968,971,975,978,
  982,985,988,992,995,999,1002,1006,1009,1013,1017,1020,1024,1027,1031,1035,
  1038,1042,1045,1049,1053,1056,1060,1064,1068,1071,1075,1079,1083,1086,1090,1094,
  1098,1102,1106,1110,1113,1117,1121,1125,1129,1133,1137,1141,1145,1149,1153,1157,
  1161,1165,1169,1173,1178,1182,1186,1190,1194,1198,1203,1207,1211,1215,1219,1224,
  1228,1232,1237,1241,1245,1250,1254,1259,1263,1267,1272,1276,1281,1285,1290,1294,
  1299,1303,1308,1312,1317,1322,1326,1331,1336,1340,1345,1350,1354,1359,1364,1369,
  1374,1378,1383,1388,1393,1398,1403,1408,1413,1418,1423,1427,1433,1438,1443,1448,
  1453,1458,1463,1468,1473,1478,1484,1489,1494,1499,1504,1510,1515,1520,1526,1531,
  1536,1542,1547,1553,1558,1563,1569,1574,1580,1586,1591,1597,1602,1608,1614,1619,
  1625,1631,1636,1642,1648,1654,1659,1665,1671,1677,1683,1689,1695,1700,1706,1712,
  1718,1724,1731,1737,1743,1749,1755,1761,1767,1773,1780,1786,1792,1798,1805,1811,
  1817,1824,1830,1837,1843,1850,1856,1863,1869,1876,1882,1889,1895,1902,1909,1915,
  1922,1929,1936,1942,1949,1956,1963,1970,1977,1984,1991,1998,2005,2012,2019,2026,
  2033,2040,2047,2054,2061,2069,2076,2083,2091
];


function isCorner(row, col) {
  return (
    (row === 0 || row === SIZE - 1) && (col === 0 || col === SIZE - 1)
  );
}

/** 是否在边缘（含角落）。困难档会**降低**边缘位置的权重 —— 角落对玩家有利。 */
function isEdge(row, col) {
  return row === 0 || row === SIZE - 1 || col === 0 || col === SIZE - 1;
}

/** 邻居偏移，顺序固定为「上、下、左、右」—— 顺序决定评分的确定性。 */
const NEIGHBOUR_OFFSETS = [
  [-1, 0],
  [1, 0],
  [0, -1],
  [0, 1],
];

/**
 * 贴着一个多大的块？越大越"不利"（块周围被堵住的价值更高）。
 * 分档与引擎的 HighValuePenalty 一致：指数 5=32、7=128、9=512。
 */
function highValuePenalty(exponent) {
  if (exponent >= 9) return 200;
  if (exponent >= 7) return 140;
  if (exponent >= 5) return 80;
  if (exponent >= 3) return 40;
  return 0;
}

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
  /**
   * @param {number|bigint|string} seed
   * @param {string} [difficulty] 'easy' | 'normal' | 'hard'，见 spawnIndexFor。
   */
  constructor(seed, difficulty = DIFFICULTY_NORMAL) {
    this.seed = seed;
    this.difficulty = difficulty;
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

  /**
   * 生成一个新方块。
   *
   * **顺序（先位置、后取值）是规则的一部分**，随机数的消耗次数也属于规则 ——
   * 任何改动都会让所有历史种子集的结果作废，必须与引擎（core/game.cpp 的
   * SpawnRandomTile）逐行一致，否则前端降级运行时玩的会是另一个游戏。
   */
  _spawn() {
    const empties = [];
    for (let r = 0; r < SIZE; r++) {
      for (let c = 0; c < SIZE; c++) {
        if (this.board[r][c] === 0) empties.push([r, c]);
      }
    }
    if (empties.length === 0) return null;

    // 位置选择。**恰好消耗两次**随机数，与难度和分支无关（详见 _spawnIndexFor）。
    const chosen = this._spawnIndexFor(empties);
    const [row, col] = chosen;

    // 数值：出 4 的概率随难度变化（简单 10% / 中等 15% / 困难 20%）。
    // 与引擎一致：都是整数比较（分母 1000），不引入浮点。
    let fourThreshold = FOUR_SPAWN_NORMAL;
    if (this.difficulty === DIFFICULTY_EASY) fourThreshold = FOUR_SPAWN_EASY;
    else if (this.difficulty === DIFFICULTY_HARD) fourThreshold = FOUR_SPAWN_HARD;
    const exponent =
      this._rng.nextBounded(SPAWN_VALUE_DENOMINATOR) < fourThreshold ? 2 : 1;

    this.board[row][col] = exponent;
    return { row, col, exponent };
  }

  /**
   * 某个空格在简单档下的「安全分」（百分数整数，越大越安全）。
   *
   * 逐项对应文档《方块生成策略》的评分表：
   *   边缘 +150 / 角落 +250 / 每个空邻居 +80
   *   每个已有邻居 −40，其中指数 1（值 2）再 +135、指数 2（值 4）再 +15
   *   指数 ≥6（值 64 及以上）再 −30
   *
   * 最后取 max(score, 0)：负分位置不该被"负权"排挤。
   */
  _safeScore(r, c) {
    let score = 0;
    if (isEdge(r, c)) score += 150;
    if (isCorner(r, c)) score += 250;

    for (const [dr, dc] of NEIGHBOUR_OFFSETS) {
      const nr = r + dr;
      const nc = c + dc;
      if (nr < 0 || nr >= SIZE || nc < 0 || nc >= SIZE) continue;
      const exponent = this.board[nr][nc];
      if (exponent === 0) {
        score += 80;
      } else {
        score -= 40;
        // 简单档 90% 出 2（指数 1）、10% 出 4（指数 2）：按期望值加权
        if (exponent === 1) score += 135;
        if (exponent === 2) score += 15;
        if (exponent >= 6) score -= 30;
      }
    }
    return score < 0 ? 0 : score;
  }

  /** 某个空格在困难档下的「不利分」（百分数整数，越大越不利）。 */
  _hostileScore(r, c) {
    let score = 0;

    for (const [dr, dc] of NEIGHBOUR_OFFSETS) {
      const nr = r + dr;
      const nc = c + dc;
      if (nr < 0 || nr >= SIZE || nc < 0 || nc >= SIZE) continue;
      const exponent = this.board[nr][nc];
      if (exponent === 0) continue; // 空邻居不加分：拥挤度由非空邻居那边算

      score += 120; // 拥挤度
      score += highValuePenalty(exponent); // 贴高价值块
      // 可立即合并的位置对玩家有利 → 降低"不利分"。
      // ⚠️ 系数按困难档 80%/20% 的取值概率加权（与引擎一致）。
      if (exponent === 1) score -= 96;
      if (exponent === 2) score -= 24;
    }

    // 断裂点：空位夹在两个**不同**数字之间，落子会打断排列。
    // 横竖两对，各自判断"两侧都有块且数值不同"。
    for (const [dr, dc] of [
      [1, 0],
      [0, 1],
    ]) {
      const r1 = r - dr;
      const c1 = c - dc;
      const r2 = r + dr;
      const c2 = c + dc;
      if (r1 < 0 || r1 >= SIZE || c1 < 0 || c1 >= SIZE) continue;
      if (r2 < 0 || r2 >= SIZE || c2 < 0 || c2 >= SIZE) continue;
      const a = this.board[r1][c1];
      const b = this.board[r2][c2];
      if (a !== 0 && b !== 0 && a !== b) score += 150;
      if (a !== 0 && b !== 0) {
        const hi = Math.max(a, b);
        const lo = Math.min(a, b);
        if (hi >= lo + 2) score += 150; // 差距 ≥ 4 倍（指数差 2）
      }
    }

    // 困难档刻意**不**偏向角落：角落对玩家有利，一直往角上放会让"角落策略"过强。
    if (isCorner(r, c)) score -= 80;
    if (isEdge(r, c)) score -= 30;

    return score < 0 ? 0 : score;
  }

  /**
   * 按难度挑一个落点。返回 [row, col]。
   *
   * 必须与引擎的 Game::SpawnRandomTile 保持一致 —— **包括 rng 的调用顺序与次数**：
   *   第 1 次 nextBounded(1000) —— 决定走「加权」还是「纯随机」
   *   第 2 次 nextBounded(1000) —— 在选定的分布里挑格子
   *   第 3 次（在调用方）—— 决定数值是 2 还是 4
   *
   * ⚠️ 消耗次数**恒定三次**，与难度、盘面、分支都无关。
   * 旧实现用 chance() 决定要不要走偏置、命中后再抽一次选格子，于是
   * "命中"与"未命中"消耗的随机数个数不同，同一种子在不同难度下后续随机流分叉，
   * 跨难度对比时说不清差异来自哪里。
   */
  _spawnIndexFor(empties) {
    const branchRoll = this._rng.nextBounded(SPAWN_VALUE_DENOMINATOR);
    const cellRoll = this._rng.nextBounded(SPAWN_VALUE_DENOMINATOR);

    let weightedShare = 0;
    if (this.difficulty === DIFFICULTY_EASY) weightedShare = EASY_WEIGHTED_SHARE;
    else if (this.difficulty === DIFFICULTY_HARD) weightedShare = HARD_WEIGHTED_SHARE;

    // 加权分支：算出每个空位的权重，再让第 2 次随机数按权重区间落点。
    if (weightedShare > 0 && branchRoll < weightedShare) {
      const useHard = this.difficulty === DIFFICULTY_HARD;
      const weights = [];
      let total = 0;
      for (const [r, c] of empties) {
        const score = useHard ? this._hostileScore(r, c) : this._safeScore(r, c);
        const w = WEIGHT_TABLE[score > SCORE_SATURATION ? SCORE_SATURATION : score];
        weights.push(w);
        total += w;
      }
      if (total > 0) {
        const pick = Math.floor((cellRoll * total) / SPAWN_VALUE_DENOMINATOR);
        let acc = 0;
        for (let i = 0; i < empties.length; i++) {
          acc += weights[i];
          if (pick < acc) return empties[i];
        }
        return empties[empties.length - 1]; // 取整边界：落到最后一个
      }
    }

    // 纯随机分支：简单/困难档的兜底，也是 normal 唯一走的分支。
    // 没有它，简单档会过于温和、困难档会显得在作弊。
    return empties[cellRoll % empties.length];
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
