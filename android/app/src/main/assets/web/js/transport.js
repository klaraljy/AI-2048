// 传输层：前端与 AI 之间**唯一**允许知道"怎么拿到决策"的文件。
//
// 为什么要有这一层：
//   1. AGENTS.md 规定的前端硬要求 —— 引擎连不上时必须能降级运行，
//      不能白屏。降级就意味着"决策来源"可以换。
//   2. 同一份前端要同时服务桌面（WebSocket 连 C++ 引擎）和 Android
//      （如果走 WebView，就是 JS bridge）。UI 与主流程不该关心这个差别。
//
// 实现的接口：
//   configure(config) -> Promise<void>
//   bestMove(board, options) -> Promise<{move: string|null, debugInfo: object|null}>
//   evaluateRootDirection(board, direction, options) -> Promise<object>   // 可选
//
// board 是数值棋盘（number[][]，0 表示空），不是指数棋盘 ——
// UI 层不该知道内部编码。

import { applyMove, DIRECTION, hasLegalMove } from './game.js';

/** 四个方向的固定顺序。降级实现与引擎都用它，保证行为一致。 */
const DIRECTIONS = [DIRECTION.up, DIRECTION.down, DIRECTION.left, DIRECTION.right];

/**
 * 降级后端：纯 JS 的贪心启发式。
 *
 * ⚠️ 它**不强**，也不打算强。C++ 引擎的搜索在 depth 8 下有 46,851 的平均分，
 * 而这个贪心只是"在四个合法方向里挑当前形状最好的那个"，大约是几千分的水平。
 *
 * 它的职责是让页面在引擎缺席时**仍然能玩、能演示**，而不是替代引擎。
 * 所以界面上必须明确显示"当前用的是降级 AI"，否则用户会以为 AI 就是这么弱。
 */
export class LocalTransport {
  constructor() {
    this.name = 'local';
    this.description = '本地降级 AI（贪心启发式，非引擎）';
  }

  async configure() {
    // 无配置项
  }

  async bestMove(board) {
    const best = this._chooseGreedy(board);
    return { move: best, debugInfo: null };
  }

  _chooseGreedy(board) {
    let bestMove = null;
    let bestScore = -Infinity;
    for (const direction of DIRECTIONS) {
      const result = applyMove(toExponents(board), direction);
      if (!result.moved) continue;
      // 用指数棋盘评估，再顺手加一点分数权重，避免它只顾形状不拿分
      const score = evaluateExponents(result.board) + result.gained * 0.5;
      if (score > bestScore) {
        bestScore = score;
        bestMove = direction;
      }
    }
    return bestMove;
  }
}

/** 数值棋盘 -> 指数棋盘。 */
function toExponents(board) {
  return board.map((row) => row.map((v) => (v === 0 ? 0 : Math.round(Math.log2(v)))));
}

// 方向 -> 哪个边界是高权重端。
// 蛇形权重右下角最大，所以往右下走的方向天然受益。
const SNAKE = [
  [16, 15, 14, 13],
  [9, 10, 11, 12],
  [8, 7, 6, 5],
  [1, 2, 3, 4],
];

function evaluateExponents(exponents) {
  const SIZE = 4;
  let empty = 0;
  let snake = 0;
  let mono = 0;
  let smooth = 0;

  for (let r = 0; r < SIZE; r++) {
    for (let c = 0; c < SIZE; c++) {
      const e = exponents[r][c];
      if (e === 0) {
        empty += 1;
        continue;
      }
      snake += e * SNAKE[r][c];
      if (c + 1 < SIZE) {
        const right = exponents[r][c + 1];
        if (right !== 0) {
          smooth -= Math.abs(e - right);
          mono -= e > right ? e - right : 0;
        }
      }
      if (r + 1 < SIZE) {
        const down = exponents[r + 1][c];
        if (down !== 0) {
          smooth -= Math.abs(e - down);
          mono -= e > down ? e - down : 0;
        }
      }
    }
  }
  return empty * 400 + mono * 120 + smooth * 30 + snake * 12;
}

/**
 * WebSocket 后端：连本地 C++ 引擎（agent 引擎进程）。
 *
 * 目前引擎侧还没有实现在（里程碑 4 才做服务）。所以这个类在连不上时
 * 会抛错，由 createTransport 决定是否降级。
 */
export class WebSocketTransport {
  /**
   * @param {string} url 形如 ws://127.0.0.1:8765
   * @param {number} [timeoutMs] 连接超时
   * @param {number} [requestTimeoutMs] 等引擎回包的超时，随 AI 强度变化
   */
  constructor(url, timeoutMs = 1500, requestTimeoutMs = 5000) {
    this.url = url;
    this.timeoutMs = timeoutMs;
    this.requestTimeoutMs = requestTimeoutMs;
    this.name = 'websocket';
    this.description = `C++ 引擎（${url}）`;
    this.socket = null;
    this.nextId = 1;
    this.pending = new Map();
    this.lastDebugInfo = null;
  }

