// 装配层：把引擎、渲染、输入、音效、传输层接起来。
//
// 分工：
//   game.js      规则（与 C++ 引擎逐位一致，由 tests/parity.test.mjs 保证）
//   renderer.js  渲染与动画
//   input.js     键盘 / 触屏 / 输入锁
//   sound.js     音效（WebAudio 合成）
//   transport.js AI 决策来源（引擎 或 本地降级）
//
// 这个文件只管**编排**，不实现上面任何一层的逻辑。

import { Game, maxTile, board_values } from './game.js';
import { Renderer } from './renderer.js';
import { Input, DIRECTION } from './input.js';
import { Sound } from './sound.js';
import { Celebration } from './celebration.js';
import { createTransport } from './transport.js';
import {
  SPEED,
  strengthOptions,
  strengthsDetail,
  speedOptions,
  speedNotes,
  specFor,
  toEngineConfig,
  requestTimeoutMs,
  difficultyOptions,
  DIFFICULTY,
  DEFAULT_DIFFICULTY,
  DEFAULT_STRENGTH,
} from './config.js';

const BEST_KEY = 'ai2048.best';
const ENGINE_URL = readEngineUrl();

/**
 * 用 config.js 的定义填充下拉框。
 *
 * 选项**不在 HTML 里写死** —— 否则参数字典就有两份（HTML 一份、config.js 一份），
 * 改了 JS 忘了 HTML 时，界面显示的和实际生效的会不一致，而且没有任何报错。
 *
 * ⚠️ **下拉框里只写选项名，绝不拼备注**（用户 2026-09-13 反复要求，共提四次）。
 * 这里原来写的是：
 *
 *     node.textContent = option.note ? `${option.label}（${option.note}）` : option.label;
 *
 * 于是三个下拉框全都拖着一条长备注（"简单（80% 按「安全分」加权：偏向角落…）"）。
 * 我前几轮只改了 config.js 的数据层，**没动这一行**，所以用户看到的始终没变 ——
 * 因为备注根本不是在 config.js 里拼的，是在这里拼的。
 *
 * 现在只读 label。备注照旧由 `option.note` 提供给规则弹窗（rulesHtml）。
 * 谁再想在这里拼备注，先看 `tests/main-structure.test.mjs` 里那条断言。
 */
function fillSelect(select, options, defaultValue) {
  select.innerHTML = '';
  for (const option of options) {
    const node = document.createElement('option');
    node.value = option.value;
    node.textContent = option.label;
    if (option.value === defaultValue) node.selected = true;
    select.appendChild(node);
  }
}

/**
 * 一次性取齐所有 DOM 元素。
 *
 * ⚠️ 只放真正用到的元素 —— 死引用（声明了却从不使用）会**掩盖真实的拼写错误**：
 * 原先有两项指向 HTML 里根本不存在的 id（`seed-shown`、`difficulty-note`），
 * 靠 `if (el.x)` 兜着所以不报错，只是永远不执行。
 * `tests/main-structure.test.mjs` 现在会检查"每个 id 都真实存在"。
 *
 * 注意本文件里 `el.` 有**两种**用法，检查工具要都能识别：
 * 直接调用（`el.undo.disabled = ...`）与整对象传参（`tilesLayer: el.tiles`）。
 */
const el = {
  board: document.getElementById('board'),
  grid: document.getElementById('grid'),
  tiles: document.getElementById('tiles'),
  score: document.getElementById('score'),
  best: document.getElementById('best'),
  difficulty: document.getElementById('difficulty'),
  strength: document.getElementById('strength'),
  speed: document.getElementById('speed'),
  newGameButton: document.getElementById('new-game'),
  undo: document.getElementById('undo'),
  aiStep: document.getElementById('ai-step'),
  aiAuto: document.getElementById('ai-auto'),
  mute: document.getElementById('mute'),
  overlay: document.getElementById('overlay'),
  overlayTitle: document.getElementById('overlay-title'),
  overlayText: document.getElementById('overlay-text'),
  overlayUndo: document.getElementById('overlay-undo'),
  overlayRestart: document.getElementById('overlay-restart'),
  notice: document.getElementById('notice'),
  bestBox: document.getElementById('best-box'),
  fxLeft: document.getElementById('fx-left'),
  fxRight: document.getElementById('fx-right'),
  rules: document.getElementById('rules'),
  rulesModal: document.getElementById('rules-modal'),
  rulesBody: document.getElementById('rules-body'),
  rulesClose: document.getElementById('rules-close'),
};

