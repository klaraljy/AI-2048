// 生成位置分布测试 —— 验"新方块是否被偏向角落"。
//
// 背景：参考实现的「简单难度」会用 70% 的概率把新方块塞进角落，
// 那是靠作弊让玩家赢得轻松。本项目明确不做这种事。
// 用户报告"新的 2 全部生成在角落"，必须用数据确认或排除。
//
// 判据：标准 2048 里每个空格被选中的概率应该相等。
// 4×4 棋盘共 16 格，均匀时每格占比 1/16 = 6.25%。
// 角落 4 格合计应为 25%。

import { Game } from '../web/js/game.js';
import { Rng } from '../web/js/rng.js';

const GAMES = 300;
const DIRECTIONS = ['up', 'down', 'left', 'right'];

let failures = 0;
function check(name, ok, detail) {
  console.log(`  ${ok ? '✓' : '✗'} ${name}${detail ? ` —— ${detail}` : ''}`);
  if (!ok) failures++;
}

/** 直接测 _spawn：在受控的空棋盘上反复生成，统计落点。 */
function testSpawnOnEmptyBoard() {
  console.log('\n[1] 空棋盘上的生成分布（直接调 _spawn）');
  const counts = new Map(); // "r,c" → 次数
  const SAMPLES = 20000;

  for (let i = 0; i < SAMPLES; i++) {
    // 每轮新建一局（棋盘上有 2 个初始块），然后把棋盘清空
    const g = new Game(i);
    for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) g.board[r][c] = 0;
    const spawn = g._spawn();
    const key = `${spawn.row},${spawn.col}`;
    counts.set(key, (counts.get(key) ?? 0) + 1);
  }

  const expected = SAMPLES / 16;
  const cornerKeys = ['0,0', '0,3', '3,0', '3,3'];
  let cornerTotal = 0;
  let minPct = Infinity;
  let maxPct = -Infinity;

  console.log('    每个格子的实际占比（均匀应为 6.25%）：');
  for (let r = 0; r < 4; r++) {
    const row = [];
    for (let c = 0; c < 4; c++) {
      const n = counts.get(`${r},${c}`) ?? 0;
      const pct = (n / SAMPLES) * 100;
      row.push(`${pct.toFixed(2)}%`.padStart(7));
      minPct = Math.min(minPct, pct);
      maxPct = Math.max(maxPct, pct);
      if (cornerKeys.includes(`${r},${c}`)) cornerTotal += n;
    }
    console.log(`      ${row.join(' ')}`);
  }

  const cornerPct = (cornerTotal / SAMPLES) * 100;
  console.log(`    角落 4 格合计: ${cornerPct.toFixed(2)}%（均匀应为 25.00%）`);

  // 卡方检验：15 自由度，α=0.001 的临界值约 37.7
  let chiSquare = 0;
  for (let r = 0; r < 4; r++) {
    for (let c = 0; c < 4; c++) {
      const n = counts.get(`${r},${c}`) ?? 0;
      chiSquare += ((n - expected) ** 2) / expected;
    }
  }
  console.log(`    卡方统计量: ${chiSquare.toFixed(1)}（15 自由度，>37.7 才算有偏）`);

  check(
    '十六格分布均匀（卡方 < 37.7）',
    chiSquare < 37.7,
    `χ²=${chiSquare.toFixed(1)}`
  );
  check(
    '角落合计接近 25%（±3%）',
    Math.abs(cornerPct - 25) < 3,
    `${cornerPct.toFixed(2)}%`
  );
  check(
    '没有任何单格占比超过 9%',
    maxPct < 9,
    `最大 ${maxPct.toFixed(2)}%`
  );
}

