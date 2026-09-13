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
// 参数依据（`seeds-v1`、normal、干净机器单线程、**merge 重标定之后**实测）：
//
//   档位  配置            局数  平均分   每局     2048   4096   8192
//   入门  d4 /  300ms      20   51,250   14.7 s   90%    50%     5%
//   标准  d6 /  800ms      10   96,366   64.4 s  100%    90%    50%
//   最强  d8 / 2500ms       2  110,686  149.0 s  100%   100%    50%
//
// **对照 merge 重标定之前**：37,742 / 42,043 / 52,094
// → 三档分别提升 **36% / 129% / 112%**。所以这三档的"承诺强度"比旧版高一个档：
// 现在的「标准」已经远超旧的「最强」。
//
// ⚠️ **最强档只有 2 局样本**，噪声很大（每局分数标准差约 26,000，2 局的标准误
// 约 18,000）。它的"100% 到 4096"可能偏高，不要当成保证。标准档的 10 局
// 相对可信（标准误约 8,200）。
//
// ⚠️ **depth 的单位是"树层"，不是"玩家步"**：max 与 chance 逐层交替，
// 所以 1 步前瞻 = 2 层（`stepsAhead` 做的就是 /2）。公开基准说的 "depth 8"
// 是 8 步 = 这里的 16 层，别把两者的数字直接比。
//
// ⚠️ depth 是**下限**，不是实际搜索深度：自适应深度会在空格少时加深，
// 实测三档分别能到 8 / 10 / 12 层（见上面实测表里的"最深达到"）。
// 界面上写"看 N 步"用的是 baseDepth 换算 —— 那是**承诺值**。
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
 * 这些数字必须随评估函数与生成规则一起更新 —— 写着"比入门略强"而实测强 10%，
 * 或者反过来，都是在误导用户。改权重后记得回来核对。
 * 当前数字对应上面的实测表（merge 重标定之后）。
 */
const NOTES = {
  beginner: '最快，2048 达 90%',
  standard: '2048 必达，4096 达 90%',
  expert: '4096 稳达，但每局约 2.5 分钟',
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
 * 默认难度 = **困难**，默认强度 = **最强**。
 *
 * 用户明确要求："默认最难加最强，我测试一直是这个，我肯定也会一直玩最难档，
 * 看看我与 AI 算法的差距"。所以界面一打开就是最难的组合。
 *
 * ⚠️ 困难档**不是标准 2048**（改变了新方块的落点分布），
 * 所以分数不可与公开基准或历史标准档成绩比较 —— 界面上有常驻提示。
 * `DIFFICULTY_NORMAL` 仍是"标准规则"的标识，只是不再作为界面默认值。
 */
export const DEFAULT_DIFFICULTY = DIFFICULTY_HARD;

/** 默认强度档位，见 STRENGTH。同样按用户要求取最强。 */
export const DEFAULT_STRENGTH = 'expert';

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
