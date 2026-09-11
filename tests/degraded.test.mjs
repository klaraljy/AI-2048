// 降级路径测试：这两条路径平时跑不到，但正是"引擎连不上不白屏"的核心。
//
// 1. sound.js —— 浏览器不支持 WebAudio（或隐私模式）时不能崩
// 2. transport.js —— 引擎连不上时必须降级并如实报告，而不是抛错或白屏
//
// 运行： node tests\degraded.test.mjs

import { installDomStub } from './dom-stub.mjs';

installDomStub();

const { Sound } = await import('../web/js/sound.js');
const { createTransport, LocalTransport } = await import('../web/js/transport.js');
const { Game, board_values } = await import('../web/js/game.js');

let failures = 0;
function check(label, condition, detail = '') {
  if (condition) {
    console.log(`  ✓ ${label}`);
  } else {
    failures += 1;
    console.error(`  ✗ ${label}${detail ? `  —— ${detail}` : ''}`);
  }
}

console.log('降级路径测试：');

// --- 1. 没有 WebAudio ---
{
  delete globalThis.window.AudioContext;
  delete globalThis.window.webkitAudioContext;

  const sound = new Sound();
  sound.unlock();
  check('无 WebAudio 时 unlock 不抛异常', true);
  check('无 WebAudio 时 ready 为假', sound.ready === false);

  let threw = null;
  try {
    sound.play('merge');
    sound.playMerge(11);
    sound.play('nonexistent-sound');
  } catch (error) {
    threw = error;
  }
  check('无 WebAudio 时 play 不抛异常（静默降级）', threw === null, threw ? String(threw) : '');

  // 静音开关必须仍然可用 —— 它是 UI 状态，不该依赖音频后端
  const muted = sound.toggleMuted();
  check('无 WebAudio 时静音开关仍可用', muted === true && sound.muted === true);
  sound.setMuted(false);
  check('静音状态可恢复', sound.muted === false);
}

// --- 2. WebAudio 存在但构造抛错（某些隐私模式会这样） ---
{
  globalThis.window.AudioContext = function Broken() {
    throw new Error('AudioContext 被策略禁用');
  };
  const sound = new Sound();
  sound.unlock();
  check('AudioContext 构造抛错时不崩', sound.ready === false);
  let threw = null;
  try {
    sound.play('slide');
  } catch (error) {
    threw = error;
  }
  check('构造失败后 play 仍不抛异常', threw === null, threw ? String(threw) : '');
}

// --- 3. context 处于 suspended 时必须仍能出声（这是真实用户报过的 bug） ---
//
// 现象：「点 AI 测试没声音，关掉重开才有」。
//
// 根因有两个，都在这里钉住：
//   a) ready 曾经要求 state === 'running'，而 resume() 是异步的 ——
//      在它落地之前 play() 会直接 return，第一次点击的音全被丢掉。
//   b) AI 按钮不经过 Input 的手势解锁路径，AudioContext 压根没被创建。
//
// (b) 在 main.js 里用全局 capture 监听解决，(a) 在这里守。
{
  const calls = { resume: 0, oscillators: 0 };

  class StubAudioContext {
    constructor() {
      // 浏览器在用户手势之外创建的 context 就是 'suspended'，
      // 这里刻意用这个状态，才能复现"第一次点击没声音"。
      this.state = 'suspended';
      this.currentTime = 0;
      this.sampleRate = 44100;
      this.destination = {};
    }
    resume() {
      calls.resume += 1;
      this.state = 'running';
      return Promise.resolve();
    }
    createGain() {
      return {
        gain: { value: 1, setValueAtTime() {}, linearRampToValueAtTime() {}, exponentialRampToValueAtTime() {} },
        connect() {},
      };
    }
    createOscillator() {
      calls.oscillators += 1;
      return { type: '', frequency: { value: 0 }, connect() {}, start() {}, stop() {} };
    }
    createBiquadFilter() {
      return { type: '', frequency: { value: 0 }, Q: { value: 0 }, connect() {} };
    }
    createBuffer(channels, frames) {
      return { getChannelData: () => new Float32Array(frames) };
    }
    createBufferSource() {
      return { buffer: null, playbackRate: { value: 1 }, connect() {}, start() {}, stop() {} };
    }
    decodeAudioData() {
      return Promise.resolve({});
    }
  }

  globalThis.window.AudioContext = StubAudioContext;

  const sound = new Sound();
  sound.unlock();
  check('suspended 的 context 仍算 ready（不再要求 state === running）', sound.ready === true);
  check('unlock 会发起 resume', calls.resume >= 1);

  // 关键：即使此刻 state 还没变成 running，play 也必须真的去发音，
  // 而不是被 ready 检查拦掉。
  const before = calls.oscillators;
  sound.ctx.state = 'suspended'; // 模拟 resume() 尚未落地
  sound.play('merge');
  check('suspended 期间 play 仍然发声（不被静默丢弃）', calls.oscillators > before,
    `oscillators: ${before} -> ${calls.oscillators}`);

  // 静音时不该发声
  const before2 = calls.oscillators;
  sound.setMuted(true);
  sound.play('merge');
  check('静音时 play 不发声', calls.oscillators === before2);
  sound.setMuted(false);
}

