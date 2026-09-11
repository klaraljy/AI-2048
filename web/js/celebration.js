// 棋盘两侧的庆祝烟花。
//
// 为什么不用弹窗：达到 2048 时弹遮罩会把 AI 演示打断 —— 用户明确说
// "走一下弹一下"没法测。改成在棋盘**左右两侧**放小烟花，
// 视觉上庆祝但不挡任何东西、不拦任何操作。
//
// 合并出更高的方块时也让新数字飞出来炸开（纯数字，没有框）。
//
// 实现要点：
//   - 两块 canvas 分别贴在棋盘左右，尺寸跟着棋盘高度走
//   - 粒子用简单的速度 + 重力 + 透明度衰减，几十个粒子的量级足够好看
//   - **只在有粒子时才跑 requestAnimationFrame**，闲时不占 CPU
//     （AI 演示时每一步都可能有烟花，但两步之间必须让出主线程）
//   - 尊重 prefers-reduced-motion：直接不放粒子

const MAX_PARTICLES = 220;

/** 一次爆炸的配色。偏暖，和棋盘的暖色背景协调。 */
const PALETTE = ['#ffd166', '#f9a03f', '#ff8fa3', '#fefcf7', '#52bdab', '#f5c451'];

function prefersReducedMotion() {
  return (
    typeof window !== 'undefined' &&
    typeof window.matchMedia === 'function' &&
    window.matchMedia('(prefers-reduced-motion: reduce)').matches
  );
}

/**
 * 一块庆祝轨道。两侧各一个。
 *
 * 坐标用 CSS 像素，canvas 按 devicePixelRatio 放大，保证高分屏不糊。
 */
class FireworksLane {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas ? canvas.getContext('2d') : null;
    this.particles = [];
    this.raf = null;
    this.lastTime = 0;
    this.width = 0;
    this.height = 0;

    if (this.canvas) this.resize();
  }

  /** 跟随元素尺寸调整画布分辨率。 */
  resize() {
    if (!this.canvas || !this.ctx) return;
    const rect = this.canvas.getBoundingClientRect();
    const ratio = Math.min(2, (typeof window !== 'undefined' && window.devicePixelRatio) || 1);
    this.width = Math.max(1, rect.width);
    this.height = Math.max(1, rect.height);
    this.canvas.width = Math.round(this.width * ratio);
    this.canvas.height = Math.round(this.height * ratio);
    this.ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  }

  /**
   * 放一次烟花。
   * @param {number} y 相对画布高度的比例（0 = 顶部，1 = 底部）
   * @param {number} scale 规模倍数，合并出更大的块时调大
   */
  burst(y = 0.45, scale = 1) {
    if (!this.ctx || prefersReducedMotion()) return;

    const count = Math.round(Math.min(46, 20 * scale));
    const cx = this.width * (0.25 + Math.random() * 0.5);
    const cy = this.height * Math.min(0.92, Math.max(0.08, y));

    for (let i = 0; i < count; i++) {
      const angle = (Math.PI * 2 * i) / count + Math.random() * 0.3;
      const speed = (28 + Math.random() * 46) * scale;
      this.particles.push({
        x: cx,
        y: cy,
        vx: Math.cos(angle) * speed,
        vy: Math.sin(angle) * speed - 14,
        life: 1,
        decay: 0.9 + Math.random() * 0.7,
        size: 1.4 + Math.random() * 1.8,
        color: PALETTE[Math.floor(Math.random() * PALETTE.length)],
      });
    }
    if (this.particles.length > MAX_PARTICLES) {
      this.particles.splice(0, this.particles.length - MAX_PARTICLES);
    }
    this._start();
  }

  _start() {
    if (this.raf !== null) return;
    this.lastTime = 0;
    const step = (time) => {
      const dt = this.lastTime === 0 ? 16 : Math.min(48, time - this.lastTime);
      this.lastTime = time;

      const ctx = this.ctx;
      ctx.clearRect(0, 0, this.width, this.height);

      const gravity = 210; // px/s²
      const alive = [];
      for (const p of this.particles) {
        const seconds = dt / 1000;
        p.vy += gravity * seconds;
        p.x += p.vx * seconds;
        p.y += p.vy * seconds;
        p.life -= p.decay * seconds;
        if (p.life <= 0) continue;

        ctx.globalAlpha = Math.max(0, Math.min(1, p.life));
        ctx.fillStyle = p.color;
        // 方块粒子，和 2048 的方块呼应；旋转不必要，成本还高
        ctx.fillRect(p.x - p.size, p.y - p.size, p.size * 2, p.size * 2);
        alive.push(p);
      }
      ctx.globalAlpha = 1;
      this.particles = alive;

      if (this.particles.length === 0) {
        ctx.clearRect(0, 0, this.width, this.height);
        this.raf = null;
        return;
      }
      this.raf = requestAnimationFrame(step);
    };
    this.raf = requestAnimationFrame(step);
  }

  destroy() {
    if (this.raf !== null) {
      cancelAnimationFrame(this.raf);
      this.raf = null;
    }
    this.particles = [];
  }
}