/**
 * 下一局的固定种子；null = 随机。
 *
 * ⚠️ 界面上**没有**任何种子控件（用户要求：玩家每次打开都必须是随机的，
 * 也不该看到"随机"这种东西）。所以这个变量只能由程序设置：
 *
 *     window.AI2048.newGame(12345)   // 浏览器控制台 / Android WebView 桥接
 *
 * 保留它的理由是**可复现性**：同种子才能把两次改动的结果放在一起比。
 * 这是本项目所有 AI 测试的地基（compare / bench 都依赖固定种子）。 */
let pendingSeed = null;

/** 从 URL 查询参数读引擎地址：?engine=ws://127.0.0.1:8765 */
function readEngineUrl() {
  try {
    const params = new URLSearchParams(window.location.search);
    const value = params.get('engine');
    return value && value.trim() !== '' ? value.trim() : null;
  } catch {
    return null;
  }
}

function readBest() {
  try {
    const raw = localStorage.getItem(BEST_KEY);
    const value = raw === null ? 0 : Number(raw);
    return Number.isFinite(value) && value >= 0 ? value : 0;
  } catch {
    return 0;
  }
}

function writeBest(value) {
  try {
    localStorage.setItem(BEST_KEY, String(value));
  } catch {
    // 隐私模式等存不下就算了，不影响本次会话
  }
}

// ---------------------------------------------------------------------------

const renderer = new Renderer({
  tilesLayer: el.tiles,
  gridLayer: el.grid,
  scoreElement: el.score,
  bestElement: el.best,
});

const sound = new Sound();

/** 棋盘两侧的庆祝烟花。达到里程碑 / 合并出新大块时放。 */
const celebration = new Celebration({
  leftCanvas: el.fxLeft,
  rightCanvas: el.fxRight,
  host: el.board,
});

let game = null;
let best = readBest();
/** 本局已经庆祝过的最大块（同一个值只庆祝一次，避免每步都放）。 */
let maxCelebrated = 0;
/** 2048 里程碑只庆祝一次。 */
let celebrated2048 = false;
let transport = null;
let transportDegraded = true;
/**
 * 当前 AI 走的是哪条通道：'native'（APK 里的 C++ 引擎）/ 'websocket'（桌面引擎）/
 * 'local'（内置 JS 降级 AI）。
 *
 * 存在的理由：手机端"AI 没接上"的表现只是**按了没反应**，页面不报错 ——
 * 完全分不清是引擎没连上、还是引擎回了"无步可走"。有了这个值，
 * 按钮的长按提示里就能写清当前用的是哪条通道。见 updateAiChannelHint。
 */
let transportSource = 'local';
let autoRunning = false;
let autoTimer = null;
let lastMove = null;

/** 当前 AI 强度规格，来源是下拉框，参数定义在 config.js。 */
function currentStrength() {
  return specFor(el.strength ? el.strength.value : 'standard');
}

/** 自动演示两步之间的间隔（速度档）。 */
function currentInterval() {
  const spec = SPEED[el.speed ? el.speed.value : 'medium'];
  return spec ? spec.intervalMs : SPEED.medium.intervalMs;
}

/**
 * 把当前强度与难度告诉引擎，并同步前端的等待超时。
 *
 * 三件事必须一起做：只改引擎不改超时，或只改强度不改难度，都会出问题。
 * 失败不抛给用户 —— 引擎不可用时本来就会走降级路径并已在页面上提示。
 */
async function applyStrength() {
  const spec = currentStrength();
  if (transport && typeof transport.setRequestTimeout === 'function') {
    transport.setRequestTimeout(requestTimeoutMs(spec));
  }
  if (!transport || transportDegraded || typeof transport.configure !== 'function') return;
  try {
    // 带上难度：引擎按它给 chance 节点的各空格加权，也就是 AI 的世界模型。
    // 不传的话 hard 档下 AI 会低估"新块贴着自己最大块出现"的风险。
    await transport.configure(toEngineConfig(spec, currentDifficulty()));
  } catch {
    // 引擎中途断了之类：不打断玩，下一步会走 transport 自己的错误路径
  }
}

