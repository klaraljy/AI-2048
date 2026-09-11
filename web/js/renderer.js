// 棋盘渲染 + 动画。
//
// 设计要点（与参考原型的关键区别）：
//
//   参考原型每步 `innerHTML = ""` 重建 16 个节点 —— 方块没有身份，
//   也就没有"上一帧位置"，**滑动动画在那层结构下不可能实现**。
//
//   这里是相反的：
//     - 每个方块有持久 id 和自己的 DOM 节点
//     - 方块绝对定位，靠 CSS transform 移动 → 有过渡就有平滑滑动
//     - 一次走子按顺序演：滑动 → 合并弹跳 → 消掉被吸收的块 → 新块缩放出现
//
// 输入锁：动画期间 `busy()` 为真，调用方应丢弃输入而不是排队 ——
// 排队会让快速连按积累出一长串延迟动作，手感很差。

const SIZE = 4;

/** 动画时长（毫秒）。必须与 css/style.css 里的 --slide-ms 等保持一致。 */
const TIMING = {
  slide: 110,
  mergePop: 170,
  appear: 160,
};

function prefersReducedMotion() {
  return (
    typeof window !== 'undefined' &&
    window.matchMedia &&
    window.matchMedia('(prefers-reduced-motion: reduce)').matches
  );
}

