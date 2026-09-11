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

const el = {
  board: document.getElementById('board'),
  grid: document.getElementById('grid'),
  tiles: document.getElementById('tiles'),
  score: document.getElementById('score'),
  best: document.getElementById('best'),
  seed: document.getElementById('seed'),
  seedShown: document.getElementById('seed-shown'),
  difficulty: document.getElementById('difficulty'),
  difficultyNote: document.getElementById('difficulty-note'),
  difficultyNotice: document.getElementById('difficulty-notice'),
  strength: document.getElementById('strength'),
  speed: document.getElementById('speed'),
  newGame: document.getElementById('new-game'),
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
};

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

let game = null;
let best = readBest();
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

/** 刷新按钮的可用状态。输入锁本身由 isLocked 回调派生，不需要在这里同步。 */
function refreshControls() {
  el.undo.disabled = !game || !game.canUndo || renderer.busy();
  el.aiStep.disabled = !game || renderer.busy() || game.gameOver;
  el.overlayUndo.style.display = game && game.canUndo ? '' : 'none';
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
  }

  // 音效：有合并就放合并音（音高随最大合并块上升），否则放滑动音
  const mergedExponent = step.moves
    .filter((m) => m.merged)
    .reduce((acc, m) => Math.max(acc, m.exponent), 0);
  if (mergedExponent > 0) {
    sound.playMerge(mergedExponent + 1);
  } else {
    sound.play('slide');
  }
  if (step.spawned) sound.play('spawn');

  // 走到这里动画已经结束，renderer.busy() 为假，输入自然解锁
  checkEndState();
  refreshControls();
  return true;
}

async function doUndo() {
  if (renderer.busy()) return;
  if (!game || !game.undo()) return;

  sound.play('undo');
  hideOverlay();
  renderer.reset(game.board, { score: game.score, best });
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
  if (game.reached2048) {
    sound.play('win');
    showOverlay('达到 2048！', '可以继续玩，挑战更大的方块', { allowUndo: false, transient: true });
  }
}

function showOverlay(title, text, { allowUndo = false, transient = false } = {}) {
  el.overlayTitle.textContent = title;
  el.overlayText.textContent = text;
  el.overlayUndo.style.display = allowUndo ? '' : 'none';
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
}

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
  el.aiAuto.textContent = '停止';
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
  el.aiAuto.textContent = 'AI 自动';
  refreshControls();
}

// ------------------------------------------------------------------ 装配

function newGame() {
  stopAuto();
  hideOverlay();

  const forced = el.seedShown.dataset.forcedSeed;
  const choice = forced !== undefined ? forced : el.seed.value;
  delete el.seedShown.dataset.forcedSeed;
  const seed = choice === 'random' ? Math.floor(Math.random() * 1e9) : Number(choice);

  // 难度是**规则**的一部分，必须传给 Game —— 不传的话界面选了"简单"
  // 而生成仍然是全盘随机，用户会以为难度没生效（而且他无法分辨）。
  game = new Game(seed, currentDifficulty());
  lastMove = null;
  el.seedShown.textContent = `本局种子 ${seed}`;
  renderer.reset(game.board, { score: game.score, best });
  refreshDifficultyNotice();
  refreshControls();
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
  if (el.difficultyNote) {
    el.difficultyNote.textContent = DIFFICULTY[value] ? DIFFICULTY[value].note : '';
  }
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
  el.mute.textContent = muted ? '音效关' : '音效开';
  el.mute.setAttribute('aria-pressed', muted ? 'true' : 'false');
  if (!muted) sound.unlock();
}

function showNotice(text) {
  el.notice.textContent = text;
  el.notice.classList.remove('hidden');
}

// ------------------------------------------------------------------ 启动

async function boot() {
  el.mute.textContent = sound.muted ? '音效关' : '音效开';
  el.mute.setAttribute('aria-pressed', sound.muted ? 'true' : 'false');

  if (el.difficulty) fillSelect(el.difficulty, difficultyOptions(), DEFAULT_DIFFICULTY);
  fillSelect(el.strength, strengthOptions(), 'standard');
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

  el.newGame.addEventListener('click', () => newGame());
  el.undo.addEventListener('click', () => void doUndo());
  el.aiStep.addEventListener('click', () => void aiStep());
  el.aiAuto.addEventListener('click', () => (autoRunning ? stopAuto() : startAuto()));
  el.mute.addEventListener('click', () => toggleMute());
  el.overlayRestart.addEventListener('click', () => newGame());
  el.overlayUndo.addEventListener('click', () => void doUndo());
  el.seed.addEventListener('change', () => newGame());
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
  window.addEventListener('orientationchange', () => setTimeout(() => renderer.relayout(), 120));

  // 标签页切回来时排一次版：尺寸可能在后台变过
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) renderer.relayout();
  });

  // 供 Android 的 WebView 桥接使用（里程碑 6）。
  // 只暴露必要动作，不让外部拿到内部对象。
  window.AI2048 = {
    newGame: (seed) => {
      if (seed !== undefined) {
        el.seedShown.dataset.forcedSeed = String(seed);
      }
      newGame();
    },
    undo: () => void doUndo(),
    move: (direction) => void performMove(direction),
    aiStep: () => void aiStep(),
    isDegraded: () => transportDegraded,
  };
}

void boot();

export { performMove, newGame, DIRECTION };