const input = new Input({
  onMove: (direction) => void performMove(direction),
  /**
   * 查询式上锁：只锁"确实不能走子"的情况。
   *
   * ⚠️ **不要再把 `renderer.busy()` 放进来。** 那会让动画期间（约 250~400ms）
   * 的滑动被直接丢弃，快速连滑时有相当比例的动作被吃掉 ——
   * 用户的手感就是"反应慢慢的、卡卡的"。
   * 动画现在是可以被打断的：`performMove` 会先 `renderer.finishNow()` 收尾上一次动画。
   */
  isLocked: () => autoRunning || game === null || game.gameOver,
  // 规则弹窗开着时：手指仍可拖动（能看规则），但不该走子。
  // ⚠️ 判定用 `.show` 类（modal 的显隐方式），不是 `.hidden`。
  isSwipeBlocked: () => el.rulesModal.classList.contains('show'),
  onUnlock: () => sound.unlock(),
  onRestart: () => newGame(),
  onUndo: () => void doUndo(),
  onToggleMute: () => toggleMute(),
  onAiStep: () => void aiStep(),
});

// 全局手势解锁音频。
//
// ⚠️ **不能只靠 Input 的 onUnlock。**（踩过的坑）
//
// `Input` 只在**棋盘**上监听 pointerdown、在 window 上监听 keydown。
// 但 AI 按钮（单步 / 自动演示）、开局、撤销这些都是普通 <button>，
// 点它们走的是另一条事件路径，永远触发不到 `_fireUnlock`。
//
// 用户看到的现象就是「点 AI 测试没声音，得关掉重开才有」——
// 因为重开后他先碰了棋盘，音频才被解锁。
//
// 这里在**捕获阶段**挂一次全局监听：任何一次点击/按键都解锁。
// 用 capture 是为了先于按钮自己的 handler 执行，保证第一次点击就出声。
// `once` 不能全用：切标签页回来 context 会被挂起，需要能再次恢复，
// 所以保留监听，由 Sound.unlock() 自己保证重复调用是廉价的。
function unlockAudioOnce() {
  sound.unlock();
}
document.addEventListener('pointerdown', unlockAudioOnce, { capture: true });
document.addEventListener('keydown', unlockAudioOnce, { capture: true });

/** 刷新按钮的可用状态。输入锁本身由 isLocked 回调派生，不需要在这里同步。 */
function refreshControls() {
  el.undo.disabled = !game || !game.canUndo || renderer.busy();
  el.aiStep.disabled = !game || renderer.busy() || game.gameOver;
  // ⚠️ 这里**不要**去动 overlay 里那两个按钮的 style.display。
  //
  // 踩过的 bug（用户报告「结束后弹窗的两个按钮没反应」）：
  // 原先这里有 `el.overlayUndo.style.display = game && game.canUndo ? '' : 'none'`，
  // 而它在 performMove() 里紧跟在 checkEndState() **之后**执行（见那里的两行调用）：
  //
  //     checkEndState();    // showOverlay(...) → 按 allowUndo 正确显示按钮
  //     refreshControls();  // 立刻又设 inline display:none，把按钮藏掉
  //
  // 游戏结束时 gameOver 为真 → canUndo 为假 → 按钮在同一帧里被隐藏，
  // 于是"弹窗出来了但点不动"。**内联样式优先级高于类，是静默生效的。**
  //
  // 现在 overlay 的显示状态**只由 showOverlay / hideOverlay 两处负责**
  //（单一归属），refreshControls 不再插手。
}

// ------------------------------------------------------------------ 走子

