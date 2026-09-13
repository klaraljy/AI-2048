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
  speedOptions,
  specFor,
  toEngineConfig,
  requestTimeoutMs,
  difficultyOptions,
  difficultyLabel,
  isStandardDifficulty,
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
 */
function fillSelect(select, options, defaultValue) {
  select.innerHTML = '';
  for (const option of options) {
    const node = document.createElement('option');
    node.value = option.value;
    node.textContent = option.note ? `${option.label}（${option.note}）` : option.label;
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
  difficultyNotice: document.getElementById('difficulty-notice'),
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
  boardElement: el.board,
  onMove: (direction) => void performMove(direction),
  // 查询式上锁：动画进行中或 AI 自动播放时丢弃玩家输入。
  isLocked: () => renderer.busy() || autoRunning || game === null || game.gameOver,
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
  if (renderer.busy()) return false;
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
  if (!game || !game.undo()) return;

  sound.play('undo');
  hideOverlay();
  maxCelebrated = 0; // 撤销后允许重新庆祝
  renderer.reset(game.board, { score: game.score, best });
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
 * ⚠️ **这里的每个数字都必须与实现一致**，因为它会显示给玩家。
 * 写死的文案最容易随改动作废 —— 本项目就发生过：难度改成"填满行/列"之后，
 * 下拉框里还写着"80% 生成在最大块旁"。改规则时**必须回来核对这里**。
 *
 * 数据来源（改的时候对这几处）：
 *   出 4 概率      → engine/src/core/game.h 的 kFourSpawnEasy/Normal/Hard
 *   难度的生成策略 → engine/src/core/game.cpp 的 SafeScore / HostileScore
 *   加权占比       → engine/src/core/game.h 的 kEasy/HardWeightedShare
 *   三档强度深度   → web/js/config.js 的 STRENGTH
 */
const RULES_HTML = `
  <h3>怎么玩</h3>
  <ul>
    <li>4×4 棋盘，开局两个方块。四个方向滑动，同值方块相撞合并成两倍。</li>
    <li>每移动一次会在空格里生成一个新方块：<strong>2</strong> 或 <strong>4</strong>。</li>
    <li>合并出的方块同一次移动内不再二次合并。</li>
    <li>棋盘填满且四个方向都动不了时结束；分数＝所有合并出的方块值之和。</li>
    <li>合出 <strong>2048</strong> 不弹窗打断（可以继续往上合）。</li>
  </ul>

  <h3>难度改变的是生成规则</h3>
  <p class="rule-note">难度不是"AI 强弱"，而是新方块出现在哪、出现多大的概率。</p>
  <table class="rule-table">
    <tr><th>难度</th><th>新方块落点</th><th>出 4 的概率</th></tr>
    <tr><td>简单</td><td>80% 偏向安全位置（角落、边上、空旷处），20% 全盘随机</td><td>10%</td></tr>
    <tr><td>中等</td><td>全盘随机（标准 2048 规则）</td><td>15%</td></tr>
    <tr><td>困难</td><td>88% 偏向"把某一行／列填满"的位置，12% 全盘随机</td><td>20%</td></tr>
  </table>
  <p class="rule-note">
    困难档的用意：一行只要还有空位就能被继续滑动，<strong>填满之后就变刚性</strong>，
    你只能靠移动整盘来重新腾挪。所以它会优先去填那些"已经快满了"的行或列。
    它还会避开能让你立刻合并的位置，以及角落（角落对你有利）。
  </p>
  <p class="rule-note">
    ⚠️ 非中等难度的分数<strong>不可与标准难度或历史最高分比较</strong> ——
    规则的期望收益不同。
  </p>

  <h3>AI 强度</h3>
  <table class="rule-table">
    <tr><th>档位</th><th>前瞻深度</th><th>每步思考上限</th></tr>
    <tr><td>入门</td><td>4 层（2 步）</td><td>300 ms</td></tr>
    <tr><td>标准</td><td>6 层（3 步）</td><td>800 ms</td></tr>
    <tr><td>最强</td><td>8 层（4 步）</td><td>2500 ms</td></tr>
  </table>
  <p class="rule-note">
    深度单位是"搜索树层数"：AI 走一步、随机生成一次，各算一层，所以看 N 步＝2N 层。
    空格变少时它会自动加深（最高约 12 层）；时间只是上限，实际通常远低于它。
  </p>
  <p class="rule-note">
    这个 AI 是"期望最大化搜索＋手写评分"。它的评分项包括：单调性、可合并对、
    蛇形排布、最大块位置、空格数等。上一轮的基准：
    标准档平均约 5 万，最强档在中等难度平均约 7 万、困难难度约 6 万。
  </p>

  <h3>快捷键</h3>
  <ul>
    <li><strong>方向键</strong> 或 <strong>W A S D</strong>：移动；手机上直接滑动</li>
    <li><strong>R</strong> 新游戏 · <strong>U</strong> 撤销 · <strong>M</strong> 静音</li>
    <li><strong>空格</strong>：让 AI 走一步</li>
  </ul>
`;

function showRules() {
  el.rulesBody.innerHTML = RULES_HTML;
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

  const decision = await transport.bestMove(board_values(game.board), { lastMove });
  if (!decision.move) {
    // 没有合法步意味着已经结束；不当作错误
    return;
  }
  await performMove(decision.move);
}

function startAuto() {
  if (autoRunning) return;
  autoRunning = true;
  el.aiAuto.classList.add('active');
  el.aiAuto.textContent = '⏸';
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
  el.aiAuto.textContent = '▶';
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
  renderer.reset(game.board, { score: game.score, best });
  refreshDifficultyNotice();
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
 * 非标准难度时在页面上**明确说明**分数不可与基准比较。
 *
 * 这一条不能省：难度改的是生成规则，简单档分数天然更高。
 * 不提示的话，用户会把"简单档刷出的高分"当成 AI 变强了。
 */
function refreshDifficultyNotice() {
  const value = currentDifficulty();
  // ⚠️ 这里**曾经**还有一段写 `el.difficultyNote` 的代码，而 HTML 里从来没有
  // `#difficulty-note` 这个元素 —— 那段是死代码，靠 `if (el.difficultyNote)`
  // 兜着所以不报错，只是永远不执行。
  // 难度的即时说明由下面这个 `#difficulty-notice`（真实存在）承担，
  // 下拉框里每一项的说明由 config.js 的 note 通过 option 文本给出。
  if (!el.difficultyNotice) return;

  if (isStandardDifficulty(value)) {
    el.difficultyNotice.classList.add('hidden');
    el.difficultyNotice.textContent = '';
    return;
  }
  el.difficultyNotice.textContent =
    `当前难度「${difficultyLabel(value)}」改的是生成规则，` +
    `不是标准 2048 —— 本局分数不可与标准难度或历史最高分比较。`;
  el.difficultyNotice.classList.remove('hidden');
}

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

  if (transportDegraded) {
    // 不白屏、不假装：明确告诉用户当前 AI 是降级的
    showNotice(`${result.reason}。游戏与规则完全正常，但"AI 自动"用的是简化 AI。`);
  } else {
    el.notice.classList.add('hidden');
  }

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
    /** 当前这一局的种子 —— 复现问题时需要它。 */
    currentSeed: () => (game ? game.seed : null),
  };
}

void boot();

export { performMove, newGame, DIRECTION };