/** 真实对局里统计 spawn 事件 —— 判据是**条件均匀**，不是绝对均匀。 */
function testRealGames() {
  console.log('\n[2] 真实对局中的生成分布');
  console.log('    判据说明：走子会把方块推走、腾出特定位置，所以"空格"本身分布不均。');
  console.log('    标准 2048 只要求**在所有空格中等概率**，而不是在 16 格里均匀。');
  console.log('    做法：每次生成时，看落点在"当前空格列表"里排第几（rank），');
  console.log('    均匀时 rank 应等概率取 0..n-1。');
  const GAMES = 300;
  const DIRECTIONS = ['up', 'down', 'left', 'right'];

  // 按"当时有多少个空格"分层统计 rank
  const rankCounts = new Map(); // n → Map(rank → 次数)
  let spawnEvents = 0;

  // 另外统计绝对落点，仅用于展示"为什么看起来偏角落"
  const absolute = new Map();

  for (let seed = 0; seed < GAMES; seed++) {
    const g = new Game(seed);
    const rng = new Rng(seed ^ 0x5a5a);
    for (let step = 0; step < 3000 && !g.gameOver; step++) {
      const dir = DIRECTIONS[rng.nextBounded(4)];

      // 走子前先记下空格顺序（与 _spawn 内部的枚举顺序必须一致：先行后列）
      const before = g.board.map((row) => row.slice());
      const result = g.step(dir);

      if (result.spawn) {
        // 走子会腾出新空位，所以要按**走子后的棋盘**枚举空格。
        // 生成点此时已被占用，但它在生成前必然是空的，所以也算进去。
        const emptiesAfter = [];
        for (let r = 0; r < 4; r++) {
          for (let c = 0; c < 4; c++) {
            const isSpawnHere = result.spawn.row === r && result.spawn.col === c;
            if (isSpawnHere || g.board[r][c] === 0) emptiesAfter.push(`${r},${c}`);
          }
        }
        const key = `${result.spawn.row},${result.spawn.col}`;
        const rank = emptiesAfter.indexOf(key);
        if (rank >= 0) {
          const n = emptiesAfter.length;
          if (!rankCounts.has(n)) rankCounts.set(n, new Map());
          const bucket = rankCounts.get(n);
          bucket.set(rank, (bucket.get(rank) ?? 0) + 1);
          spawnEvents++;
        }
        absolute.set(key, (absolute.get(key) ?? 0) + 1);
      }
    }
  }

  console.log(`    共 ${spawnEvents} 次生成（${GAMES} 局）\n`);

  // 绝对落点：展示"看起来偏角落"的现象
  console.log('    绝对落点占比（仅供解释观感，不作为判据）：');
  const total = [...absolute.values()].reduce((a, b) => a + b, 0);
  for (let r = 0; r < 4; r++) {
    const row = [];
    for (let c = 0; c < 4; c++) {
      const n = absolute.get(`${r},${c}`) ?? 0;
      row.push(`${((n / total) * 100).toFixed(2)}%`.padStart(7));
    }
    console.log(`      ${row.join(' ')}`);
  }

  // 条件均匀：按空格数分层做卡方
  console.log('\n    rank 分布（每个空格数分层检验，均匀时 rank 等概率）：');
  let chiSquareTotal = 0;
  let dofTotal = 0;
  let worstLayer = '';
  let worstChi = 0;

  for (const n of [...rankCounts.keys()].sort((a, b) => a - b)) {
    const bucket = rankCounts.get(n);
    const layerTotal = [...bucket.values()].reduce((a, b) => a + b, 0);
    if (layerTotal < 200) continue; // 样本太小的层不参与检验
    const e = layerTotal / n;
    let chi = 0;
    for (let rank = 0; rank < n; rank++) {
      const observed = bucket.get(rank) ?? 0;
      chi += ((observed - e) ** 2) / e;
    }
    chiSquareTotal += chi;
    dofTotal += n - 1;
    if (chi > worstChi) {
      worstChi = chi;
      worstLayer = `${n} 个空格时 χ²=${chi.toFixed(1)}（dof=${n - 1}）`;
    }
    if (n <= 8 || n === 16) {
      console.log(
        `      ${String(n).padStart(2)} 个空格：${layerTotal} 次，χ²=${chi.toFixed(1)}（dof=${n - 1}）`
      );
    }
  }

  // 卡方临界值近似：dof 很大时约等于 dof + 3*sqrt(2*dof)（α≈0.001 量级）
  const critical = dofTotal + 3 * Math.sqrt(2 * dofTotal);
  console.log(
    `\n    合计 χ²=${chiSquareTotal.toFixed(1)}，dof=${dofTotal}，临界约 ${critical.toFixed(0)}`
  );
  console.log(`    最差的一层：${worstLayer}`);

  check(
    '在所有空格中等概率（条件均匀，χ² 未超临界）',
    chiSquareTotal < critical,
    `χ²=${chiSquareTotal.toFixed(1)} vs 临界 ${critical.toFixed(0)}`
  );

  // 开局两块
  const cornerKeys = ['0,0', '0,3', '3,0', '3,3'];
  const initialCounts = new Map();
  for (let seed = 0; seed < GAMES; seed++) {
    const g = new Game(seed);
    for (let r = 0; r < 4; r++) {
      for (let c = 0; c < 4; c++) {
        if (g.board[r][c] !== 0) {
          const k = `${r},${c}`;
          initialCounts.set(k, (initialCounts.get(k) ?? 0) + 1);
        }
      }
    }
  }
  const initialTotal = GAMES * 2;
  const initialCorner = cornerKeys.reduce((s, k) => s + (initialCounts.get(k) ?? 0), 0);
  const initialCornerPct = (initialCorner / initialTotal) * 100;
  console.log(
    `\n    开局两块：角落 ${initialCorner} / ${initialTotal} = ${initialCornerPct.toFixed(1)}%（均匀 25%）`
  );
  check(
    '开局两块不偏角落（±8%）',
    Math.abs(initialCornerPct - 25) < 8,
    `${initialCornerPct.toFixed(1)}%`
  );
}

/** 2 与 4 的比例应为 90% / 10%。 */
function testTileValues() {
  console.log('\n[3] 新方块取值的比例');
  let twos = 0;
  let fours = 0;
  for (let i = 0; i < 20000; i++) {
    const g = new Game(i);
    for (let r = 0; r < 4; r++) for (let c = 0; c < 4; c++) g.board[r][c] = 0;
    const spawn = g._spawn();
    if (spawn.exponent === 1) twos++;
    else if (spawn.exponent === 2) fours++;
  }
  const fourPct = (fours / (twos + fours)) * 100;
  console.log(`    2 出现 ${twos} 次，4 出现 ${fours} 次 → 4 占 ${fourPct.toFixed(2)}%`);
  check('4 的比例接近 10%（±1%）', Math.abs(fourPct - 10) < 1, `${fourPct.toFixed(2)}%`);
}

console.log('=== 新方块生成位置分布测试 ===');
console.log('（标准 2048：每个空格等概率，不应偏向角落）');
testSpawnOnEmptyBoard();
testRealGames();
testTileValues();

console.log(`\n${'─'.repeat(56)}`);
console.log(failures === 0 ? '生成规则符合标准 2048 ✓' : `${failures} 项失败 —— 生成规则有偏！`);
process.exitCode = failures === 0 ? 0 : 1;