async function performMove(direction) {
  // ⚠️ 这里原来写的是 `if (renderer.busy()) return false;` —— 动画期间（约 250~400ms）
  // 玩家的滑动被**直接丢弃**。快速连滑时有相当比例的动作被吃掉，手感就是"没反应、
  // 卡卡的"（用户 2026-09-13 反馈）。
  //
  // 现在改成**打断上一次动画**再走这一步：游戏逻辑本来就已同步落定，
  // 打断只影响观感，不会让状态错位。
  if (renderer.busy()) renderer.finishNow();
  if (!game || game.gameOver) return false;

  const before = game.board.map((row) => row.slice());
  const step = game.step(direction);
  if (!step.moved) return false;

  lastMove = direction;
  await renderer.animateMove(step, before, game.score, best);

  if (game.score > best) {
    best = game.score;
    writeBest(best);
    renderer.setBest(best);
    refreshBestHighlight();
  }

  // 音效与庆祝都要用**合并结果**的指数，不是被吸收那张牌的指数。
  //
  // ⚠️ `move.merged === true` 的那条记录描述的是**被吸收**的方块，
  // 它的 exponent 是合并**之前**的值（见 game.js：`exponent: packed[absorbedIndex]`）。
  // 两个 4 合成 8 时它记的是 2（=4），所以结果指数是 exponent + 1。
  //
  // 这里踩过坑：直接把那个值当结果用，于是"合成 8 却弹出 4"，
  // 而且**永远是一半** —— 因为差值恰好是一个二的幂次。
  const absorbedExponent = step.moves
    .filter((m) => m.merged)
    .reduce((acc, m) => Math.max(acc, m.exponent), 0);
  const mergedExponent = absorbedExponent > 0 ? absorbedExponent + 1 : 0;

  if (mergedExponent > 0) {
    sound.playMerge(mergedExponent);
  } else {
    sound.play('slide');
  }
  if (step.spawned) sound.play('spawn');

  // 庆祝：合并出足够大的新块时，在棋盘两侧放小烟花并弹出数字。
  // 用"首次出现的更大块"作为判据，而不是每步都放 ——
  // 每步都放会变成噪声，AI 演示时也吵。
  checkCelebration(mergedExponent);

  // 走到这里动画已经结束，renderer.busy() 为假，输入自然解锁
  checkEndState();
  refreshControls();
  return true;
}

/** 从多大的块开始庆祝。用户指定：1024 起步，更小的不用管。 */
const CELEBRATE_FROM_EXPONENT = 10; // 2^10 = 1024

/**
 * 合并出历史新高的方块时庆祝一次。
 *
 * @param {number} mergedExponent **合并结果**的指数（2^10 = 1024）
 */
function checkCelebration(mergedExponent) {
  if (mergedExponent < CELEBRATE_FROM_EXPONENT) return;
  const value = 2 ** mergedExponent;
  if (value <= maxCelebrated) return; // 同一个值只庆祝第一次

  maxCelebrated = value;
  // 1024 规模 1.0，2048 约 1.2 …… 上限约 1.8，块越大越隆重
  const scale = 1.0 + Math.min(8, mergedExponent - CELEBRATE_FROM_EXPONENT) * 0.1;
  celebration.celebrate(String(value), scale);
}

async function doUndo() {
  if (renderer.busy()) return;
  // 撤销前的分数：动画要把分数滚回旧值，需要知道从哪儿滚
  const scoreBefore = game ? game.score : 0;
  if (!game || !game.undo()) return;

  sound.play('undo');
  hideOverlay();
  maxCelebrated = 0; // 撤销后允许重新庆祝

  // ⚠️ 这里原来是 `renderer.reset(game.board, ...)` —— 整盘清空重画，
  // 方块**直接闪现**到原位，没有滑动动画（用户报的"撤回是直接闪现的"）。
  // 现在走 animateUndo：拿撤销后的棋盘和屏幕上的现状对照，反推出反向滑动。
  await renderer.animateUndo(game.board, scoreBefore, game.score, best);
  refreshBestHighlight();
  refreshControls();
}

function checkEndState() {
  if (game.gameOver) {
    sound.play('gameover');
    showOverlay('游戏结束', `本局 ${game.score} 分，最大方块 ${maxTile(game.board)}`, {
      allowUndo: game.canUndo,
    });
    stopAuto();
    return;
  }
  // 达到 2048 **不再弹窗** —— 弹窗会打断 AI 演示，用户明确反馈过
  // "走一下弹一下"没法测。改成两侧烟花（在 performMove 里已经放了）。
  if (game.reached2048 && !celebrated2048) {
    celebrated2048 = true;
    celebration.celebrate('2048', 2);
  }
}

function showOverlay(title, text, { allowUndo = false, transient = false } = {}) {
  el.overlayTitle.textContent = title;
  el.overlayText.textContent = text;
  // 两个按钮的显示状态**只在这里与 hideOverlay 决定**（单一归属）。
  // 「再来一局」永远可用 —— 它此前从没被显式设置过，一旦被别处留下 inline
  // display:none 就再也回不来了，所以这里显式复位。
  el.overlayUndo.style.display = allowUndo ? '' : 'none';
  el.overlayRestart.style.display = '';
  el.overlay.classList.add('show');
  if (transient) {
    setTimeout(() => {
      // 只在这一层面板仍然是那个"2078 庆祝"面板时收起来，
      // 避免把随后的"游戏结束"面板误关掉
      if (el.overlayTitle.textContent === title) hideOverlay();
    }, 1400);
  }
}

