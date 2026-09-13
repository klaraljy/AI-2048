// input.js 的 headless 测试。
//
// 为什么专门测这个：
// 输入锁出过一个真实 bug —— 早期实现把"是否锁定"做成一个布尔量，
// 由外部在动作开始/结束时手动同步。结果动画结束后没人解锁，
// **第一次走子之后键盘就永久失效**。
//
// 修法是把锁改成**查询式**（isLocked 回调），所以这里重点验证：
//   1. isLocked 每次尝试都会被重新查询（不是一次性的快照）
//   2. 锁从 true 变回 false 之后，输入确实恢复
//   3. 键盘映射、修饰键、输入框焦点等边界行为
//
// 运行： node tests\input.test.mjs

import { installDomStub } from './dom-stub.mjs';

const dom = installDomStub();

const { Input, DIRECTION } = await import('../web/js/input.js');

let failures = 0;
function check(label, condition, detail = '') {
  if (condition) {
    console.log(`  ✓ ${label}`);
  } else {
    failures += 1;
    console.error(`  ✗ ${label}${detail ? `  —— ${detail}` : ''}`);
  }
}

/** 建一个 Input，记录收到的方向。
 *
 * 注意：installDomStub 之后 window 的 keydown 监听是全局累积的，
 * 每个 Input 实例都会往 keyListeners 里推一个 handler。
 * 所以这里先把已有的 handler 清掉，保证每个测试块只受**自己那个** Input 影响 ——
 * 否则上一块的 Input 也会响应，断言会看到重复的动作。 */
function makeInput(lockState = { locked: false }, { swipeBlocked = () => false } = {}) {
  const received = [];
  const input = new Input({
    onMove: (d) => received.push(d),
    isLocked: () => lockState.locked,
    isSwipeBlocked: swipeBlocked,
    onUnlock: () => {},
    onRestart: () => received.push('RESTART'),
    onUndo: () => received.push('UNDO'),
    onToggleMute: () => received.push('MUTE'),
  });
  return { input, received, lockState };
}

/** 派发一个 keydown 到 window 的监听器上。 */
function pressKey(key, { target } = {}) {
  const event = {
    key,
    target: target || { tagName: 'DIV' },
    preventDefault() {
      this.defaultPrevented = true;
    },
  };
  globalThis.window.dispatchKeydown(event);
  return event;
}

// installDomStub 里的 window 是普通对象，这里补上事件派发能力
const keyListeners = [];
globalThis.window.addEventListener = (type, handler) => {
  if (type === 'keydown') keyListeners.push(handler);
};
globalThis.window.dispatchKeydown = (event) => {
  for (const handler of keyListeners) handler(event);
};

// document 也要能派发：滑动监听挂在 **document** 上（全屏可滑），不在棋盘上。
// 为什么从棋盘搬到 document：手机上拇指常落在棋盘外，挂棋盘上那些拖动没反应。
const docListeners = new Map();
globalThis.document.addEventListener = (type, handler) => {
  if (!docListeners.has(type)) docListeners.set(type, []);
  docListeners.get(type).push(handler);
};
globalThis.document.dispatch = (type, event) => {
  for (const handler of docListeners.get(type) || []) handler(event);
};
// setPointerCapture 的目标
globalThis.document.documentElement = {
  setPointerCapture() {},
  releasePointerCapture() {},
};

/** 造一个"能被 closest 判定"的假元素。 */
function fakeEl({ button = false, modal = false } = {}) {
  return {
    tagName: 'DIV',
    closest(selector) {
      // 选择器里同时含 button 与 .modal，这里按标记各自命中
      if (button && selector.includes('button')) return this;
      if (modal && selector.includes('.modal')) return this;
      return null;
    },
  };
}

console.log('input.js 测试：');

// 1. 键位映射
{
  const { received } = makeInput();
  // 上一次构造已经把监听器推入 keyListeners 了
  pressKey('ArrowUp');
  pressKey('ArrowDown');
  pressKey('ArrowLeft');
  pressKey('ArrowRight');
  check('方向键映射正确', received.join(',') === 'up,down,left,right', received.join(','));

  received.length = 0;
  pressKey('w');
  pressKey('a');
  pressKey('s');
  pressKey('d');
  check('WASD 映射正确（大小写都认）', received.join(',') === 'up,left,down,right', received.join(','));

  received.length = 0;
  pressKey('W');
  pressKey('A');
  check('大写 WASD 也认', received.join(',') === 'up,left', received.join(','));
}

