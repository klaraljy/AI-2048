// 音效：以**真实采样**为主 + WebAudio 现场合成为辅。
//
// 素材：assets/sounds/drum33.wav（用户提供的鼓声，25 KB）
//   - 合并音用它做主体，靠**播放速率**变调 —— 块越大音越高
//   - 合成只用来补"滑动/出现/撤销"这些采样不适合的短音
//
// 为什么改用采样：之前两版都是纯合成，用户反馈"钝、听多了不适"。
// 第一版是正弦 + 音高下滑（听着像叹气），第二版改成固定音高 + 双泛音
// （好一些但"还是难听"）。真实敲击的瞬态是合成很难模仿的，所以主体换成采样。
//
// 采样**异步**加载：没到位时走合成兜底，保证第一次点击就出声，
// 不会因为加载而静默。

/** 采样文件，相对 web/ 的路径。 */
const SAMPLE_URL = './assets/sounds/drum33.wav';

/** 合成兜底：固定音高 + 极快起音 + 两个泛音各自衰减。 */
const SYNTH = {
  merge: { freq: 660, duration: 0.12, gain: 0.07, partial: 2.2 },
  spawn: { freq: 880, duration: 0.07, gain: 0.035, partial: 1.8 },
  slide: { noise: true, duration: 0.03, gain: 0.028, filterHz: 3200 },
  undo: { freq: 520, duration: 0.08, gain: 0.04, partial: 1.6 },
};

const MUTE_KEY = 'ai2048.muted';

export class Sound {
  constructor() {
    this.ctx = null;
    /** @type {AudioBuffer|null} 鼓声采样，加载完成后可用 */
    this.drum = null;
    this.loading = false;
    this.muted = false;
    try {
      this.muted = localStorage.getItem(MUTE_KEY) === '1';
    } catch {
      this.muted = false;
    }
  }

  /** 音频是否可用（AudioContext 只能由用户手势创建）。 */
  get ready() {
    return this.ctx !== null && this.ctx.state === 'running';
  }

  /** 采样是否就绪。没就绪时走合成兜底，不影响出声。 */
  get hasSample() {
    return this.drum !== null;
  }

  /**
   * 在用户手势里调用一次即可解锁音频，并开始加载采样。
   * 重复调用是安全的。
   */
  unlock() {
    if (!this.ctx) {
      const Ctor =
        typeof window !== 'undefined' && (window.AudioContext || window.webkitAudioContext);
      if (!Ctor) return; // 浏览器不支持：静默降级，不影响玩法
      try {
        this.ctx = new Ctor();
      } catch {
        this.ctx = null;
        return;
      }
    } else if (this.ctx.state === 'suspended') {
      // 有些浏览器会把 context 挂起（切标签页回来）
      this.ctx.resume().catch(() => {});
    }
    this._loadSample();
  }