function sleep(ms) {
  if (ms <= 0) return Promise.resolve();
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** 把数值转成配色/字号用的档位。超过上限就复用最后一档。 */
function tileClass(value) {
  const max = 1 << 20;
  const capped = value > max ? max : value;
  return `t-${capped}`;
}

function fontClass(value) {
  const digits = String(value).length;
  if (digits <= 2) return 'f-2';
  if (digits === 3) return 'f-3';
  if (digits === 4) return 'f-4';
  if (digits === 5) return 'f-5';
  if (digits === 6) return 'f-6';
  return 'f-7';
}

export class Renderer {
  /**
   * @param {object} elements
   * @param {HTMLElement} elements.tilesLayer 放方块的绝对定位层
   * @param {HTMLElement} elements.gridLayer 空槽背景层
   * @param {HTMLElement} elements.scoreElement
   * @param {HTMLElement} elements.bestElement
   */
  constructor({ tilesLayer, gridLayer, scoreElement, bestElement }) {
    this.tilesLayer = tilesLayer;
    this.gridLayer = gridLayer;
    this.scoreElement = scoreElement;
    this.bestElement = bestElement;

    /** @type {Map<number, HTMLElement>} 方块 id -> DOM 节点 */
    this.nodes = new Map();
    /** @type {Map<number, {row: number, col: number, exponent: number}>} 方块 id -> 逻辑状态 */
    this.tiles = new Map();
    this.nextId = 1;
    this._busy = false;
    this._scoreShown = 0;

    this._buildGrid();
  }

  _buildGrid() {
    this.gridLayer.innerHTML = '';
    for (let i = 0; i < SIZE * SIZE; i++) {
      const cell = document.createElement('div');
      cell.className = 'grid-cell';
      this.gridLayer.appendChild(cell);
    }
  }

  /**
   * 方块位置：按容器百分比计算，这样棋盘随窗口缩放时不用重算。
   * gap 与 padding 都在 .board-wrap 上，.tiles 层已经避开 padding，
   * 所以这里只用容器宽度与 gap 推。
   */
  _position(row, col) {
    // CSS 里 gap 会随窄屏变，用计算样式读实际值
    const style = getComputedStyle(this.tilesLayer);
    const gap = parseFloat(style.getPropertyValue('--gap')) || 12;
    const width = this.tilesLayer.clientWidth || 1;
    const cell = (width - gap * (SIZE - 1)) / SIZE;
    const x = col * (cell + gap);
    const y = row * (cell + gap);
    return { x, y, cell };
  }

  _applyTransform(node, row, col, animate) {
    const { x, y, cell } = this._position(row, col);
    node.style.width = `${cell}px`;
    node.style.height = `${cell}px`;
    const transform = `translate3d(${x}px, ${y}px, 0)`;
    // --tile-pos 供 keyframes 使用（缩放动画必须叠加在位移之上，
    // 否则动画会把方块拉回原点）
    node.style.setProperty('--tile-pos', transform);
    if (animate === false) {
      const previous = node.style.transition;
      node.style.transition = 'none';
      node.style.transform = transform;
      // 强制回流让 transition:none 生效，否则下一次改动会被合并掉
      void node.offsetWidth;
      node.style.transition = previous;
    } else {
      node.style.transform = transform;
    }
  }

  /** 动画是否在进行中。为真时调用方应丢弃输入。 */
  busy() {
    return this._busy;
  }

  /** 按逻辑棋盘全量重建（新游戏 / 撤销 / 首次渲染）。 */
  reset(board, { score = 0, best = 0 } = {}) {
    this.tilesLayer.innerHTML = '';
    this.nodes.clear();
    this.tiles.clear();
    this.nextId = 1;

    for (let row = 0; row < SIZE; row++) {
      for (let col = 0; col < SIZE; col++) {
        const exponent = board[row][col];
        if (exponent === 0) continue;
        this._createTile(row, col, exponent, { animate: false });
      }
    }
    this.setScore(score, { animate: false });
    this.setBest(best);
  }

  _createTile(row, col, exponent, { animate }) {
    const id = this.nextId++;
    const node = document.createElement('div');
    const value = 2 ** exponent;
    node.className = `tile ${tileClass(value)} ${fontClass(value)}`;
    // CSS 按 data-value 上色、按 data-len 调字号。
    // 用属性而不是类名，是因为配色表有十几档、字号还有动态变化，
    // 类名会变成 `t-2048 f-4` 这种两份信息，属性更直白也好在 DevTools 里看。
    node.dataset.value = String(value);
    node.dataset.len = String(String(value).length);
    node.textContent = String(value);
    this.tilesLayer.appendChild(node);

    this._applyTransform(node, row, col, animate);
    if (animate) {
      node.classList.add('new');
      setTimeout(() => node.classList.remove('new'), TIMING.appear + 40);
    }

    this.nodes.set(id, node);
    this.tiles.set(id, { row, col, exponent });
    return id;
  }

  /**
   * 演示一次走子。
   *
   * @param {object} step Game.step() 的返回值
   * @param {number[][]} beforeBoard 走子前的指数棋盘
   * @param {number} scoreAfter 走子并生成新块之后的分数
   * @param {number} bestAfter
   * @returns {Promise<void>} 动画结束时 resolve
   */
  async animateMove(step, beforeBoard, scoreAfter, bestAfter) {
    this._busy = true;
    const reduced = prefersReducedMotion();
    const slideMs = reduced ? 0 : TIMING.slide;

    // 建立"位置 -> 方块 id"的索引，把轨迹里的坐标映射到具体方块。
    // 轨迹给的是坐标而不是 id，因为引擎侧也没有 id 概念 ——
    // id 是纯前端的表现层概念。
    const idAt = new Map();
    for (const [id, tile] of this.tiles) {
      idAt.set(`${tile.row},${tile.col}`, id);
    }

    /** @type {{id:number, exponent:number}[]} 需要在滑动结束后删掉的被吸收块 */
    const absorbed = [];
    /** @type {{id:number, exponent:number}[]} 合并后需要改变数值和弹跳的块 */
    const mergedTargets = [];

    // 第一步：把所有会动的块移到目标位置
    for (const move of step.moves) {
      const fromKey = `${move.fromRow},${move.fromCol}`;
      const id = idAt.get(fromKey);
      if (id === undefined) continue;
      // 一个源位置只会被消费一次
      idAt.delete(fromKey);

      const node = this.nodes.get(id);
      if (!node) continue;

      this._applyTransform(node, move.toRow, move.toCol, true);
      const tile = this.tiles.get(id);
      if (tile) {
        tile.row = move.toRow;
        tile.col = move.toCol;
      }

      if (move.merged) {
        absorbed.push({ id });
      } else {
        // 存活的那块：如果它这一位发生了合并，数值会变
        const mergedHere = step.moves.some(
          (m) => m.merged && m.toRow === move.toRow && m.toCol === move.toCol
        );
        if (mergedHere) mergedTargets.push({ id, exponent: move.exponent });
      }
    }

    await sleep(slideMs);

    // 第二步：合并的块换值并弹跳；被吸收的块消失
    for (const { id, exponent } of mergedTargets) {
      const node = this.nodes.get(id);
      if (!node) continue;
      const value = 2 ** exponent;
      node.className = `tile ${tileClass(value)} ${fontClass(value)} merged`;
      node.dataset.value = String(value);
      node.dataset.len = String(String(value).length);
      node.textContent = String(value);
      const tile = this.tiles.get(id);
      if (tile) tile.exponent = exponent;
      setTimeout(() => node.classList.remove('merged'), TIMING.mergePop + 40);
    }

    for (const { id } of absorbed) {
      const node = this.nodes.get(id);
      if (node) node.remove();
      this.nodes.delete(id);
      this.tiles.delete(id);
    }

    // 第三步：新生成的方块
    if (step.spawned && step.spawn) {
      this._createTile(step.spawn.row, step.spawn.col, step.spawn.exponent, { animate: true });
    }

    this.setScore(scoreAfter, { animate: step.gained > 0, gained: step.gained });
    this.setBest(bestAfter);

    await sleep(reduced ? 0 : Math.max(TIMING.mergePop, TIMING.appear));
    this._busy = false;
  }

  /**
   * 分数以**滚轮**方式变化：新数字从下方推上来（变大），或从上方压下来（变小）。
   *
   * 为什么不用数字逐位滚动：2048 的分数一次能涨几千，逐位滚会看不清也来不及。
   * 整块数字滚动既保留"在变"的动感，又不会糊。
   *
   * @param {number} score 目标分数
   * @param {object} [options]
   * @param {boolean} [options.animate] 是否播滚动动画
   * @param {number} [options.gained] 本次得分，用于飘出 +N
   */
  setScore(score, { animate = false, gained = 0 } = {}) {
    const previous = this._scoreShown;
    const roll = this.scoreElement ? this.scoreElement.parentElement : null;

    if (this.scoreElement) {
      if (animate && roll && score !== previous) {
        this._rollNumber(roll, this.scoreElement, score, score > previous ? 'up' : 'down');
      } else {
        this.scoreElement.textContent = String(score);
      }
    }

    if (animate && gained > 0 && roll) {
      const float = document.createElement('span');
      float.className = 'score-float';
      float.textContent = `+${gained}`;
      roll.appendChild(float);
      setTimeout(() => float.remove(), 900);
    }
    this._scoreShown = score;
  }

  /** 最高分同样用滚轮，但**只在真正变化时**才动（平时不该有动静）。 */
  setBest(best) {
    if (!this.bestElement) return;
    const roll = this.bestElement.parentElement;
    const previous = Number(this.bestElement.textContent) || 0;
    if (roll && best !== previous && previous > 0) {
      this._rollNumber(roll, this.bestElement, best, best > previous ? 'up' : 'down');
      return;
    }
    if (best !== previous) this.bestElement.textContent = String(best);
  }

  /**
   * 把数字"滚"到新值。
   *
   * 做法：在容器里临时插一个上一帧的数字，让两个数字一起平移 ——
   * 旧的移出、新的移入。动画结束把临时的那个删掉，容器里只剩最终数字。
   *
   * @param {HTMLElement} roll 有 overflow:hidden 的容器
   * @param {HTMLElement} target 真正承载数字的元素
   * @param {number} value 新值
   * @param {'up'|'down'} direction up = 新数字从下往上推入
   */
  _rollNumber(roll, target, value, direction) {
    const reduced =
      typeof window !== 'undefined' &&
      typeof window.matchMedia === 'function' &&
      window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (reduced) {
      target.textContent = String(value);
      return;
    }

    // 上一帧的数字：临时的，动画结束后删掉
    const outgoing = document.createElement('strong');
    outgoing.className = 'score-value score-value-out';
    outgoing.textContent = target.textContent;

    const distance = direction === 'up' ? 100 : -100;
    outgoing.style.transform = `translateY(${-distance}%)`;
    target.textContent = String(value);
    target.style.transform = `translateY(${distance}%)`;

    roll.appendChild(outgoing);

    // 强制一次重排，保证起点被浏览器采纳，否则两个 transform 会合并掉
    void roll.offsetHeight;

    const transition = 'transform 260ms cubic-bezier(0.22, 0.61, 0.36, 1)';
    outgoing.style.transition = transition;
    target.style.transition = transition;
    outgoing.style.transform = 'translateY(0)';
    target.style.transform = 'translateY(0)';

    setTimeout(() => {
      outgoing.remove();
      target.style.transition = '';
      target.style.transform = '';
      // 一次性脉冲，让变化更醒目
      target.classList.add('pop');
      setTimeout(() => target.classList.remove('pop'), 320);
    }, 300);
  }

  /** 窗口尺寸变化后重排所有方块（无动画）—— 百分比方案下仍需重算像素。 */
  relayout() {
    for (const [id, tile] of this.tiles) {
      const node = this.nodes.get(id);
      if (node) this._applyTransform(node, tile.row, tile.col, false);
    }
  }
}

export { TIMING };