function hideOverlay() {
  el.overlay.classList.remove('show');
  // 收起时把 inline display 清干净，下一个面板从"默认可见"开始 ——
  // 否则上一个面板留下的 none 会带进下一局（同样是静默的）。
  el.overlayUndo.style.display = '';
  el.overlayRestart.style.display = '';
}

// ------------------------------------------------------------------ 规则弹窗

/**
 * 规则说明的内容。
 *
 * ⚠️ **刻意做成数据驱动**：难度与强度的表格直接读 `DIFFICULTY` / `STRENGTH`
 * / `strengthsDetail()`，不在这里重抄一遍数字。
 *
 * 理由是踩过的坑：上一版把数字写死在文案里，于是难度改成"填满行/列"之后，
 * 下拉框里还写着"80% 生成在最大块旁" —— **两处真相必然分家**。
 * 现在改 config.js 的数值，规则弹窗自动跟着变。
 *
 * 仍然需要手写、也必须回来核对的是**规则描述本身**（比如"88% 按填满加权"
 * 这句话的百分比来自 core/game.h 的 kHardWeightedShare）——
 * 改生成规则时对这几处：
 *   出 4 概率 / 加权占比 → engine/src/core/game.h
 *   难度的评分策略       → engine/src/core/game.cpp 的 SafeScore / HostileScore
 *   三档强度深度         → web/js/config.js 的 STRENGTH（本函数自动读取）
 */
function rulesHtml() {
  const p4 = { easy: '10%', normal: '15%', hard: '20%' };
  const diffDesc = {
    easy: '80% 偏向安全位置（角落、边上、空旷处）',
    normal: '全盘均匀随机（标准 2048 规则）',
    hard: '88% 偏向"把某一行／列填满"的位置',
  };
  const diffRows = Object.entries(DIFFICULTY)
    .map(
      ([value, spec]) =>
        `<tr><td>${spec.label}</td><td>${diffDesc[value] ?? '—'}</td><td>${p4[value] ?? '—'}</td></tr>`
    )
    .join('');

  const strengthRows = strengthsDetail()
    .map(
      (s) =>
        `<tr><td>${s.label}</td><td>${s.layers} 层（${s.steps} 步）</td><td>${s.budgetMs} ms</td></tr>`
    )
    .join('');
  const strengthNotes = strengthsDetail()
    .map((s) => `${s.label}：${s.note}`)
    .join('；');

  // 速度档：间隔与用途都从 config.js 读，避免文案与实现分家。
  const speedRows = speedNotes()
    .map((s) => `<tr><td>${s.label}</td><td>${s.intervalMs} ms</td><td>${s.note}</td></tr>`)
    .join('');

  return `
  <h3>怎么玩</h3>
  <ul>
    <li>4×4 棋盘，开局两个方块。四个方向滑动，同值方块相撞合并成两倍。</li>
    <li>每移动一次会在空格里生成一个新方块：<strong>2</strong> 或 <strong>4</strong>。</li>
    <li>合并出的方块同一次移动内不再二次合并。</li>
    <li>棋盘填满且四个方向都动不了时结束；分数＝所有合并出的方块值之和。</li>
    <li>合出 <strong>2048</strong> 不弹窗打断（可以继续往上合）。</li>
  </ul>

  <h3>难度：改的是生成规则</h3>
  <p class="rule-note">难度不是"AI 强弱"，而是新方块出现在哪、出现多大的概率。</p>
  <table class="rule-table">
    <tr><th>难度</th><th>新方块落点</th><th>出 4 的概率</th></tr>
    ${diffRows}
  </table>
  <p class="rule-note">
    困难档的用意：一行只要还有空位就能被继续滑动，<strong>填满之后就变刚性</strong>，
    你只能靠移动整盘来重新腾挪。所以它会优先去填那些"已经快满了"的行或列；
    同时避开能让你立刻合并的位置，以及角落（角落对你有利）。
  </p>
  <p class="rule-note">
    ⚠️ <strong>非「标准」难度的分数不可与标准难度或历史最高分比较</strong> ——
    规则的期望收益不同：简单档天然更容易刷高分，困难档天然更难。
  </p>

  <h3>AI 强度：改的是搜索</h3>
  <table class="rule-table">
    <tr><th>档位</th><th>前瞻深度</th><th>每步思考上限</th></tr>
    ${strengthRows}
  </table>
  <p class="rule-note">
    深度单位是"搜索树层数"：AI 走一步、随机生成一次，各算一层，所以看 N 步＝2N 层。
    空格变少时它会自动加深（最高约 12 层）。时间只是上限，实测通常远低于它 ——
    真正的瓶颈是深度，不是时间。
  </p>
  <p class="rule-note">实测（中等难度，样本较小，仅供量级参考）：${strengthNotes}。</p>
  <p class="rule-note">
    这个 AI 是"期望最大化搜索＋手写评分"：评分项包括单调性、可合并对、蛇形排布、
    最大块位置、空格数等。它不知道后面的方块会出什么，只能按概率求期望。
  </p>

  <h3>AI 速度</h3>
  <table class="rule-table">
    <tr><th>档位</th><th>每步之间的停顿</th><th>用途</th></tr>
    ${speedRows}
  </table>
  <p class="rule-note">只影响「AI操作」连跑时每步之间的停顿，不影响 AI 的下棋水平。</p>

  <h3>快捷键</h3>
  <ul>
    <li><strong>方向键</strong> 或 <strong>W A S D</strong>：移动；手机上直接滑动</li>
    <li><strong>R</strong> 重来 · <strong>U</strong> 撤回 · <strong>M</strong> 音量</li>
    <li><strong>空格</strong>：让 AI 走一步</li>
  </ul>
`;
}

