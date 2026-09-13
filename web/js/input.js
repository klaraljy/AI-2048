// 输入：键盘 + 触屏/鼠标滑动 + 输入锁。
//
// 输入锁为什么必要：
// 参考原型是"按下 → 立即走子 → 立即重绘"，同步完成，所以没有竞态。
// 这里走子后要播动画（滑动 + 弹跳 + 出现，约 250ms），
// 期间如果再来一次走子，逻辑状态已经推进但画面还在演上一步 —— 会错位。
//
// 处理方式是**丢弃**动画期间的输入，而不是排队：
// 排队会让快速连按积累出一长串延迟动作，手感很差。

/** 方向名，与 game.js 的 DIRECTION 对应。 */
export const DIRECTION = {
  up: 'up',
  down: 'down',
  left: 'left',
  right: 'right',
};

const KEY_MAP = new Map([
  ['ArrowUp', DIRECTION.up],
  ['ArrowDown', DIRECTION.down],
  ['ArrowLeft', DIRECTION.left],
  ['ArrowRight', DIRECTION.right],
  ['w', DIRECTION.up],
  ['W', DIRECTION.up],
  ['s', DIRECTION.down],
  ['S', DIRECTION.down],
  ['a', DIRECTION.left],
  ['A', DIRECTION.left],
  ['d', DIRECTION.right],
  ['D', DIRECTION.right],
  ['k', DIRECTION.up],
  ['j', DIRECTION.down],
  ['h', DIRECTION.left],
  ['l', DIRECTION.right],
]);

/**
 * 滑动多远才算一次操作（像素）。
 *
 * ⚠️ 这个值直接决定**手感延迟**：手指要先走完这段距离才会触发走子，
 * 所以它是"一次操作总延迟"的三部分之一（另外两部分是浏览器的手势消歧、
 * 以及动画时长）。原来 24px，用户反馈"屏幕操作有一定的延迟"——
 * 拇指滑 12px 约 40~60ms，滑 24px 要 80~120ms。收到 12px。
 *
 * 不要再小：低于 10px 时手指按下时的轻微抖动会被误判成滑动，
 * 玩家"只是想点一下"就变成了走子。
 */
const SWIPE_THRESHOLD = 12;

/**
 * 这些区域内**不接管滑动**，保留它们本来的点击/选择行为。
 *
 * 用户 2026-09-13 要求"所有位置都能够触发方块的移动"，
 * 但按钮和下拉框必须排除 —— 否则点「重来」会变成走子。
 */
const SWIPE_EXCLUDED = 'button, select, input, textarea, a, .modal, .overlay';

/** 事件是否发生在不该接管滑动的元素上（点下去时判断一次即可）。 */
function isExcludedTarget(target) {
  if (!target || typeof target.closest !== 'function') return false;
  try {
    return target.closest(SWIPE_EXCLUDED) !== null;
  } catch {
    return false;
  }
}

export class Input {
  /**
   * @param {object} options
   * @param {(direction: string) => void} options.onMove
   * @param {() => boolean} [options.isLocked] 查询"现在是否应丢弃输入"。
   *   **必须是查询而不是快照** —— 早期实现用一个 locked 布尔量、由外部在
   *   动作前后手动同步，结果动画结束后没人解锁，第一次走子之后键盘就永久失效。
   * @param {() => void} [options.onUnlock] 首次用户手势时调用（用于解锁音频）
   * @param {() => void} [options.onRestart] R 键重开
   * @param {() => void} [options.onUndo] U 键 / Backspace 撤销
   * @param {() => void} [options.onToggleMute] M 键静音
   * @param {() => void} [options.onAiStep] 空格让 AI 走一步
   * @param {() => boolean} [options.isSwipeBlocked] 弹窗打开等"能拖但不该走子"的状态。
   *   与 isLocked 分开：isLocked 是"忙，丢掉输入"，这个是"这次手势不算数"。
   */
  constructor({
    onMove,
    isLocked,
    onUnlock,
    onRestart,
    onUndo,
    onToggleMute,
    onAiStep,
    isSwipeBlocked,
  }) {
    this.onMove = onMove;
    this.isLocked = isLocked || (() => false);
    this.onUnlock = onUnlock;
    this.onRestart = onRestart;
    this.onUndo = onUndo;
    this.onToggleMute = onToggleMute;
    this.onAiStep = onAiStep;
    // 弹窗打开之类的情况：手指仍可拖动，但不该走子
    this.isSwipeBlocked = isSwipeBlocked || (() => false);

    this._pointerId = null;
    this._startX = 0;
    this._startY = 0;
    this._unlocked = false;

    this._bind();
  }

