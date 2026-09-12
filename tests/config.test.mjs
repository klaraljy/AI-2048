// AI 强度/速度的契约测试。
//
// 为什么值得单独测：界面上的「看 2 步」是**对用户的承诺**。
// 如果 config.js 里的 depth 改了而没人核对引擎行为，
// 界面就会显示「入门·看 2 步」而引擎实际搜 4 步 —— 用户被骗且无从发现。
//
// 这里验三件事：
//   1. config.js 的换算（depth → 步数）与自己的定义一致
//   2. 发给引擎的字段名与引擎真正认识的字段一致
//   3. 引擎收到后**实际搜索深度**确实等于承诺值（真起进程验）

import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { connectWebSocket } from './ws-client.mjs';
import {
  SPEED,
  STRENGTH,
  specFor,
  stepsAhead,
  toEngineConfig,
  requestTimeoutMs,
} from '../web/js/config.js';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const ROOT_ESM = ROOT.split('\\').join('/');

let passed = 0;
const failures = [];

function check(name, ok, detail) {
  if (ok) {
    passed++;
    console.log(`  ✓ ${name}`);
  } else {
    failures.push(name);
    console.log(`  ✗ ${name}${detail === undefined ? '' : ` —— ${detail}`}`);
  }
}

async function main() {
  console.log('\n[1] 强度档位定义');
  check('三档存在：入门 / 标准 / 最强', Object.keys(STRENGTH).length === 3, Object.keys(STRENGTH).join(','));
  check(
    '深度递增（入门 < 标准 < 最强）',
    STRENGTH.beginner.depth < STRENGTH.standard.depth && STRENGTH.standard.depth < STRENGTH.expert.depth,
    `${STRENGTH.beginner.depth} / ${STRENGTH.standard.depth} / ${STRENGTH.expert.depth}`
  );
  check(
    '用时递增',
    STRENGTH.beginner.budgetMs < STRENGTH.standard.budgetMs &&
      STRENGTH.standard.budgetMs < STRENGTH.expert.budgetMs
  );
  // 深度是**层**数（max 与 chance 交替），所以步数 = depth / 2。
  // 写成奇数会让换算出现半步，界面上就会显示成「看 2.5 步」。
  check(
    '三个深度都是偶数（否则步数会出现 .5）',
    [STRENGTH.beginner, STRENGTH.standard, STRENGTH.expert].every((s) => s.depth % 2 === 0),
    [STRENGTH.beginner.depth, STRENGTH.standard.depth, STRENGTH.expert.depth].join(',')
  );
  check('stepsAhead(4) = 2 步', stepsAhead(4) === 2, String(stepsAhead(4)));
  check('stepsAhead(8) = 4 步', stepsAhead(8) === 4, String(stepsAhead(8)));

  console.log('\n[2] 速度档位定义');
  check('三档存在：慢 / 中 / 快', Object.keys(SPEED).length === 3, Object.keys(SPEED).join(','));
  check(
    '间隔递减（慢 > 中 > 快）',
    SPEED.slow.intervalMs > SPEED.medium.intervalMs &&
      SPEED.medium.intervalMs > SPEED.fast.intervalMs,
    `${SPEED.slow.intervalMs} / ${SPEED.medium.intervalMs} / ${SPEED.fast.intervalMs}`
  );

  console.log('\n[3] 超时必须大于引擎的思考预算');
  for (const [key, spec] of Object.entries(STRENGTH)) {
    check(
      `${spec.label}：等待超时 > 思考预算`,
      requestTimeoutMs(spec) > spec.budgetMs,
      `${requestTimeoutMs(spec)}ms > ${spec.budgetMs}ms`
    );
  }

  console.log('\n[4] 未知识别回退到标准档（不能让界面选了个不存在的档就崩）');
  check('specFor("不存在") 回退到标准', specFor('不存在') === STRENGTH.standard);

  console.log('\n[5] 引擎真的按承诺深度搜（起真进程）');
  const probe = createServer();
  await new Promise((r) => probe.listen(0, '127.0.0.1', r));
  const port = probe.address().port;
  await new Promise((r) => probe.close(r));

  const child = spawn(join(ROOT, 'engine', 'build', 'ai2048-server.exe'), [
    '--port',
    String(port),
    // 故意给一个很深的启动默认值：能改小才说明 configure 真的生效
    '--depth',
    '10',
  ]);
  const engineOutput = [];
  child.stdout.on('data', (c) => engineOutput.push(c.toString('utf8')));
  child.stderr.on('data', (c) => engineOutput.push(c.toString('utf8')));

  await new Promise((r) => setTimeout(r, 800));
  const ws = await connectWebSocket(`ws://127.0.0.1:${port}`);

  // 中局局面：空格少一点，搜索量真实
  const board = [
    [2, 0, 4, 2],
    [8, 4, 16, 4],
    [32, 16, 64, 8],
    [128, 64, 256, 512],
  ];

  for (const [key, spec] of Object.entries(STRENGTH)) {
    const config = toEngineConfig(spec);
    // 字段名必须是引擎认识的：写错了引擎会静默忽略（只认已知键）
    check(
      `${spec.label}：configure 用的是引擎认识的字段名`,
      'baseDepth' in config && 'timeBudgetMs' in config,
      JSON.stringify(config)
    );

    await ws.request({ id: 1, type: 'configure', payload: { config } });
    const response = await ws.request({
      id: 2,
      type: 'best-move',
      payload: { state: { board } },
    });

    const info = response.payload.debugInfo;
    // ⚠️ **不能断言 searchDepth === spec.depth。**
    //
    // 那曾经是对的，但只是因为**自适应深度的加成当时完全没生效**：
    // 迭代加深的循环只在偶数层步进，而所有调整都是 ±1，奇数目标被静默丢掉。
    // 修掉之后三档分别搜到 6 / 8 / 10 层 —— 测试立刻变红，而那时引擎是对的。
    //
    // 现在断言的是**真正要保证的性质**：承诺深度是**下限**（引擎至少搜那么深），
    // 实际深度是偶数，且不会离谱地超过承诺值。
    const lowerBound = spec.depth;
    const upperBound = spec.depth + 6; // 自适应最多加三层（空格 + 机动性）
    check(
      `${spec.label}：引擎实际搜到不少于承诺的 ${stepsAhead(spec.depth)} 步（自适应只会更深）`,
      info.searchDepth >= lowerBound && info.searchDepth <= upperBound,
      `承诺 depth=${spec.depth}，实际 ${info.searchDepth}`
    );
    check(
      `${spec.label}：实际深度是偶数层（奇数层拿不到局面信息）`,
      info.searchDepth % 2 === 0,
      `实际 ${info.searchDepth}`
    );
    check(
      `${spec.label}：debugInfo.baseDepth 回显为 ${spec.depth}`,
      info.baseDepth === spec.depth,
      `实际 ${info.baseDepth}`
    );
    check(
      `${spec.label}：单步耗时不超过预算的 2 倍（预算 ${spec.budgetMs}ms）`,
      info.timeCostMs < spec.budgetMs * 2,
      `${info.timeCostMs.toFixed(1)}ms`
    );
  }

  ws.close();
  child.kill();
  await new Promise((r) => setTimeout(r, 200));

  console.log(`\n${'─'.repeat(56)}`);
  if (failures.length === 0) {
    console.log(`config.test.mjs：${passed} 项全部通过`);
    process.exitCode = 0;
  } else {
    console.log(`config.test.mjs：${passed} 项通过，${failures.length} 项失败`);
    for (const f of failures) console.log(`  ✗ ${f}`);
    process.exitCode = 1;
  }
}

main().catch((error) => {
  console.error(`\n测试中断：${error.message}`);
  process.exitCode = 1;
});