// 2. 非方向键
{
  const { received } = makeInput();
  pressKey('r');
  pressKey('u');
  pressKey('m');
  check('R/U/M 分别触发重开、撤销、静音', received.join(',') === 'RESTART,UNDO,MUTE', received.join(','));

  received.length = 0;
  pressKey('x');
  pressKey('1');
  pressKey('Backspace');
  check('无关键不产生动作（Backspace 归入撤销）', received.join(',') === 'UNDO', received.join(','));
}

// 3. 输入框里打字不该吃按键 —— 否则没法在输入框里输种子
{
  const { received } = makeInput();
  pressKey('ArrowUp', { target: { tagName: 'INPUT' } });
  pressKey('a', { target: { tagName: 'SELECT' } });
  check('INPUT/SELECT 获得焦点时不响应', received.length === 0, received.join(','));
}

// 4. preventDefault 行为
{
  makeInput();
  const arrow = pressKey('ArrowUp');
  const letter = pressKey('q');
  check('方向键调用 preventDefault（防止页面滚动）', arrow.defaultPrevented === true);
  check('无关键不调用 preventDefault', letter.defaultPrevented === undefined);
}

// 5. 核心：锁是查询式的，且能恢复
{
  const state = { locked: false };
  const { input, received } = makeInput(state);

  // 未锁定时可输入
  pressKey('ArrowUp');
  check('未锁定时输入生效', received.join(',') === 'up', received.join(','));

  // 上锁
  received.length = 0;
  state.locked = true;
  pressKey('ArrowUp');
  pressKey('ArrowLeft');
  check('锁定期间输入被丢弃', received.length === 0, received.join(','));

  // 解锁 —— 这是早期 bug 的现场：当时没有这一步，锁永久保持
  state.locked = false;
  pressKey('ArrowDown');
  check('解锁后输入恢复（早期 bug 就在这里）', received.join(',') === 'down', received.join(','));

  // 再锁再解，确认是可重复的
  state.locked = true;
  received.length = 0;
  pressKey('ArrowRight');
  check('二次锁定仍然丢弃输入', received.length === 0, received.join(','));
  state.locked = false;
  pressKey('ArrowRight');
  check('二次解锁仍然恢复', received.join(',') === 'right', received.join(','));

  // tryMove 直接调用也必须遵守锁
  state.locked = true;
  received.length = 0;
  check('tryMove 在锁定时返回 false', input.tryMove(DIRECTION.up) === false);
  check('tryMove 在锁定时不产生动作', received.length === 0);
  state.locked = false;
  check('tryMove 在解锁后返回 true', input.tryMove(DIRECTION.up) === true);
  check('tryMove 在解锁后产生动作', received.join(',') === 'up', received.join(','));
}

// 6. 滑动 —— 在**整页**任意位置都能触发走子（用户 2026-09-13 要求）
{
  const state = { locked: false };
  const { received } = makeInput(state);

  // 关键：事件派发到 document，target 是普通元素（模拟点在棋盘外）
  const swipe = (dx, dy, { target = fakeEl() } = {}) => {
    globalThis.document.dispatch('pointerdown', { pointerId: 1, clientX: 200, clientY: 200, target });
    globalThis.document.dispatch('pointerup', { pointerId: 1, clientX: 200 + dx, clientY: 200 + dy, target });
  };

  swipe(0, -60);
  swipe(0, 60);
  swipe(-60, 0);
  swipe(60, 0);
  check('四个方向的滑动都识别', received.join(',') === 'up,down,left,right', received.join(','));

  received.length = 0;
  swipe(50, 20); // 斜向：横向分量明显更大，主轴判定为右
  check('斜向滑动按主轴判定', received.join(',') === 'right', received.join(','));

  received.length = 0;
  globalThis.document.dispatch('pointerdown', { pointerId: 2, clientX: 200, clientY: 200, target: fakeEl() });
  globalThis.document.dispatch('pointerup', { pointerId: 2, clientX: 205, clientY: 203, target: fakeEl() });
  check('位移过小视为点击，不产生动作', received.length === 0, received.join(','));

  received.length = 0;
  state.locked = true;
  swipe(0, -60);
  check('锁定期间滑动也被丢弃', received.length === 0, received.join(','));
  state.locked = false;
}

