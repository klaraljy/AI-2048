// 音效：用 WebAudio 现场合成，**不引入任何音频文件**。
//
// 为什么合成而不是下载素材：
//   - 零仓库体积，零版权问题，无需 assets 目录
//   - 音高可以按瓦片数值变 —— 合并出越大的块，音越高，
//     这是素材方案要准备几十个文件才能做到的效果
//
// ---------------------------------------------------------------------------
// 音色模型：敲击音（struck bell / 木琴）
//
// 第一版用的是「正弦波 + 音高下滑 + 指数衰减」，听感很钝、听久了不适。
// 原因有三，都是可复现的：
//   1. **音高下滑**听着像叹气，天然"往下掉"，不脆
//   2. 纯正弦没有泛音，音色发闷、没有"亮"的成分
//   3. 时长偏长（合并音 120ms）且全是低频段（320→520Hz），
//      连续合并时低频糊在一起，就是"钝"和"不适"的来源
//
// 现在改成：固定音高 + 极快起音（2ms）+ 两个泛音各自独立衰减。
// 高频泛音先消失、基频留一点余韵 —— 这正是"清脆"的物理成因。
//
// 起音必须是**极快的线性上升**而不是直接从 0 开始：
// 直接从 0 会爆音（波形瞬间跳变），2ms 上升既无爆音又有敲击感。
// ---------------------------------------------------------------------------

/** 每个音效的参数。gain 与 partial 都经过收敛，避免叠加后过载。 */
const SOUNDS = {
  // 滑动：极短的一声"嗒"。用噪声比用乐音更贴切，且不会糊在一起。
  slide: { kind: 'noise', duration: 0.035, gain: 0.035, filterHz: 3200 },
  // 新方块出现：轻轻一点，比滑动稍亮
  spawn: { kind: 'struck', freq: 880, duration: 0.075, gain: 0.038, partial: 1.8 },
  // 合并：主音，随瓦片变大升调
  merge: { kind: 'struck', freq: 660, duration: 0.13, gain: 0.075, partial: 2.2 },
  // 达到 2048：三音上行
  win: { kind: 'arp', notes: [784, 988, 1319], duration: 0.36, gain: 0.08 },
  // 结束：低沉但**不要**下滑拖长，两个短音更干净
  gameover: { kind: 'arp', notes: [392, 262], duration: 0.3, gain: 0.07 },
  undo: { kind: 'struck', freq: 520, duration: 0.08, gain: 0.045, partial: 1.6 },
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
    const Ctor =
      typeof window !== 'undefined' && (window.AudioContext || window.webkitAudioContext);
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
    if (spec.kind === 'noise') {
      this._noise(spec);
    } else if (spec.kind === 'arp') {
      this._arpeggio(spec, pitchScale);
    } else {
      this._struck(spec.freq * pitchScale, spec.duration, spec.gain, spec.partial);
    }
  }

  /**
   * 敲击音：基频 + 一个上方泛音，各自独立指数衰减。
   *
   * 泛音衰减得比基频快，所以听起来是"当"而不是"嗡"。
   * 两个泛音的音量比约 1 : 0.4 —— 泛音再高就变成刺耳的金属声了。
   */
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
      osc.type = 'sine'; // 正弦 + 独立泛音，比 triangle/sawtooth 干净，不会"毛躁"
      osc.frequency.value = freq * voice.ratio;

      const envelope = ctx.createGain();
      // 起音 2ms：足够快以产生敲击感，又不至于爆音
      envelope.gain.setValueAtTime(0, now);
      envelope.gain.linearRampToValueAtTime(voice.gain, now + 0.002);
      // 指数衰减到极小的非零值（不能到 0，指数斜坡不接受 0）
      envelope.gain.exponentialRampToValueAtTime(0.0001, now + voice.decay);

      osc.connect(envelope);
      envelope.connect(out);
      osc.start(now);
      osc.stop(now + voice.decay + 0.02);
    }
  }

  /** 噪声短音：带通滤过的极短脉冲，用作"嗒"。 */
  _noise(spec) {
    const ctx = this.ctx;
    const now = ctx.currentTime;
    const frames = Math.max(1, Math.floor(ctx.sampleRate * spec.duration));
    const buffer = ctx.createBuffer(1, frames, ctx.sampleRate);
    const data = buffer.getChannelData(0);
    for (let i = 0; i < frames; i++) {
      // 后段衰减：白噪声直接切断会"啪"一下
      const decay = 1 - i / frames;
      data[i] = (Math.random() * 2 - 1) * decay * decay;
    }

    const source = ctx.createBufferSource();
    source.buffer = buffer;

    const filter = ctx.createBiquadFilter();
    filter.type = 'bandpass';
    filter.frequency.value = spec.filterHz;
    filter.Q.value = 0.9;

    const envelope = ctx.createGain();
    envelope.gain.setValueAtTime(spec.gain, now);
    envelope.gain.exponentialRampToValueAtTime(0.0001, now + spec.duration);

    source.connect(filter);
    filter.connect(envelope);
    envelope.connect(ctx.destination);
    source.start(now);
  }

  /** 琶音：一串错开的敲击音，用于胜利 / 结束。 */
  _arpeggio(spec, pitchScale) {
    const step = spec.duration / (spec.notes.length + 0.5);
    spec.notes.forEach((freq, index) => {
      setTimeout(
        () => {
          if (!this.ready || this.muted) return;
          this._struck(freq * pitchScale, spec.duration * 0.7, spec.gain, 2.0);
        },
        index * step * 1000
      );
    });
  }

  /**
   * 合并音：瓦片越大音越高。
   *
   * 用等程（八度）映射而不是线性：线性在高数值时几乎听不出差别，
   * 而八度映射让 2→4→8 每一步都是明确的音阶上行。
   */
  playMerge(maxExponent) {
    // 指数 1（=2）时是基频，每升一级升高两个半音，升到 4096 也不会刺耳
    const steps = Math.min(10, Math.max(0, maxExponent - 1));
    const scale = Math.pow(2, (steps * 2) / 12);
    this.play('merge', scale);
  }
}
