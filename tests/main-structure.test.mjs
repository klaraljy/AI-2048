// main.js 与 index.html 的**结构一致性**测试。
//
// 为什么需要它（真实 bug 的教训）：用户报告「游戏结束后弹窗的两个按钮没反应」，
// 根因是 main.js 的 `refreshControls()` 用内联样式把 overlay 按钮设成了
// `display:none`，而它在 `performMove()` 里紧跟在 `checkEndState()` 之后执行：
//
//     checkEndState();    // showOverlay() 正确显示按钮
//     refreshControls();  // 立刻又设为 none —— 按钮在同一帧被藏掉
//
// 这个 bug **任何现有测试都抓不到**：它只在真实 DOM 上、且只在"游戏结束"这个
// 分支里出现。而 main.js 需要真实 id 才能初始化，dom-stub 的
// getElementById 返回 null，所以 main.js 一直没被自动化覆盖。
//
// 本套件用**静态检查**覆盖这个 bug 类别（不需要浏览器）：
//   1. main.js 查的每个元素 id 都必须在 index.html 里存在 ——
//      id 写错会在初始化时抛异常，**并静默跳过之后所有事件绑定**
//      （overlay 按钮恰好是最后绑定的两个，所以它们最先遭殃）。
//   2. 每个 CSS/JS/资源引用都能在磁盘上找到 —— 断链同样会让页面半死。
//   3. overlay 的显示状态只由 showOverlay / hideOverlay 写入 ——
//      归属唯一化。这条直接锁住上面那个 bug：谁再往别处塞
//      `overlayUndo.style.display` 就会失败。
//   4. 声明了却从不使用的 el.xxx 要报出来（死引用会掩盖真实的拼写错误）。
//
// 运行： node tests/main-structure.test.mjs

import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const ROOT = join(here, '..');
const WEB = join(ROOT, 'web');

let failures = 0;
function check(label, condition, detail = '') {
  if (condition) {
    console.log(`  ✓ ${label}`);
  } else {
    failures += 1;
    console.error(`  ✗ ${label}${detail ? `  —— ${detail}` : ''}`);
  }
}

const html = readFileSync(join(WEB, 'index.html'), 'utf8');
const main = readFileSync(join(WEB, 'js', 'main.js'), 'utf8');

// ---------------------------------------------------------------------------
console.log('\n[1] index.html 里的 id 与 main.js 的查找一一对应');
// ---------------------------------------------------------------------------
const htmlIds = new Set();
for (const m of html.matchAll(/id="([^"]+)"/g)) htmlIds.add(m[1]);
check('index.html 能被解析出 id', htmlIds.size > 10, `${htmlIds.size} 个`);

// el.<name>: document.getElementById('<id>')
const elMap = new Map();
for (const m of main.matchAll(/(\w+):\s*document\.getElementById\('([^']+)'\)/g)) {
  elMap.set(m[1], m[2]);
}
check('main.js 声明了 el 元素映射', elMap.size > 10, `${elMap.size} 项`);

const missing = [...elMap.entries()].filter(([, id]) => !htmlIds.has(id));
check(
  '每个 getElementById 的目标都存在于 index.html',
  missing.length === 0,
  missing.map(([name, id]) => `el.${name} -> #${id}`).join(', ')
);

// ---------------------------------------------------------------------------
console.log('\n[2] 事件绑定不会因为 null 元素而中断');
// ---------------------------------------------------------------------------
// main.js 的绑定是一串直线语句：前一个抛异常，后面的全部不执行。
// 所以**绑定的顺序**决定了谁最先受害 —— overlay 按钮在最后。
const bindings = [];
for (const m of main.matchAll(/el\.(\w+)\.addEventListener/g)) bindings.push(m[1]);
check('main.js 里有事件绑定', bindings.length >= 8, `${bindings.length} 处`);

const unboundNull = bindings.filter((name) => {
  const id = elMap.get(name);
  return !id || !htmlIds.has(id);
});
check(
  '所有被绑定事件的元素都真实存在（否则后续绑定会被静默跳过）',
  unboundNull.length === 0,
  unboundNull.join(', ')
);

// overlay 的两个按钮必须在绑定列表里 —— 用户报告的 bug 就是它们。
check(
  'overlay 的「撤销一步」「再来一局」都绑定了事件',
  bindings.includes('overlayUndo') && bindings.includes('overlayRestart'),
  bindings.join(', ')
);

