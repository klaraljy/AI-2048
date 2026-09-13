// 跑完全部测试（前端 + 服务端端到端）。
//
// 顺序有讲究：先验规则（parity），再验渲染，最后验输入 ——
// 规则错了，后面两层的通过就没有意义。
// 服务端那套放最后，因为它会真的起进程，也最慢。
//
// 前置：engine\build 已构建（服务端测试需要 ai2048-server.exe）
//
// 运行（在项目根目录）：
//   node tests\run-all.mjs
//
// 退出码：全过 0，任一失败 1。

import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));

const SUITES = [
  ['规则一致性（与 C++ 引擎穷举对拍）', 'parity.test.mjs'],
  // 生成规则必须是标准 2048：在所有空格中等概率，不得偏向角落。
  // 参考实现的「简单难度」就是靠 70% 概率塞角落来放水的，本项目不做。
  ['新方块生成分布（防角落偏好）', 'spawn.test.mjs'],
  // 三档难度的前端 ↔ 引擎逐位对拍：难度改的是"落点在哪"，
  // 而降级运行时这件事由 JS 决定，两边不一致就变成两个游戏。
  ['难度规则（前端 ↔ 引擎逐位对拍）', 'difficulty-parity.test.mjs'],
  ['JSON 解析器（与 JSON.parse 对拍）', 'json-parity.test.mjs'],
  ['渲染与动画（headless）', 'renderer.test.mjs'],
  // main.js 至今**没有**自动化覆盖（它需要真实 DOM，而 dom-stub 的
  // getElementById 返回 null）。这一套用静态检查覆盖那个盲区：
  // id 是否都存在、事件绑定会不会因 null 元素而中断、
  // overlay 的显示状态是否只有一处写入。
  // 起因：用户报告「结束后弹窗的按钮没反应」，就是最后一类问题。
  ['main.js 结构一致性（id/绑定/显示归属）', 'main-structure.test.mjs'],
  ['输入与输入锁', 'input.test.mjs'],
  ['降级路径（无 WebAudio / 连不上引擎）', 'degraded.test.mjs'],
  // AI 强度/速度的契约：界面承诺「看 2 步」就必须真的搜 2 步
  ['AI 强度与速度档位（含引擎实测）', 'config.test.mjs'],
  // 放最后：这两套会真的起引擎进程（server 还会起静态服务器），
  // 需要先构建好；也是最慢的两套。
  ['WebSocket 服务端（真实进程端到端）', 'server.test.mjs'],
  ['双击启动链路（静态服务器 + 引擎 + 前端连得上）', 'launcher.test.mjs'],
];

const results = [];
for (const [label, file] of SUITES) {
  console.log(`\n=== ${label} ===`);
  const result = spawnSync(process.execPath, [join(here, file)], {
    stdio: 'inherit',
    cwd: join(here, '..'),
  });
  const ok = result.status === 0;
  results.push({ label, file, ok, status: result.status });
}

console.log('\n========================================');
console.log('汇总：');
for (const r of results) {
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.label}  (${r.file})`);
}

const failed = results.filter((r) => !r.ok);
if (failed.length > 0) {
  console.error(`\n${failed.length} / ${results.length} 个测试套件失败`);
  process.exit(1);
}
console.log('\n全部测试通过');