// 7. 按钮 / 下拉框 / 弹窗上的拖动不算走子
//    否则点「重来」「撤回」会变成走子 —— 这是全屏滑动最容易引入的回归。
{
  const state = { locked: false };
  const { received } = makeInput(state);
  const swipeOn = (target) => {
    globalThis.document.dispatch('pointerdown', { pointerId: 1, clientX: 200, clientY: 200, target });
    globalThis.document.dispatch('pointerup', { pointerId: 1, clientX: 200, clientY: 260, target });
  };

  swipeOn(fakeEl({ button: true }));
  check('从按钮上起手的拖动不走子', received.length === 0, received.join(','));

  received.length = 0;
  swipeOn(fakeEl({ modal: true }));
  check('在弹窗内的拖动不走子', received.length === 0, received.join(','));

  received.length = 0;
  swipeOn(fakeEl());
  check('普通区域仍然能滑（排除项没有误伤全局）', received.join(',') === 'down', received.join(','));
}

// 8. isSwipeBlocked：能拖动但这次手势不算数（规则弹窗开着的场景）
{
  const state = { locked: false };
  let blocked = true;
  const { received } = makeInput(state, { swipeBlocked: () => blocked });
  const swipe = () => {
    globalThis.document.dispatch('pointerdown', { pointerId: 1, clientX: 200, clientY: 200, target: fakeEl() });
    globalThis.document.dispatch('pointerup', { pointerId: 1, clientX: 200, clientY: 260, target: fakeEl() });
  };

  swipe();
  check('被拦截时不走子', received.length === 0, received.join(','));

  blocked = false;
  swipe();
  check('解除拦截后恢复走子', received.join(',') === 'down', received.join(','));
}

// 9. 滑过阈值就立刻走子，**不等抬手**
//
// 用户反馈"触屏到操作就已经很慢了"。原来判定只在 pointerup 里做，
// 延迟 = 滑完阈值 + **抬手时间** + 动画时长，抬手那一下是纯白等
//（而且慢慢松手的人感觉特别慢）。现在 pointermove 越阈值即触发。
// 这里锁住两件事：① 越阈值立刻走子；② 随后的 pointerup **不能**再走一次。
{
  const state = { locked: false };
  const { received } = makeInput(state);

  const down = (x, y) =>
    globalThis.document.dispatch('pointerdown', { pointerId: 7, clientX: x, clientY: y, target: fakeEl() });
  const move = (x, y) =>
    globalThis.document.dispatch('pointermove', { pointerId: 7, clientX: x, clientY: y, target: fakeEl() });
  const up = (x, y) =>
    globalThis.document.dispatch('pointerup', { pointerId: 7, clientX: x, clientY: y, target: fakeEl() });

  // ① 指针还没抬起，只是滑过了阈值 → 应当已经走子
  down(200, 200);
  move(200, 180); // 向上滑 20px，超过 12px 阈值
  check('滑过阈值立刻走子（不等抬手）', received.join(',') === 'up', received.join(','));

  // ② 抬手不能重复触发
  received.length = 0;
  up(200, 160);
  check('随后抬手不重复走子（避免一步走两次）', received.length === 0, received.join(','));

  // ③ 位移不足阈值时不该触发
  received.length = 0;
  down(300, 300);
  move(304, 303); // 只有 4px
  check('位移不足阈值时 pointermove 不触发', received.length === 0, received.join(','));
  up(304, 303);
  check('位移不足阈值时抬手也不触发（当作点击）', received.length === 0, received.join(','));

  // ④ 一次大幅滑动只算一步（多次 pointermove 不重复触发）
  received.length = 0;
  down(200, 400);
  move(200, 380);
  move(200, 340);
  move(200, 300);
  up(200, 300);
  check('一次滑动只走一步', received.join(',') === 'up', received.join(','));
}

console.log('');
if (failures > 0) {
  console.error(`input.js 测试失败：${failures} 项`);
  process.exit(1);
}
console.log('input.js 测试通过');
dom.restore();