  _fireUnlock() {
    if (this._unlocked) return;
    this._unlocked = true;
    if (this.onUnlock) this.onUnlock();
  }

  /** 尝试执行一个方向。当前被锁定时返回 false。 */
  tryMove(direction) {
    if (this.isLocked()) return false;
    this.onMove(direction);
    return true;
  }

  _bind() {
    window.addEventListener('keydown', (event) => {
      // 输入框里打字时不要吃按键
      const target = event.target;
      if (target && (target.tagName === 'INPUT' || target.tagName === 'SELECT')) return;

      this._fireUnlock();

      const direction = KEY_MAP.get(event.key);
      if (direction) {
        event.preventDefault();
        this.tryMove(direction);
        return;
      }
      if (event.key === 'r' || event.key === 'R') {
        event.preventDefault();
        if (this.onRestart) this.onRestart();
        return;
      }
      if (event.key === 'u' || event.key === 'U' || event.key === 'Backspace') {
        event.preventDefault();
        if (this.onUndo) this.onUndo();
        return;
      }
      if (event.key === 'm' || event.key === 'M') {
        event.preventDefault();
        if (this.onToggleMute) this.onToggleMute();
        return;
      }
      // 空格 = 让 AI 走一步。
      // 用 event.code 判断而不是 event.key：空格键的 key 值是 ' '，
      // 不同输入法/键盘布局下更容易出岔子，code 稳定得多。
      if (event.code === 'Space' || event.key === ' ') {
        event.preventDefault();
        if (this.onAiStep) this.onAiStep();
      }
    });

    // 指针事件同时覆盖触屏、笔和鼠标拖拽。
    //
    // ⚠️ 监听挂在 **document** 上，不是棋盘上（用户 2026-09-13 要求：
    // "所有位置都能够触发方块的移动"）。挂在棋盘上时，只有在棋盘里起手的
    // 拖动才算数 —— 手机上一根拇指常常落在棋盘外，用户的感觉就是"没反应"。
    //
    // 排除按钮/下拉框/弹窗：否则点「重来」会被当成走子。
    // 开始拖动后把指针捕获到 documentElement，手指滑到哪儿事件都还能收到。
    document.addEventListener('pointerdown', (event) => {
      this._fireUnlock();
      if (this._pointerId !== null) return; // 已经在跟一个指针了
      if (isExcludedTarget(event.target)) return; // 按钮/下拉框/弹窗：不接管
      this._pointerId = event.pointerId;
      this._startX = event.clientX;
      this._startY = event.clientY;
      const capture = document.documentElement;
      if (capture && capture.setPointerCapture) {
        try {
          capture.setPointerCapture(event.pointerId);
        } catch {
          // 某些浏览器在特定情况下会抛，忽略即可
        }
      }
    });

    const finish = (event) => {
      if (this._pointerId === null || event.pointerId !== this._pointerId) return;
      this._pointerId = null;

      const dx = event.clientX - this._startX;
      const dy = event.clientY - this._startY;
      const absX = Math.abs(dx);
      const absY = Math.abs(dy);

      if (Math.max(absX, absY) < SWIPE_THRESHOLD) return; // 当作点击，不算滑动
      if (this.isSwipeBlocked()) return; // 弹窗开着之类

      // 主轴方向决定走子方向
      const direction =
        absX > absY ? (dx > 0 ? DIRECTION.right : DIRECTION.left) : dy > 0 ? DIRECTION.down : DIRECTION.up;
      this.tryMove(direction);
    };

    document.addEventListener('pointerup', finish);
    document.addEventListener('pointercancel', (event) => {
      if (event.pointerId === this._pointerId) this._pointerId = null;
    });

    // 触屏上拖动时阻止页面跟着滚 —— 整屏滑动没有这一步的话，
    // 页面会一边滚一边走子。只在确实跟随着一个指针时拦截，不影响正常滚动。
    document.addEventListener(
      'touchmove',
      (event) => {
        if (this._pointerId !== null) event.preventDefault();
      },
      { passive: false }
    );
  }
}