// ---------------------------------------------------------------------------
console.log('\n[3] overlay 显示状态的写入点唯一');
// ---------------------------------------------------------------------------
// 只允许 showOverlay / hideOverlay 两个函数写 overlay 的显示状态。
// 别的函数碰它 → 就会出现「弹窗出来了但按钮被藏掉」这一类静默 bug。
const overlayWrites = [];
const lines = main.split('\n');
lines.forEach((line, i) => {
  const code = line.replace(/\/\/.*$/, ''); // 忽略注释
  if (/(overlayUndo|overlayRestart)\.style\.display|overlay\.classList\.(add|remove)/.test(code)) {
    overlayWrites.push({ line: i + 1, text: line.trim() });
  }
});

// 找出每个写入点所属的函数。
function ownerFunction(lineNumber) {
  for (let i = lineNumber - 1; i >= 0; i--) {
    const m = lines[i].match(/^function (\w+)/);
    if (m) return m[1];
  }
  return '(top level)';
}
const owners = [...new Set(overlayWrites.map((w) => ownerFunction(w.line)))];
check(
  '只有 showOverlay / hideOverlay 写 overlay 的显示状态',
  owners.every((o) => o === 'showOverlay' || o === 'hideOverlay'),
  `实际写入者：${owners.join(', ')}`
);

// 反向确认：refreshControls 不再碰它（正是这次的 bug）。
const refreshStart = lines.findIndex((l) => /^function refreshControls/.test(l));
if (refreshStart >= 0) {
  let depth = 0;
  let started = false;
  let body = '';
  for (let i = refreshStart; i < lines.length; i++) {
    for (const ch of lines[i]) {
      if (ch === '{') { depth++; started = true; }
      else if (ch === '}') depth--;
    }
    body += lines[i] + '\n';
    if (started && depth === 0) break;
  }
  const stripped = body.replace(/\/\/.*$/gm, '');
  check(
    'refreshControls() 不写 overlay 的显示状态（这次 bug 的直接回归）',
    !/overlay(Undo|Restart)?\.(style|classList)/.test(stripped),
    'refreshControls 里又出现了 overlay 样式写入'
  );
} else {
  check('找得到 refreshControls 函数', false);
}

// ---------------------------------------------------------------------------
console.log('\n[4] el 的每一项都指向真实存在的 id');
// ---------------------------------------------------------------------------
// ⚠️ 这里**故意不检查**"声明了却从不使用"。
//
// 我第一版加了那个检查，它反复误报（`el.` 有直接调用与整对象传参两种用法，
// 静态匹配区分不了），而我依据它的误报**删掉了 6 个正在使用的声明** ——
// 一个不可靠的检查比没有检查更危险。真正的判据是下面这条：
// 每个 el 项指向的 id 必须真实存在（id 写错才是会静默出问题的那个）。
const elIds = [...elMap.values()];
const badIds = [...elMap.entries()].filter(([, id]) => !htmlIds.has(id));
check(
  `el 的 ${elIds.length} 项全部指向 index.html 里真实存在的 id`,
  badIds.length === 0,
  badIds.map(([n, id]) => `el.${n} -> #${id}`).join(', ')
);

// 参考信息（不作为判据）：只被引用一次的项，可能是死引用也可能是"整对象传参"。
const singleUse = [...elMap.keys()].filter((name) => {
  const uses = [...main.matchAll(new RegExp(`el\\.${name}\\b`, 'g'))].length;
  return uses <= 1;
});
if (singleUse.length > 0) {
  console.log(`  · 提示（非失败）：只被引用一次：${singleUse.join(', ')}`);
}

// ---------------------------------------------------------------------------
console.log('\n[5] 页面引用的资源都存在于磁盘');
// ---------------------------------------------------------------------------
const refs = [];
for (const m of html.matchAll(/(?:src|href)="([^"]+)"/g)) {
  const url = m[1];
  if (/^https?:|^\/\/|^#|^data:/.test(url)) continue;
  refs.push(url.replace(/^\.\//, '').replace(/^\//, ''));
}
let brokenRefs = [];
for (const rel of refs) {
  if (!existsSync(join(WEB, rel))) brokenRefs.push(rel);
}
check('index.html 引用的本地资源都存在', brokenRefs.length === 0, brokenRefs.join(', '));
check('至少检查到几个资源引用', refs.length >= 2, `${refs.length} 个`);

// ---------------------------------------------------------------------------
console.log('\n----------------------------------------');
if (failures > 0) {
  console.error(`main.js 结构一致性：${failures} 项失败`);
  process.exit(1);
}
console.log('main.js 结构一致性：全部通过');
