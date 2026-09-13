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
import { difficultyOptions, strengthOptions, speedOptions } from '../web/js/config.js';

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
console.log('\n[6] 下拉框里只有选项名，没有备注文字');
// ---------------------------------------------------------------------------
// 真实 bug 的教训（用户提了**四次**才被我改对）：
// 「三个下拉框里的备注文字统统删掉」—— 我前几轮一直在改 config.js 的数据层
// （去掉了 label 里拼的备注、把说明挪到规则弹窗），但界面**一点没变**。
// 因为备注根本不是 config.js 拼的，是 main.js 的 fillSelect 拼的：
//
//     node.textContent = option.note ? `${option.label}（${option.note}）` : option.label;
//
// 三处下拉框共用这一个函数，所以改一处就够了 —— 而我一直没打开这个文件。
//
// 这条断言从**两个方向**夹住它：
//   a) 源码方向：fillSelect 里只允许出现 `= option.label`，出现模板串/拼接就是回归；
//   b) 数据方向：真正 import config.js，三个 options() 给出的 label
//      必须不带任何备注痕迹（括号、间隔号、换行）。
//      —— 这一条能抓住"从 config.js 侧把备注塞回 label"的改法。
const fillStart = lines.findIndex((l) => /^function fillSelect/.test(l));
let fillBody = '';
if (fillStart >= 0) {
  let depth = 0;
  let started = false;
  for (let i = fillStart; i < lines.length; i++) {
    for (const ch of lines[i]) {
      if (ch === '{') { depth++; started = true; }
      else if (ch === '}') depth--;
    }
    fillBody += lines[i] + '\n';
    if (started && depth === 0) break;
  }
} else {
  check('找得到 fillSelect 函数', false);
}
const fillCode = fillBody.replace(/\/\/.*$/gm, '');

check(
  'fillSelect 只把 label 写进选项，不拼 note（这次 bug 的直接回归）',
  /textContent\s*=\s*option\.label\s*;/.test(fillCode) &&
    !/\$\{.*(note|label)/.test(fillCode) &&
    !/option\.label\s*\+/.test(fillCode),
  fillCode.match(/textContent[^;]*/)?.[0] ?? '（没找到赋值语句）'
);

// 数据方向：三个下拉框的 label 必须是"光名"，不含备注痕迹。
const SUSPECT = /[（(·、：:\n]|--|——/;
for (const [name, fn] of [
  ['难度', difficultyOptions],
  ['强度', strengthOptions],
  ['速度', speedOptions],
]) {
  const bad = fn().filter((o) => SUSPECT.test(o.label));
  check(
    `${name}下拉框的 label 不带备注（例：${fn()[0].label}）`,
    bad.length === 0,
    bad.map((o) => o.label).join(' | ')
  );
}

// 反向确认：备注**没有丢**，它们还在规则弹窗里（不能靠删数据来通过上面的断言）。
const noteKeepers = [
  ['强度', strengthOptions().every((o) => typeof o.note === 'string' && o.note.length > 0)],
  ['难度', difficultyOptions().every((o) => typeof o.note === 'string' && o.note.length > 0)],
  ['速度', main.includes('speedNotes()')],
];
check(
  '备注只是移出下拉框，仍然保留给规则弹窗',
  noteKeepers.every(([, ok]) => ok),
  noteKeepers.filter(([, ok]) => !ok).map(([n]) => n).join(', ')
);

// ---------------------------------------------------------------------------
console.log('\n[7] 带图标的按钮不会被 textContent 抹掉图标');
// ---------------------------------------------------------------------------
// 真实 bug 的教训：`#ai-auto`（AI操作）的图标**从来不显示**。
// 根因是 `el.aiAuto.textContent = '▶'` —— 按钮的 textContent 是**整个按钮的
// 文本**，一赋值就把里面的 <svg> 节点全部删掉。而且 `newGame()` 会调
// `stopAuto()`，所以页面一打开图标就已经没了。
//
// 这类 bug 很难靠肉眼发现：按钮看起来"有内容"（有个 ▶ 字形），只是没图标、
// 还比别的按钮窄。UI 检查脚本（真浏览器量 innerHTML）才把它揪出来。
//
// 判据：任何**含内联 <svg> 的按钮**，其 id 都不能出现在 `.textContent =` 的左边，
// 也不能出现在 `.innerHTML =` 的左边。
const buttonsWithSvg = [];
for (const m of html.matchAll(/<button\b([^>]*)>([\s\S]*?)<\/button>/g)) {
  const [, attrs, inner] = m;
  const id = attrs.match(/id="([^"]+)"/)?.[1];
  if (id && /<svg\b/.test(inner)) buttonsWithSvg.push(id);
}
check(
  'index.html 里能找到带内联 SVG 的按钮',
  buttonsWithSvg.length >= 8,
  buttonsWithSvg.join(', ')
);

const codeOnly = main.replace(/\/\/.*$/gm, '').replace(/\/\*[\s\S]*?\*\//g, '');
const clobbered = [];
for (const id of buttonsWithSvg) {
  // el.<name> 映射到该 id 的元素名
  const names = [...elMap.entries()].filter(([, elId]) => elId === id).map(([n]) => n);
  for (const name of names) {
    const re = new RegExp(`el\\.${name}\\.(textContent|innerHTML)\\s*=`, 'g');
    for (const hit of codeOnly.matchAll(re)) clobbered.push(`${name}.${hit[1]}`);
  }
}
check(
  '没有代码给"带图标的按钮"写 textContent / innerHTML（会删掉 SVG）',
  clobbered.length === 0,
  clobbered.join(', ')
);

// AI操作按钮的状态必须画在 SVG 里，而不是靠替换按钮文字。
const aiAutoTag = html.match(/<button[^>]*id="ai-auto"[\s\S]*?<\/button>/)?.[0] ?? '';
check(
  'AI操作按钮在 SVG 里同时准备了播放与暂停两个图形',
  /class="ai-play"/.test(aiAutoTag) && /class="ai-pause"/.test(aiAutoTag),
  aiAutoTag.replace(/\s+/g, ' ').slice(0, 120)
);
check(
  'main.js 只切换 active 类，不再替换按钮文字',
  /el\.aiAuto\.classList\.(add|remove)\('active'\)/.test(codeOnly) &&
    !/el\.aiAuto\.textContent/.test(codeOnly)
);

// ---------------------------------------------------------------------------
console.log('\n----------------------------------------');
if (failures > 0) {
  console.error(`main.js 结构一致性：${failures} 项失败`);
  process.exit(1);
}
console.log('main.js 结构一致性：全部通过');