// --- 4. 不传 engineUrl：应直接用降级 AI ---
{
  const result = await createTransport({ engineUrl: null });
  check('未配置引擎时降级', result.degraded === true);
  check('降级时给出原因文字', typeof result.reason === 'string' && result.reason.length > 0,
    result.reason || '(空)');
  check('降级后端是 LocalTransport', result.transport instanceof LocalTransport);
}

// --- 4. 配了 engineUrl 但连不上：必须降级而不是抛错 ---
{
  // 指向一个几乎肯定没人监听的端口
  const result = await createTransport({ engineUrl: 'ws://127.0.0.1:59999' });
  check('连不上引擎时降级（不抛错）', result.degraded === true);
  check('降级原因里包含失败信息', typeof result.reason === 'string' && result.reason.length > 0,
    result.reason || '(空)');
  check('降级后端可用', result.transport instanceof LocalTransport);
}

// --- 5. 降级 AI 必须真的能给出决策，否则页面还是没法玩 ---
{
  const result = await createTransport({ engineUrl: null });
  const transport = result.transport;
  await transport.configure({});

  const game = new Game(1);
  const decision = await transport.bestMove(board_values(game.board), { lastMove: null });
  check('降级 AI 能给出方向', typeof decision.move === 'string' && decision.move.length > 0,
    String(decision.move));

  // 走一步之后仍应能继续决策
  game.step(decision.move);
  const second = await transport.bestMove(board_values(game.board), { lastMove: decision.move });
  check('降级 AI 能连续决策', typeof second.move === 'string' && second.move.length > 0,
    String(second.move));

  // 给出的方向必须合法 —— 否则会卡住玩家的局
  const { applyMove } = await import('../web/js/game.js');
  check('降级 AI 给出的方向合法', applyMove(game.board, second.move).moved === true);
}

// --- 6. 终局时降级 AI 应返回 null 而不是乱给方向 ---
{
  const result = await createTransport({ engineUrl: null });
  // 构造一个无路可走的满盘（相邻皆不同）
  const dead = [
    [1, 2, 1, 2],
    [2, 1, 2, 1],
    [1, 2, 1, 2],
    [2, 1, 2, 1],
  ];
  const { board_values: toValues } = await import('../web/js/game.js');
  const decision = await result.transport.bestMove(toValues(dead), {});
  check('终局时降级 AI 返回 null（表示无步可走）', decision.move === null, String(decision.move));
}

console.log('');
if (failures > 0) {
  console.error(`降级路径测试失败：${failures} 项`);
  process.exit(1);
}
console.log('降级路径测试通过');
