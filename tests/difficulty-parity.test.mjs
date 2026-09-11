// 三档难度的**前端 ↔ 引擎**逐位对拍。
//
// 为什么必须单独验：难度改变的是"新方块落在哪"，而这件事**只有前端降级运行时
// 才由 JS 决定**。两边一旦不一致，同一个难度下：
//   - 引擎在跑时，玩家玩的是引擎的落点规则
//   - 引擎连不上时，玩家玩的是 JS 的落点规则
// 同一档难度变成两个游戏，而分数会被拿来互相比较。
//
// 对拍方式：用同一串固定的方向序列（左、下、右、上，取第一个能走通的）跑完整局，
// 三档各自把最终状态与引擎的 `trace --difficulty <档>` 输出逐字节比较。
//
// 边界情况必须覆盖，它们最容易两边写法不同：
//   - 角落已被占满（easy 的偏置无处可落）
//   - 最大块四邻全被占（hard 的偏置无处可落）
//   - 多个最大方块（规则只认行优先第一个）
// 这些由"跑到终局"自然覆盖：一盘棋里各种状态都会出现。

import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { Game, board_values, maxTile } from '../web/js/game.js';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const CLI = join(ROOT, 'engine', 'build', 'ai2048-cli.exe');
const SEEDS = join(ROOT, 'benchmarks', 'seeds-v1.txt');

let failures = 0;
function check(name, ok, detail) {
  console.log(`  ${ok ? '✓' : '✗'} ${name}${detail ? ` —— ${detail}` : ''}`);
  if (!ok) failures++;
}

/** 与引擎的 trace 完全一致：左、下、右、上，取第一个能走通的。 */
const MOVE_ORDER = ['left', 'down', 'right', 'up'];

/** 跑完整局并产出与引擎同格式的一行状态。 */
function playFullGame(seed, difficulty) {
  const game = new Game(seed, difficulty);
  while (!game.gameOver) {
    let advanced = false;
    for (const direction of MOVE_ORDER) {
      if (game.step(direction).moved) {
        advanced = true;
        break;
      }
    }
    if (!advanced) break;
  }
  // 与 Game#serialize 同构，但压成一行便于逐行对拍
  const rows = [];
  for (let r = 0; r < 4; r++) {
    rows.push(board_values(game.board)[r].map(String).join(' '));
  }
  return (
    `seed=${game.seed} steps=${game.stepCount} score=${game.score} ` +
    `max=${maxTile(game.board)} over=${game.gameOver ? 1 : 0} ` +
    `overflow=${game.sawOverflow ? 1 : 0} board=${rows.join('|')}`
  );
}

/** 取引擎的 trace 输出（每个种子一行）。 */
function engineTrace(difficulty, limit) {
  const result = spawnSync(
    CLI,
    ['trace', '--seeds', SEEDS, '--limit', String(limit), '--difficulty', difficulty],
    { encoding: 'utf8' }
  );
  if (result.status !== 0) {
    throw new Error(`trace 失败（${difficulty}）：${result.stderr}`);
  }
  return result.stdout.split(/\r?\n/).filter((line) => line.length > 0);
}

const DIFFICULTIES = ['normal', 'easy', 'hard'];
const LIMIT = 25;

function main() {
  if (!existsSync(CLI)) {
    console.error(`找不到 ${CLI}\n请先构建：cmake --build engine\\build`);
    process.exit(1);
  }

  const seeds = [];
  for (let i = 1; i <= LIMIT; i++) seeds.push(i);

  for (const difficulty of DIFFICULTIES) {
    console.log(`\n[${difficulty}] 前端 vs 引擎，${LIMIT} 局逐字节对拍`);
    const expected = engineTrace(difficulty, LIMIT);
    check(
      `引擎输出了 ${LIMIT} 行`,
      expected.length === LIMIT,
      `实际 ${expected.length} 行`
    );

    let mismatches = 0;
    let firstMismatch = '';
    for (let i = 0; i < Math.min(seeds.length, expected.length); i++) {
      const actual = playFullGame(seeds[i], difficulty);
      if (actual !== expected[i]) {
        mismatches++;
        if (firstMismatch === '') {
          firstMismatch = `种子 ${seeds[i]}\n      引擎: ${expected[i]}\n      前端: ${actual}`;
        }
      }
    }
    check(
      `${LIMIT} 局全部逐字节一致`,
      mismatches === 0,
      mismatches > 0 ? `${mismatches} 局不一致。首例：\n      ${firstMismatch}` : ''
    );
  }

  console.log('\n[跨档] 三档必须产出不同的对局');
  // 若三档跑出相同结果，说明难度根本没生效（或者被实现成了同一个分支）
  const firstSeed = seeds[0];
  const states = DIFFICULTIES.map((d) => playFullGame(firstSeed, d));
  check(
    'easy / normal / hard 在同一粒子上结果各不相同',
    new Set(states).size === DIFFICULTIES.length,
    states.map((s) => s.slice(0, 40)).join(' / ')
  );

  console.log('\n[默认值] 不传难度等于 normal');
  // 历史分数全部基于标准规则，默认值错了会让所有旧成绩失去意义
  const explicit = playFullGame(1, 'normal');
  const omitted = (() => {
    const game = new Game(1);
    while (!game.gameOver) {
      let advanced = false;
      for (const direction of MOVE_ORDER) {
        if (game.step(direction).moved) {
          advanced = true;
          break;
        }
      }
      if (!advanced) break;
    }
    const rows = [];
    for (let r = 0; r < 4; r++) rows.push(board_values(game.board)[r].map(String).join(' '));
    return (
      `seed=${game.seed} steps=${game.stepCount} score=${game.score} ` +
      `max=${maxTile(game.board)} over=${game.gameOver ? 1 : 0} ` +
      `overflow=${game.sawOverflow ? 1 : 0} board=${rows.join('|')}`
    );
  })();
  check('省略难度参数时行为与 normal 完全一致', omitted === explicit);

  console.log(`\n${'─'.repeat(56)}`);
  if (failures === 0) {
    console.log('难度对拍全部通过');
    process.exitCode = 0;
  } else {
    console.log(`${failures} 项失败`);
    process.exitCode = 1;
  }
}

main();