/**
 * 把 count 个水平位置均匀分散在棋盘中部，带少量抖动。
 *
 * 纯随机会有较大概率两个数字挨得很近、叠在一起看不清，
 * 所以用"均分 + 抖动"而不是纯随机。
 *
 * @returns {number[]} 百分比位置，长度 count
 */
function spreadSlots(count) {
  const slots = [];
  for (let i = 0; i < count; i++) {
    const base = ((i + 0.5) / count) * 100; // 均分
    const jitter = (Math.random() - 0.5) * (40 / count); // 抖动幅度随个数收窄
    slots.push(Math.min(88, Math.max(12, base + jitter)));
  }
  // 打乱顺序，避免总是"从左往右依次出现"
  for (let i = slots.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [slots[i], slots[j]] = [slots[j], slots[i]];
  }
  return slots;
}

/**
 * 两侧烟花 + 数字弹出。
 *
 * 数字只用 DOM 元素，跟着烟花一起出现、淡出后自己删掉 ——
 * **没有背景框**，就是纯数字。
 */
export class Celebration {
  /**
   * @param {object} options
   * @param {HTMLCanvasElement} options.leftCanvas
   * @param {HTMLCanvasElement} options.rightCanvas
   * @param {HTMLElement} options.host 数字弹层的容器（一般是棋盘容器）
   */
  constructor({ leftCanvas, rightCanvas, host }) {
    this.left = new FireworksLane(leftCanvas);
    this.right = new FireworksLane(rightCanvas);
    this.host = host;
  }

  relayout() {
    this.left.resize();
    this.right.resize();
  }

  /**
   * 庆祝一次。
   *
   * @param {string} [text] 要弹出来的数字（纯数字，无框）
   * @param {number} [scale] 规模：块越大越隆重
   * @param {number} [count] 弹几个数字 —— 用户要求弹 3 个
   */
  celebrate(text = '', scale = 1, count = 3) {
    if (prefersReducedMotion()) return;

    // 两侧错开一点，别像镜像
    const y = 0.32 + Math.random() * 0.25;
    this.left.burst(y, scale);
    setTimeout(() => this.right.burst(0.3 + Math.random() * 0.3, scale), 90);

    if (!text || !this.host) return;

    // 弹多个：位置分散 + 时间错开，否则会叠在一起看不清
    const slots = spreadSlots(count);
    for (let i = 0; i < count; i++) {
      const delay = i * 120;
      if (delay === 0) {
        this._floatNumber(text, scale, slots[i]);
      } else {
        setTimeout(() => this._floatNumber(text, scale, slots[i]), delay);
      }
    }
  }

  /**
   * 把窗口内水平位置分散开，避免多个数字重叠。
   *
   * 均分 + 少量抖动：纯随机有较大概率两个挨得很近。
   */
  _floatNumber(text, scale, leftPercent) {
    const node = document.createElement('div');
    node.className = 'celebration-number';
    node.textContent = text;
    node.style.setProperty('--x', `${leftPercent}%`);
    node.style.setProperty('--scale', String(Math.min(1.9, 1 + (scale - 1) * 0.5)));
    // 纵向也错开，避免同一高度排成一行
    node.style.setProperty('--y', `${18 + Math.random() * 26}%`);
    this.host.appendChild(node);
    setTimeout(() => node.remove(), 1300);
  }

  destroy() {
    this.left.destroy();
    this.right.destroy();
  }
}
