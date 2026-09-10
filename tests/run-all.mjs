// 跑完全部前端测试。
//
// 顺序有讲究：先验规则（parity），再验渲染，最后验输入 ——
// 规则错了，后面两层的通过就没有意义。
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
  ['渲染与动画（headless）', 'renderer.test.mjs'],
  ['输入与输入锁', 'input.test.mjs'],
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
console.log('\n全部前端测试通过');