function showRules() {
  el.rulesBody.innerHTML = rulesHtml();
  el.rulesModal.classList.add('show');
}

function hideRules() {
  el.rulesModal.classList.remove('show');
}

// Esc 关闭规则弹窗。放在这里而不是 input.js：input.js 只管「游戏操作」按键，
// 而弹窗的开合属于界面状态。遮罩点击在装配处绑定。
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && el.rulesModal.classList.contains('show')) {
    hideRules();
  }
});

// ------------------------------------------------------------------ AI

async function aiStep() {
  if (renderer.busy() || !game || game.gameOver) return;
  if (!transport) return;

  let decision;
  try {
    decision = await transport.bestMove(board_values(game.board), { lastMove });
  } catch (error) {
    // 通道本身出错（JNI 异常、WebSocket 断了）。以前这里没有 catch，
    // 异常会被 aiStep 的调用方吞掉 —— 表现为"按了没反应"，什么提示都没有。
    showNotice(`AI 通道出错：${error && error.message ? error.message : String(error)}`);
    stopAuto();
    return;
  }

  if (!decision || !decision.move) {
    // 引擎说"无合法走子"= 局面已经结束，不是错误。
    // ⚠️ 但**通道不可用**也走这条分支（原生桥返回 null、请求超时），
    // 两者必须分开报，否则用户看到的就是"按了没反应"而不知道是哪种。
    if (!game.gameOver) {
      showNotice(`AI 没有给出走子（当前通道：${aiChannelName()}）。可能引擎未接上或响应超时。`);
      stopAuto();
    }
    return;
  }
  await performMove(decision.move);
}

function startAuto() {
  if (autoRunning) return;
  autoRunning = true;
  el.aiAuto.classList.add('active');
  // ⚠️ 不写 el.aiAuto.textContent —— 那会把按钮里的 <svg> 一起删掉
  // （图标之所以长期不显示，就是这两行造成的）。播放/暂停由 CSS 按
  // button.active 切换 SVG 内两个 path，见 index.html 的 #ai-auto。
  el.aiAuto.title = '停止 AI 演示';
  refreshControls();

  const loop = async () => {
    if (!autoRunning) return;
    await aiStep();
    if (!autoRunning || !game || game.gameOver) {
      stopAuto();
      return;
    }
    autoTimer = setTimeout(loop, currentInterval());
  };
  void loop();
}

function stopAuto() {
  autoRunning = false;
  if (autoTimer !== null) {
    clearTimeout(autoTimer);
    autoTimer = null;
  }
  el.aiAuto.classList.remove('active');
  el.aiAuto.title = 'AI 自动演示';
  refreshControls();
}

// ------------------------------------------------------------------ 装配

