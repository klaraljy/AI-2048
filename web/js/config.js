// AI 强度与速度的定义 —— **单一数据源**。
//
// 界面的下拉框、发给引擎的 configure、自动演示的间隔，全部从这里读。
// 分散在多处写数字是这类参数最容易出的错：改了 A 忘了 B，
// 界面上写着"最强"而引擎还在跑旧参数，且没有任何报错。
//
// 术语约定（重要）：
//   **「难度」与「AI 强度」是两件事**，界面与文档都要分开：
//     难度 → 改规则（新方块落在哪），影响游戏本身有多难
//     强度 → 改搜索深度与用时，影响 AI 下得多好
//
// 参数依据（seeds-v1.txt 各 100 局实测，**蛇形评分修正之后**）：
//   d4 → 42,792 分，2048 达 88%、4096 达 37%，每步约 3 ms
//   d6 → 47,249 分，2048 达 92%、4096 达 45%，每步约 21 ms  ← 性价比最好
//   d8 → 明显更慢，见 docs/results
//
// **d6 已经超过修正前的 d8（46,851）**，所以「标准」档就是引擎的甜点。
// 界面必须如实标注"看几步"，让人一眼知道强在哪。
//
// ⚠️ 时间预算是**上限**，不是目标：实测每步耗时远低于预算，
// 所以正常情况下深度才是实际瓶颈。预算的作用是防止残局深搜卡住 ——
// 超时会返回已完成搜索中的最佳合法步（见 AGENTS.md 的硬实时预算一条）。
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
 * 每档强度的实测备注，直接显示在界面上。
 *
 * 这些数字必须随评估函数一起更新 —— 写着"比入门略强"而实测强 10%，
 * 或者反过来，都是在误导用户。改权重后记得回来核对。
 */
const NOTES = {
  beginner: '最快，2048 达 88%',
  standard: '性价比最好，4096 达 45%',
  expert: '更强但慢很多',
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

/**
 * 发给引擎的 configure 报文。字段名与 docs/protocol.md 的约定一致。
 *
 * 除强度参数外还要带上**难度** —— 它是 AI 的世界模型：
 * 引擎按它给 chance 节点的各空格加权。不带的话 AI 一律假设"全盘均匀"，
 * hard 档下会低估"新块贴着自己最大块出现"的风险，走子偏乐观。
 */
export function toEngineConfig(spec, difficulty = DIFFICULTY_NORMAL) {
  return {
    baseDepth: spec.depth,
    timeBudgetMs: spec.budgetMs,
    difficulty,
  };
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

// ---------------------------------------------------------------------------
// 难度：改变**新方块出现的位置分布**（属于规则，不属于 AI 强度）
//
// 与「AI 强度」是两件完全不同的事，界面上必须分开：
//   难度 → 改规则，影响**游戏本身有多难**
//   强度 → 改搜索，影响**AI 下得多好**
//
// 参数与引擎的 core/game.h 完全一致，由
// tests/difficulty-parity.test.mjs 逐位对拍保证。
//
// ⚠️ 分数**不跨难度可比**：改的是生成规则，简单档分数天然更高。
// ---------------------------------------------------------------------------
export const DIFFICULTY_EASY = 'easy';
export const DIFFICULTY_NORMAL = 'normal';
export const DIFFICULTY_HARD = 'hard';

export const DIFFICULTY = {
  easy: { label: '简单', note: '70% 生成在角落' },
  normal: { label: '中等', note: '全盘随机（标准）' },
  hard: { label: '困难', note: '80% 生成在最大块旁' },
};

/**
 * 默认难度 = 标准 2048。**不得改动** ——
 * 所有历史分数与公开基准都是在标准规则下取得的。
 */
export const DEFAULT_DIFFICULTY = DIFFICULTY_NORMAL;

export function difficultyOptions() {
  return Object.entries(DIFFICULTY).map(([value, spec]) => ({
    value,
    label: spec.label,
    note: spec.note,
  }));
}

/** 难度是不是标准规则。非标准时界面要提示分数不可与基准比较。 */
export function isStandardDifficulty(value) {
  return value === DIFFICULTY_NORMAL;
}

export function difficultyLabel(value) {
  return DIFFICULTY[value]?.label ?? DIFFICULTY[DIFFICULTY_NORMAL].label;
}
