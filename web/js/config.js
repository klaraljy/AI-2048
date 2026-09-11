// AI 强度与速度的定义 —— **单一数据源**。
//
// 界面的下拉框、发给引擎的 configure、自动演示的间隔，全部从这里读。
// 分散在多处写数字是这类参数最容易出的错：改了 A 忘了 B，
// 界面上写着"最强"而引擎还在跑旧参数，且没有任何报错。
//
// 术语约定（重要）：
//   **参考实现里的「简单/中等/困难」不是 AI 强度**，而是改生成规则的
//   （简单模式 70% 的概率把新方块塞进角落）。那种"难度"是靠作弊让玩家赢得轻松，
//   本项目已确认不做。所以这里的强度只体现在**搜索深度与用时**上，
//   并且必须如实标注"看几步"，让人一眼知道强在哪。

/**
 * 搜索深度按**层**计（max 层与 chance 层交替），所以：
 *   depth 8 = 向前看 4 步
 * 界面上一律换算成"步"，避免"深度 8"被理解成 8 步。
 *
 * 参数依据（seeds-v1.txt 各 100 局实测）：
 *   d4 → 34,934 分，2048 达 80%，每局 1.5 s   （d6 的 1/4 耗时换 98% 的分数）
 *   d6 → 35,658 分，2048 达 79%，每局 6.1 s   （相对 d4 只 +2%，几乎白花）
 *   d8 → 46,851 分，2048 达 92%，每局 ~8 s    （真正的台阶）
 *
 * 所以界面必须**如实**标注：中间档不是"比入门强一截"，而是"多一点、
 * 慢很多"。把三档说成等距提升是在骗用户，他会以为调中间档能明显变强。
 */
export const STRENGTH = {
  beginner: { label: '入门', depth: 4, budgetMs: 300 },
  standard: { label: '标准', depth: 6, budgetMs: 800 },
  expert: { label: '最强', depth: 8, budgetMs: 2500 },
};

/** 每档强度对应的"看几步"，只用于显示。 */
export function stepsAhead(depth) {
  return Math.round(depth / 2);
}

/**
 * 每档强度的"看几步"，只用于显示。
 *
 * note 是实测备注，直接显示在界面上 —— 用户有权知道中间档其实是"性价比最差"的：
 * 花 4 倍时间只多 2% 分数。不说的话他会以为"标准"比"入门"强一截。
 */
const NOTES = {
  beginner: '最快，分数已接近标准档',
  standard: '比入门略强，但慢很多',
  expert: '明显更强，也最慢',
};

export function strengthOptions() {
  return Object.entries(STRENGTH).map(([value, spec]) => ({
    value,
    label: `${spec.label} · 看 ${stepsAhead(spec.depth)} 步`,
    note: NOTES[value],
    spec,
  }));
}

/**
 * 自动演示的速度：**两步之间的间隔**（毫秒）。
 *
 * 语义按用户的说法定死：
 *   慢 = 看它怎么想（有停顿，看得清每一步）
 *   中 = 正常观看
 *   快 = 快速测试（几乎不停，尽快出结果）
 *
 * 注意这里只是"间隔"。引擎**思考**用多久由 STRENGTH.budgetMs 决定 ——
 * 两者是独立的旋钮：想看慢但算得快，把速度调慢即可，不必降低强度。
 */
export const SPEED = {
  slow: { label: '慢', intervalMs: 160 },
  medium: { label: '中', intervalMs: 80 },
  fast: { label: '快', intervalMs: 30 },
};

export function speedOptions() {
  return Object.entries(SPEED).map(([value, spec]) => ({
    value,
    label: `${spec.label} · ${describeSpeed(value)}`,
    spec,
  }));
}

function describeSpeed(value) {
  if (value === 'slow') return '看它怎么想';
  if (value === 'fast') return '快速测试';
  return '正常观看';
}

/** 发给引擎的 configure 报文。字段名与 docs/protocol.md 的约定一致。 */
export function toEngineConfig(spec) {
  return { baseDepth: spec.depth, timeBudgetMs: spec.budgetMs };
}

/**
 * 前端等待引擎回包的超时。
 *
 * 必须**大于**引擎自己的时间预算：预算 2500ms 而前端 2000ms 超时的话，
 * 前端会先放弃，然后每次请求都报"引擎响应超时"，看起来像引擎坏了。
 * 留 2 倍再加 1 秒余量，容忍残局时引擎略微超出预算的情况。
 */
export function requestTimeoutMs(spec) {
  return spec.budgetMs * 2 + 1000;
}

/** 从下拉框的当前值取出规格；值不认识时退回标准档。 */
export function specFor(value) {
  return STRENGTH[value] ?? STRENGTH.standard;
}