function newGame() {
  stopAuto();
  hideOverlay();

  // 种子：默认随机（玩家每次打开都不一样）；只有程序显式给过才用固定值。
  // 界面没有种子控件 —— 见 pendingSeed 的说明。
  const seed = pendingSeed !== null ? pendingSeed : Math.floor(Math.random() * 1e9);
  pendingSeed = null; // 用掉就清空：下一次点击"新游戏"回到随机

  // 难度是**规则**的一部分，必须传给 Game —— 不传的话界面选了"简单"
  // 而生成仍然是全盘随机，用户会以为难度没生效（而且他无法分辨）。
  game = new Game(seed, currentDifficulty());
  lastMove = null;
  maxCelebrated = 0;
  celebrated2048 = false;

  // 通知 AI 后端"新的一局开始了"。
  // WebSocket 引擎无所谓，但 **Android 的原生引擎要清置换表** ——
  // 表是按"一局之内"设计的（见 engine/src/ai/search.h），跨局复用会把上一局的
  // 搜索结果带进来，既不干净也没必要。
  if (transport && typeof transport.newGame === 'function') transport.newGame();

  renderer.reset(game.board, { score: game.score, best });
  refreshBestHighlight();
  refreshControls();
  celebration.relayout();
}

/**
 * 「所有时间」分数框的状态。
 *
 * 平时是深色底 + 白字；**当前分数超过历史最高时**底色与文字都变成强调色，
 * 而且这个状态只在超过期间保持 —— 这是用户指定的行为。
 */
function refreshBestHighlight() {
  if (!el.bestBox) return;
  const beaten = game !== null && game.score > 0 && game.score >= best;
  el.bestBox.classList.toggle('beaten', beaten);
}

/** 当前难度。元素缺失或值不认识时退回标准档（绝不悄悄变成别的难度）。 */
function currentDifficulty() {
  const value = el.difficulty ? el.difficulty.value : DEFAULT_DIFFICULTY;
  return DIFFICULTY[value] ? value : DEFAULT_DIFFICULTY;
}

/**
// 难度提示条（#difficulty-notice）已按用户要求删除 ——
// "非标准难度的分数不可与基准比较"这条说明移进规则弹窗。
// 这里保留一行注释而不是留一个空函数：死代码会让人以为还有行为。
// 一并去掉的还有它引用的 isStandardDifficulty 导入（现已无人使用）。

function toggleMute() {
  const muted = sound.toggleMuted();
  updateMuteIcon(muted);
  if (!muted) sound.unlock();
}

/**
 * 静音按钮的状态。
 *
 * ⚠️ **不要再往按钮里写 emoji / 文字。** 图标是 HTML 里的内联 SVG，
 * 这里只切 `aria-pressed` —— CSS 依据它显示/隐藏斜杠（见 style.css 的
 * `#mute[aria-pressed='true']`）。
 * 以前这里是 `el.mute.textContent = muted ? '🔇' : '🔊'`，
 * 那样会把 SVG 整个覆盖掉，图标就没了。
 */
function updateMuteIcon(muted) {
  el.mute.setAttribute('aria-pressed', muted ? 'true' : 'false');
  el.mute.title = muted ? '音效已关（M）' : '音效开（M）';
}

function showNotice(text) {
  el.notice.textContent = text;
  el.notice.classList.remove('hidden');
}

/** 通道名，给提示文字用。 */
function aiChannelName() {
  if (transportSource === 'native') return '手机内置引擎（C++）';
  if (transportSource === 'websocket') return '桌面引擎';
  return '降级 AI（简化版）';
}

/**
 * 把"当前用的哪个 AI"写进按钮的 title（长按可见）。
 *
 * ⚠️ 只改 `title`，**不要写 textContent** —— 那两个按钮里都有内联 SVG，
 * 写 textContent 会把图标整个删掉（`#ai-auto` 的图标就这么丢过一次）。
 */
function updateAiChannelHint() {
  const channel = aiChannelName();
  if (el.aiStep) el.aiStep.title = `AI 走一步（空格）· 引擎：${channel}`;
  if (el.aiAuto) el.aiAuto.title = `AI 自动演示 · 引擎：${channel}`;
}

// ------------------------------------------------------------------ 启动

