// 音效：用 WebAudio 现场合成，**不引入任何音频文件**。
//
// 为什么合成而不是下载素材：
//   - 零仓库体积，零版权问题，无需 assets 目录
//   - 音高可以按瓦片数值变 —— 合并出越大的块，音越高，
//     这是素材方案要准备几十个文件才能做到的效果
//
// 浏览器要求音频必须由**用户手势**触发才能播放（autoplay 策略）。
// 所以这里在第一次 pointerdown / keydown 时才创建 AudioContext。

/** 参数：每个音效的波形、起始频率、时长、音量。 */
const SOUNDS = {
  slide: { type: 'triangle', from: 180, to: 120, duration: 0.06, gain: 0.05 },
  spawn: { type: 'sine', from: 420, to: 560, duration: 0.08, gain: 0.045 },
  // 合并音：音高随瓦片对数上升，让"合出大块"听起来更有分量
  merge: { type: 'sine', from: 320, to: 520, duration: 0.12, gain: 0.09 },
  win: { type: 'sine', from: 523, to: 1046, duration: 0.5, gain: 0.12 },
  gameover: { type: 'sawtooth', from: 330, to: 90, duration: 0.7, gain: 0.1 },
  undo: { type: 'triangle', from: 300, to: 180, duration: 0.1, gain: 0.05 },
};

const MUTE_KEY = 'ai2048.muted';

export class Sound {
  constructor() {
    this.ctx = null;
    this.muted = false;
    try {
      this.muted = localStorage.getItem(MUTE_KEY) === '1';
    } catch {
      // localStorage 不可用（隐私模式等）时按未静音处理
      this.muted = false;
    }
  }

  /** 是否已可用（AudioContext 只能由用户手势创建）。 */
  get ready() {
    return this.ctx !== null && this.ctx.state === 'running';
  }

  /**
   * 在用户手势里调用一次即可解锁音频。
   * 重复调用是安全的。
   */
  unlock() {
    if (this.ctx) {
      // 有些浏览器会把 context 挂起（切标签页回来），需要显式恢复
      if (this.ctx.state === 'suspended') this.ctx.resume().catch(() => {});
      return;
    }
    const Ctor = typeof window !== 'undefined' && (window.AudioContext || window.webkitAudioContext);
    if (!Ctor) return; // 浏览器不支持：静默降级，不影响玩法
    try {
      this.ctx = new Ctor();
    } catch {
      this.ctx = null;
    }
  }

  setMuted(muted) {
    this.muted = Boolean(muted);
    try {
      localStorage.setItem(MUTE_KEY, this.muted ? '1' : '0');
    } catch {
      // 存不下就算了，不影响本次会话
    }
  }

  toggleMuted() {
    this.setMuted(!this.muted);
    return this.muted;
  }

  /**
   * 播放一个音效。
   * @param {string} name SOUNDS 里的键
   * @param {number} [pitchScale] 音高倍数，用于按瓦片大小变调
   */
  play(name, pitchScale = 1) {
    if (this.muted || !this.ready) return;
    const spec = SOUNDS[name];
    if (!spec) return;

    const ctx = this.ctx;
    const now = ctx.currentTime;
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();

    osc.type = spec.type;
    const from = spec.from * pitchScale;
    const to = spec.to * pitchScale;
    osc.frequency.setValueAtTime(from, now);
    osc.frequency.exponentialRampToValueAtTime(Math.max(20, to), now + spec.duration);

    // 短促的包络，避免爆音
    gain.gain.setValueAtTime(0, now);
    gain.gain.linearRampToValueAtTime(spec.gain, now + 0.008);
    gain.gain.exponentialRampToValueAtTime(0.0001, now + spec.duration);

    osc.connect(gain);
    gain.connect(ctx.destination);
    osc.start(now);
    osc.stop(now + spec.duration + 0.02);
  }

  /** 合并音：瓦片越大音越高（用对数映射，避免高数值时刺耳）。 */
  playMerge(maxExponent) {
    // 指数 1（=2）时不升调，指数 11（=2048）时升到约 2 倍
    const scale = 1 + Math.min(10, Math.max(0, maxExponent - 1)) * 0.09;
    this.play('merge', scale);
  }
}
