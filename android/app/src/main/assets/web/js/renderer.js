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
   * 演示一次**撤销**。
   *
   * 为什么需要它：撤销原来是 `reset(game.board)` —— 整盘清空重画，方块直接
   * **闪现**到原位，和走子时的滑动动画完全不是一个观感（用户 2026-09-13 报的）。
   *
   * 做法：不去改游戏逻辑（`Game` 的历史里只有棋盘快照，没有轨迹），
   * 直接拿**两份棋盘**在渲染层反推：
   *   - 撤销前棋盘（当前屏幕上这些方块）
   *   - 撤销后棋盘（`history` 里那份 = 走子**之前**的状态）
   *
   * 逐格对照就能知道每一块该去哪：
   *   1. 先删掉"当前有、目标没有"的块 —— 那些是走子时**新生成**的，
   *      撤销时让它淡出（不是直接闪掉）。
   *   2. 剩下的块按**数值相同 + 就近**配对到目标格：配对成功就滑过去。
   *   3. 目标格上还没人认领的，说明那里原来有一块被**合并**掉了 ——
   *      原地新建一块，从透明渐显。
   *   4. 配不上目标格的块：滑到就近空格（只可能出现在"好几块合成一块"的情况，
   *      视觉上一闪而过）。
   *
   * ⚠️ 已知的近似：两个相同数值合成一块后，**无法分辨哪一块是从哪边来的**
   * （数据里没这个信息），所以按位置就近分配。两个块长得一样，看不出来。
   *
   * @param {number[][]} previousBoard 撤销**后**的指数棋盘
   * @param {number} scoreBefore 撤销**前**的分数
   * @param {number} scoreAfter 撤销后的分数
   * @param {number} bestAfter
   * @returns {Promise<void>} 动画结束时 resolve
   */
  async animateUndo(previousBoard, scoreBefore, scoreAfter, bestAfter) {
    this._busy = true;
    const reduced = prefersReducedMotion();
    const slideMs = reduced ? 0 : TIMING.slide;
    const exitMs = reduced ? 0 : 140;

    /** 当前屏幕上的方块 */
    const current = [];
    for (const [id, tile] of this.tiles) {
      const node = this.nodes.get(id);
      if (node) current.push({ id, node, row: tile.row, col: tile.col, exponent: tile.exponent });
    }

    // ---- 第一步：先锁定"原位同值"的块，它们不动 ----
    // 必须先做这一步。否则下面的就近配对会把留在原地的块也算进去，
    // 反而让"走子时新生成的那块"匹配不上目标格、于是删不掉 ——
    // 实测出现过"撤销后棋盘从 3 块变成 5 块"。
    const claimed = new Set();
    const claimedTargets = new Set();
    const targets = [];
    for (let row = 0; row < SIZE; row++) {
      for (let col = 0; col < SIZE; col++) {
        const exponent = previousBoard[row][col];
        if (exponent === 0) continue;
        const target = { row, col, exponent };
        targets.push(target);
        const staying = current.find(
          (c) => !claimed.has(c.id) && c.row === row && c.col === col && c.exponent === exponent
        );
        if (staying) {
          claimed.add(staying.id);
          claimedTargets.add(target);
        }
      }
    }

    // ---- 第二步：剩下的当前块 → 剩下的目标格，全局最小总距离配对 ----
    // 按距离从小到大贪心（不是逐格找最近），避免"某块被就近抢占后，
    // 另一块被迫走很远、甚至配不上"。
    const freeTiles = current.filter((c) => !claimed.has(c.id));
    const freeTargets = targets.filter((t) => !claimedTargets.has(t));

    const candidatePairs = [];
    for (const tile of freeTiles) {
      for (const target of freeTargets) {
        if (tile.exponent !== target.exponent) continue;
        candidatePairs.push({
          tile,
          target,
          distance: Math.abs(tile.row - target.row) + Math.abs(tile.col - target.col),
        });
      }
    }
    candidatePairs.sort((a, b) => a.distance - b.distance);

    const pairs = [];
    for (const { tile, target } of candidatePairs) {
      if (claimed.has(tile.id) || claimedTargets.has(target)) continue;
      claimed.add(tile.id);
      claimedTargets.add(target);
      pairs.push({ from: tile, to: target });
    }

    // ---- 第三步：配对结果分三类处理 ----
    // a) 没配上目标格的当前块：只可能是"走子时新生成的那块" → 淡出
    const leaving = current.filter((c) => !claimed.has(c.id));
    for (const c of leaving) c.node.classList.add('undo-out');

    // b) 目标格上没人认领的：那里原来有一块被**合并**掉了 → 原地渐显
    const appearing = [];
    for (const target of freeTargets) {
      if (claimedTargets.has(target)) continue;
      const id = this._createTile(target.row, target.col, target.exponent, { animate: false });
      const node = this.nodes.get(id);
      if (node) {
        node.classList.add('undo-appear');
        appearing.push(node);
      }
    }
    // 强制一次重排：让浏览器**先把"透明"这个状态画出来**。
    // 少了这一步，插入节点与挂类可能被合进同一帧，opacity 过渡从"已是 1"
    // 开始算，方块会直接跳出来而不是渐显。
    if (appearing.length > 0) void this.tilesLayer.offsetWidth;

    // c) 要滑动的块（原位同值的不进这里，省掉一次无意义的 transform 写入）
    const moving = pairs.filter((p) => p.from.row !== p.to.row || p.from.col !== p.to.col);

    // 先播"淡出"，这一段里**数值不变**。
    // ⚠️ 数值回退不能放在这里（放这里会"一闪一闪"）：合并成的那块当场从 4
    // 变回 2，而要走的那半块还亮着 140ms，画面上就是先闪一下。
    // 正确做法是下面和滑动同一帧改值 —— 视觉上就是"滑回来 + 分开"。
    await sleep(exitMs);

    // 渐显的块这时候才摘掉 .undo-appear：它们在上一段里保持透明，
    // 现在与整盘滑动一起浮现。
    for (const node of appearing) node.classList.remove('undo-appear');

    for (const c of leaving) {
      c.node.remove();
      this.nodes.delete(c.id);
      this.tiles.delete(c.id);
    }

    // 数值回退 + 位移，同一帧发生
    for (const { from, to } of moving) {
      if (from.exponent !== to.exponent) {
        this._setTileValue(from.node, from.id, to.exponent);
      }
      this._applyTransform(from.node, to.row, to.col, true);
      const tile = this.tiles.get(from.id);
      if (tile) {
        tile.row = to.row;
        tile.col = to.col;
      }
    }

    this.setScore(scoreAfter, { animate: scoreAfter !== scoreBefore });
    this.setBest(bestAfter);

    await sleep(slideMs);

    this._busy = false;
  }

  /** 把一块的数值改掉（文字、配色、data 属性一起改）。 */
  _setTileValue(node, id, exponent) {
    const value = 2 ** exponent;
    node.className = `tile ${tileClass(value)} ${fontClass(value)}`;
    node.dataset.value = String(value);
    node.dataset.len = String(String(value).length);
    node.textContent = String(value);
    const tile = this.tiles.get(id);
    if (tile) tile.exponent = exponent;
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