async function boot() {
  updateMuteIcon(sound.muted);

  if (el.difficulty) fillSelect(el.difficulty, difficultyOptions(), DEFAULT_DIFFICULTY);
  // 默认强度与难度都取最高档（用户要求：默认最难 + 最强）
  fillSelect(el.strength, strengthOptions(), DEFAULT_STRENGTH);
  fillSelect(el.speed, speedOptions(), 'medium');

  const requestTimeout = requestTimeoutMs(currentStrength());
  const result = await createTransport({ engineUrl: ENGINE_URL, requestTimeoutMs: requestTimeout });
  transport = result.transport;
  transportDegraded = result.degraded;
  transportSource = result.source || (transportDegraded ? 'local' : 'websocket');

  if (transportDegraded) {
    // 不白屏、不假装：明确告诉用户当前 AI 是降级的
    showNotice(`${result.reason}。游戏与规则完全正常，但"AI 自动"用的是简化 AI。`);
  } else {
    el.notice.classList.add('hidden');
  }

  // AI 通道写进按钮的提示文字里。
  // 为什么值得做：手机端"AI 没接上"的表现是**按了没反应**，而页面一片安静 ——
  // 完全看不出是引擎没连上、还是引擎说"无步可走"。长按按钮能看到用的是哪条通道。
  updateAiChannelHint();

  // 连上之后立刻把当前强度告诉引擎。不告诉的话引擎会用它启动时的默认值，
  // 界面上写着"入门·看 2 步"而实际跑的是深度 8 —— 参数与实际不符比没有参数更糟。
  await applyStrength();

  newGame();

  el.newGameButton.addEventListener('click', () => newGame());
  el.undo.addEventListener('click', () => void doUndo());
  el.aiStep.addEventListener('click', () => void aiStep());
  el.aiAuto.addEventListener('click', () => (autoRunning ? stopAuto() : startAuto()));
  el.mute.addEventListener('click', () => toggleMute());
  el.overlayRestart.addEventListener('click', () => newGame());
  el.overlayUndo.addEventListener('click', () => void doUndo());
  el.rules.addEventListener('click', () => showRules());
  el.rulesClose.addEventListener('click', () => hideRules());
  // 点遮罩空白处也关闭（点面板内部不关 —— 用户可能想选中文字）
  el.rulesModal.addEventListener('click', (event) => {
    if (event.target === el.rulesModal) hideRules();
  });
  el.strength.addEventListener('change', () => void applyStrength());
  // 速度只影响下一步之后的间隔，不需要打断正在进行的演示
  el.speed.addEventListener('change', () => {});
  // 难度是规则的一部分，改档必须重开一局 ——
  // 否则同一局会前半段一套生成规则、后半段另一套，分数失去意义。
  // 还要同步告诉引擎：难度是 AI 的世界模型，不同步的话它会按旧规则评估走子。
  el.difficulty.addEventListener('change', () => {
    newGame();
    void applyStrength();
  });

  window.addEventListener('resize', () => renderer.relayout());
  window.addEventListener('resize', () => {
    renderer.relayout();
    celebration.relayout();
  });
  window.addEventListener('orientationchange', () =>
    setTimeout(() => {
      renderer.relayout();
      celebration.relayout();
    }, 120)
  );

  // 标签页切回来时排一次版：尺寸可能在后台变过
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) {
      renderer.relayout();
      celebration.relayout();
    }
  });

  // 供 Android 的 WebView 桥接使用（里程碑 6）。
  // 只暴露必要动作，不让外部拿到内部对象。
  //
  // ⚠️ `newGame(seed)` 是**唯一**能指定种子的入口 —— 界面上没有种子控件
  //（玩家每次打开都随机，也不该看到"随机"这种东西）。
  // 我的复现命令：控制台执行 `AI2048.newGame(12345)`。
  window.AI2048 = {
    newGame: (seed) => {
      if (seed !== undefined && Number.isFinite(Number(seed))) {
        pendingSeed = Number(seed);
      }
      newGame();
    },
    undo: () => void doUndo(),
    move: (direction) => void performMove(direction),
    aiStep: () => void aiStep(),
    isDegraded: () => transportDegraded,
    /**
     * 当前 AI 走的是哪条通道：'native'（APK 里的 C++ 引擎）/ 'websocket' / 'local'。
     * 排查"AI 没反应"时第一个该看的就是它。
     */
    aiChannel: () => transportSource,
    /** 当前这一局的种子 —— 复现问题时需要它。 */
    currentSeed: () => (game ? game.seed : null),
  };
}

void boot();

export { performMove, newGame, DIRECTION };