  /**
   * 调整等回包的超时。
   *
   * 必须大于引擎自己的时间预算 —— 见 web/js/config.js 的 requestTimeoutMs()。
   * 前端先超时的话，每次请求都报"引擎响应超时"，看起来像引擎坏了。
   */
  setRequestTimeout(ms) {
    if (Number.isFinite(ms) && ms > 0) this.requestTimeoutMs = ms;
  }

  connect() {
    return new Promise((resolve, reject) => {
      let settled = false;
      const timer = setTimeout(() => {
        if (settled) return;
        settled = true;
        reject(new Error(`连接 ${this.url} 超时`));
      }, this.timeoutMs);

      let socket;
      try {
        socket = new WebSocket(this.url);
      } catch (error) {
        clearTimeout(timer);
        settled = true;
        reject(error);
        return;
      }

      socket.addEventListener('open', () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        this.socket = socket;
        resolve();
      });

      socket.addEventListener('error', () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        reject(new Error(`无法连接 ${this.url}`));
      });

      socket.addEventListener('message', (event) => {
        let envelope;
        try {
          envelope = JSON.parse(event.data);
        } catch {
          return;
        }
        const handlers = this.pending.get(envelope.id);
        if (!handlers) return;
        this.pending.delete(envelope.id);

        if (envelope.type === 'result') {
          if (envelope.payload && envelope.payload.debugInfo) {
            this.lastDebugInfo = envelope.payload.debugInfo;
          }
          handlers.resolve(envelope.payload);
        } else if (envelope.type === 'error') {
          handlers.reject(new Error((envelope.payload && envelope.payload.message) || '引擎返回错误'));
        } else {
          handlers.reject(new Error(`未知响应类型: ${envelope.type}`));
        }
      });

      socket.addEventListener('close', () => {
        this.socket = null;
        // 连接断了要让所有等待中的请求失败，否则 UI 会一直转圈
        for (const [, handlers] of this.pending) {
          handlers.reject(new Error('与引擎的连接已断开'));
        }
        this.pending.clear();
      });
    });
  }

  _post(type, payload) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error('与引擎的连接不可用'));
    }
    return new Promise((resolve, reject) => {
      // 请求超时：引擎卡住时不能让 UI 无限等待。
      // AGENTS.md 要求"超时也必须返回已完成搜索中的最佳合法步" ——
      // 那是引擎侧的责任；前端这里只负责不把自己挂死。
      const id = this.nextId++;
      const timer = setTimeout(() => {
        if (!this.pending.has(id)) return;
        this.pending.delete(id);
        reject(new Error('引擎响应超时'));
      }, this.requestTimeoutMs);

      this.pending.set(id, {
        resolve: (value) => {
          clearTimeout(timer);
          resolve(value);
        },
        reject: (error) => {
          clearTimeout(timer);
          reject(error);
        },
      });

      this.socket.send(JSON.stringify({ id, type, payload }));
    });
  }

  async configure(config) {
    await this._post('configure', { config });
  }

  async bestMove(board, options = {}) {
    const payload = await this._post('best-move', {
      state: { board },
      options,
    });
    return {
      move: payload && payload.move ? payload.move : null,
      debugInfo: (payload && payload.debugInfo) || this.lastDebugInfo,
    };
  }

  async evaluateRootDirection(board, direction, options = {}) {
    return this._post('evaluate-root-direction', { board, direction, options });
  }

  destroy() {
    if (this.socket) {
      try {
        this.socket.close();
      } catch {
        // 已经断了就无所谓
      }
      this.socket = null;
    }
  }
}

/**
 * 建一个可用的传输后端。
 *
 * 优先连引擎；连不上就降级到本地 AI，并**如实报告降级事实** ——
 * 调用方要把这件事显示给用户，不能让用户以为降级 AI 就是引擎的水平。
 *
 * @param {object} [options]
 * @param {string|null} [options.engineUrl] 引擎地址；null 表示不用引擎
 * @param {number} [options.requestTimeoutMs] 等引擎回包的超时（随 AI 强度变化）
 * @returns {Promise<{transport: object, degraded: boolean, reason: string|null}>}
 */
export async function createTransport({ engineUrl = null, requestTimeoutMs } = {}) {
  if (!engineUrl) {
    return {
      transport: new LocalTransport(),
      degraded: true,
      reason: '未配置引擎地址，使用本地降级 AI',
    };
  }

  const remote = new WebSocketTransport(engineUrl, 1500, requestTimeoutMs);
  try {
    await remote.connect();
    return { transport: remote, degraded: false, reason: null };
  } catch (error) {
    remote.destroy();
    const reason = error && error.message ? error.message : String(error);
    return {
      transport: new LocalTransport(),
      degraded: true,
      reason: `连不上引擎（${reason}），已降级为本地 AI`,
    };
  }
}

export { DIRECTIONS, hasLegalMove };
