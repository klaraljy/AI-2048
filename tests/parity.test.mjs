// 前端规则引擎与 C++ 引擎的一致性对拍。
//
// 为什么必须有这个测试：
// 前端有一份降级用的本地规则引擎（web/js/game.js）。如果它与 C++ 引擎规则不一致，
// 那么"引擎在跑"和"引擎连不上"会得到不同的对局 —— 这比不能玩更糟，
// 因为它会静默地让分数、AI 决策、基准数据全部失去意义。
//
// 判据不是"看起来一样"，而是**逐字节比较**。分两层：
//
//   1. 穷举单行走子：全部 65536 种行状态 x 4 个方向。
//      这一层是主力 —— 整局对拍会漏掉罕见的方向/合并组合。
//      实测就漏掉过一个"下移把棋盘清空"的 bug（30 局全过，但四方向里有两个是错的）。
//   2. 整局对拍：固定策略跑完整局，比较终局状态逐字节一致。
//
// 运行方式（在项目根目录）：
//   node tests\parity.test.mjs
//
// 找不到引擎可执行文件时直接失败，不静默跳过 —— 跳过等于没有验证。

import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { Game, applyMove, SIZE } from '../web/js/game.js';

const here = dirname(fileURLToPath(import.meta.url));
const projectRoot = join(here, '..');
const cli = join(projectRoot, 'engine', 'build', 'ai2048-cli.exe');
const seedsFile = join(projectRoot, 'benchmarks', 'seeds-v1.txt');

const DIRECTIONS = ['left', 'down', 'right', 'up'];
const GAME_COUNT = 30;

if (!existsSync(cli)) {
  console.error(`找不到引擎可执行文件：${cli}`);
  console.error('请先构建： cmake --build engine\\build');
  process.exit(1);
}

let failures = 0;

function reportFailure(title, detail) {
  failures += 1;
  console.error(`\n✗ ${title}`);
  console.error(`  ${detail}`);
}

// ---------------------------------------------------------------------------
// 第 1 层：穷举单行走子
//
// 只把行放在第 0 行、其余行留空。这样一次走子只影响一行，
// 逐位比较即可覆盖所有合并情形（含 4 格全同、间隔相等、满格等边界）。
// ---------------------------------------------------------------------------

function exhaustiveRowCheck() {
  const states = 1 << 16;
  const lines = [];
  const expectations = [];

  for (let state = 0; state < states; state++) {
    const exponents = [0, 0, 0, 0];
    for (let i = 0; i < SIZE; i++) {
      exponents[i] = (state >> (4 * i)) & 0xf;
    }
    const board = [exponents.slice(), [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]];
    const flatIn = board.flat().join(',');

    for (const direction of DIRECTIONS) {
      lines.push(`${flatIn} ${direction}`);

      const result = applyMove(board, direction);
      const flatOut = result.board.flat().join(',');
      expectations.push(`${flatOut},${result.gained}`);
    }
  }

  const actual = execFileSync(cli, ['move', '--stdin'], {
    input: lines.join('\n') + '\n',
    encoding: 'utf8',
    maxBuffer: 256 * 1024 * 1024,
  })
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line.length > 0);

  if (actual.length !== expectations.length) {
    reportFailure(
      '穷举对拍输出条数不符',
      `引擎返回 ${actual.length} 条，期望 ${expectations.length} 条`
    );
    return;
  }

  let mismatches = 0;
  for (let i = 0; i < expectations.length; i++) {
    if (actual[i] !== expectations[i]) {
      mismatches += 1;
      if (mismatches <= 3) {
        const direction = DIRECTIONS[i % DIRECTIONS.length];
        const state = Math.floor(i / DIRECTIONS.length);
        reportFailure(
          `单行走子不一致：行状态 0x${state.toString(16).padStart(4, '0')}，方向 ${direction}`,
          `引擎=${actual[i]}  前端=${expectations[i]}`
        );
      }
    }
  }
  if (mismatches > 0) {
    console.error(`\n穷举对拍失败：${mismatches} / ${expectations.length} 条不一致`);
    process.exit(1);
  }
  console.log(`  ✓ 穷举单行走子：${expectations.length} 条（65536 行 x 4 方向）全部一致`);
}

// ---------------------------------------------------------------------------
// 第 2 层：整局对拍
//
// 走子策略必须与 CLI 的 RunTrace 完全相同：按固定顺序取第一个合法方向。
// 这里不能用 AI，否则对拍会掺进搜索实现的影响。
// ---------------------------------------------------------------------------

function playToEnd(seed) {
  const game = new Game(seed);
  for (;;) {
    let advanced = false;
    for (const direction of DIRECTIONS) {
      if (game.step(direction).moved) {
        advanced = true;
        break;
      }
    }
    if (!advanced) break;
  }
  return game;
}

function fullGameCheck() {
  const expected = execFileSync(
    cli,
    ['trace', '--seeds', seedsFile, '--limit', String(GAME_COUNT)],
    { encoding: 'utf8' }
  )
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line.length > 0);

  if (expected.length !== GAME_COUNT) {
    reportFailure('整局对拍输出条数不符', `引擎返回 ${expected.length} 局，期望 ${GAME_COUNT} 局`);
    return;
  }

  for (let i = 0; i < GAME_COUNT; i++) {
    const actual = playToEnd(i + 1).serialize().replace(/\n/g, '|');
    if (actual !== expected[i]) {
      reportFailure(`整局不一致：种子 ${i + 1}`, `引擎=${expected[i]}\n  前端=${actual}`);
      if (failures >= 3) return;
    }
  }
  if (failures === 0) {
    console.log(`  ✓ 整局对拍：${GAME_COUNT} 局逐字节一致`);
  }
}

// ---------------------------------------------------------------------------

console.log('前端规则引擎 vs C++ 引擎：');
exhaustiveRowCheck();
fullGameCheck();

if (failures > 0) {
  console.error(`\n一致性对拍失败（共 ${failures} 处）`);
  process.exit(1);
}
console.log('一致性对拍通过');
