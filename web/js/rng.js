// 确定性伪随机数发生器。
//
// **必须与 C++ 引擎（engine/src/core/rng.cpp）逐位一致** ——
// 同一个种子在前端和引擎里必须产生完全相同的对局，
// 否则"前端降级运行时用本地规则"和"引擎在跑时用引擎规则"就会得到不同的结果，
// 那比不能玩更糟。
//
// 算法：xoshiro256** + splitmix64 播种 + Lemire 拒绝采样，全部用 BigInt 实现
// 64 位无符号运算。不用 Math.random、不用 number 位运算 ——
// number 只有 53 位精度，做不了 64 位状态。

const MASK64 = (1n << 64n) - 1n;

/** 64 位无符号取模：BigInt 的 & 在负数上会得到负数，必须显式截断。 */
function u64(x) {
  return BigInt.asUintN(64, x);
}

/** splitmix64。用于把单个种子扩展成 4 个质量良好的状态字。 */
function splitMix64(state) {
  const next = u64(state.value + 0x9e3779b97f4a7c15n);
  state.value = next;
  let z = next;
  z = u64((z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n);
  z = u64((z ^ (z >> 27n)) * 0x94d049bb133111ebn);
  return u64(z ^ (z >> 31n));
}

function rotl(x, k) {
  return u64((x << BigInt(k)) | (x >> BigInt(64 - k)));
}

export class Rng {
  /** @param {number|bigint|string} seed 种子。0 也会得到有效状态。 */
  constructor(seed) {
    let s;
    try {
      s = u64(BigInt(seed));
    } catch {
      s = 0n;
    }
    const holder = { value: s };
    this._state = [splitMix64(holder), splitMix64(holder), splitMix64(holder), splitMix64(holder)];
  }

  /** 返回 [0, 2^64) 内的下一个值。 */
  nextU64() {
    const s = this._state;
    const result = u64(rotl(u64(s[1] * 5n), 7) * 9n);
    const t = u64(s[1] << 17n);

    s[2] = u64(s[2] ^ s[0]);
    s[3] = u64(s[3] ^ s[1]);
    s[1] = u64(s[1] ^ s[2]);
    s[0] = u64(s[0] ^ s[3]);
    s[2] = u64(s[2] ^ t);
    s[3] = rotl(s[3], 45);

    return result;
  }

  /**
   * 返回 [0, bound) 内的均匀整数。
   * 用拒绝采样而不是取模 —— 取模会引入偏差，偏差会让统计不可信。
   */
  nextBounded(bound) {
    if (bound <= 1) return 0;
    const b = BigInt(Math.floor(bound));
    if (b <= 1n) return 0;
    const threshold = u64((0n - b) % b);
    for (;;) {
      const r = this.nextU64();
      if (r >= threshold) return Number(r % b);
    }
  }

  /** 以 numerator/denominator 的概率返回 true。 */
  chance(numerator, denominator) {
    if (numerator <= 0) return false;
    if (numerator >= denominator) return true;
    return this.nextBounded(denominator) < numerator;
  }
}

export { MASK64 };