  /** 异步加载鼓声采样，只加载一次。失败就保持合成兜底，不打断玩法。 */
  _loadSample() {
    if (this.loading || this.drum !== null || !this.ctx) return;
    if (typeof fetch !== 'function') return;
    this.loading = true;

    fetch(SAMPLE_URL)
      .then((response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.arrayBuffer();
      })
      .then((buffer) => this.ctx.decodeAudioData(buffer))
      .then((decoded) => {
        this.drum = decoded;
      })
      .catch(() => {
        // 采样拿不到就一直是合成音 —— 用户听得出区别，但游戏不受影响
        this.drum = null;
      })
      .finally(() => {
        this.loading = false;
      });
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
   * @param {string} name 'merge' | 'spawn' | 'slide' | 'undo'
   * @param {number} [pitchScale] 音高倍数（合并音按块大小变调）
   */
  play(name, pitchScale = 1) {
    if (this.muted || !this.ready) return;

    // 合并音优先用采样：真实鼓声的敲击感是合成做不到的
    if (name === 'merge' && this.drum) {
      this._playSample(this.drum, pitchScale, 0.85);
      return;
    }

    const spec = SYNTH[name];
    if (!spec) return;
    if (spec.noise) {
      this._noise(spec, pitchScale);
    } else {
      this._struck(spec.freq * pitchScale, spec.duration, spec.gain, spec.partial);
    }
  }

  /**
   * 播放采样。
   *
   * @param {AudioBuffer} buffer
   * @param {number} rate 播放速率（>1 更快更高）
   * @param {number} gainValue 音量
   */
  _playSample(buffer, rate, gainValue) {
    const ctx = this.ctx;
    const source = ctx.createBufferSource();
    source.buffer = buffer;
    source.playbackRate.value = rate;

    const now = ctx.currentTime;
    const gain = ctx.createGain();
    gain.gain.setValueAtTime(gainValue, now);
    // 短促收尾：采样尾巴拖长会在连续合并时糊成一片
    gain.gain.exponentialRampToValueAtTime(0.0001, now + 0.22);

    // 轻微高通：去掉一点低频，听感更脆
    const highpass = ctx.createBiquadFilter();
    highpass.type = 'highpass';
    highpass.frequency.value = 220;

    source.connect(highpass);
    highpass.connect(gain);
    gain.connect(ctx.destination);
    source.start(now);
    source.stop(now + 0.28);
  }

  /** 合成敲击音：基频 + 上方泛音，各自独立衰减。 */
  _struck(freq, duration, gainValue, partialRatio) {
    const ctx = this.ctx;
    const now = ctx.currentTime;
    const out = ctx.createGain();
    out.gain.value = 1;
    out.connect(ctx.destination);

    const voices = [
      { ratio: 1, gain: gainValue, decay: duration },
      { ratio: partialRatio, gain: gainValue * 0.4, decay: duration * 0.55 },
    ];

    for (const voice of voices) {
      const osc = ctx.createOscillator();
      osc.type = 'sine';
      osc.frequency.value = freq * voice.ratio;

      const envelope = ctx.createGain();
      // 起音 2ms：够快才有敲击感，又不至于爆音
      envelope.gain.setValueAtTime(0, now);
      envelope.gain.linearRampToValueAtTime(voice.gain, now + 0.002);
      envelope.gain.exponentialRampToValueAtTime(0.0001, now + voice.decay);

      osc.connect(envelope);
      envelope.connect(out);
      osc.start(now);
      osc.stop(now + voice.decay + 0.02);
    }
  }

  /** 极短噪声脉冲，用作"嗒"。 */
  _noise(spec, pitchScale) {
    const ctx = this.ctx;
    const now = ctx.currentTime;
    const frames = Math.max(1, Math.floor(ctx.sampleRate * spec.duration));
    const buffer = ctx.createBuffer(1, frames, ctx.sampleRate);
    const data = buffer.getChannelData(0);
    for (let i = 0; i < frames; i++) {
      const decay = 1 - i / frames;
      data[i] = (Math.random() * 2 - 1) * decay * decay;
    }

    const source = ctx.createBufferSource();
    source.buffer = buffer;

    const filter = ctx.createBiquadFilter();
    filter.type = 'bandpass';
    filter.frequency.value = spec.filterHz * pitchScale;
    filter.Q.value = 0.9;

    const envelope = ctx.createGain();
    envelope.gain.setValueAtTime(spec.gain, now);
    envelope.gain.exponentialRampToValueAtTime(0.0001, now + spec.duration);

    source.connect(filter);
    filter.connect(envelope);
    envelope.connect(ctx.destination);
    source.start(now);
  }

  /**
   * 合并音：块越大音越高。
   *
   * 用**等程（八度）映射**而不是线性 —— 线性在高数值时几乎听不出差别。
   * 每级升两个半音，升到 4096 也只有约 1.8 倍速，不会刺耳。
   */
  playMerge(maxExponent) {
    const steps = Math.min(10, Math.max(0, maxExponent - 1));
    this.play('merge', Math.pow(2, (steps * 2) / 12));
  }
}
