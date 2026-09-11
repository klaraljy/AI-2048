// TD 学习：用自我对弈训练 n-tuple 价值网络。
//
// ## 学的是什么
//
// 网络预测的是 **afterstate 的价值**，也就是"走完这一步、还没生成新方块"的
// 盘面最终能拿到多少分（归一化）。用它做 TD(0)：
//
//     δ = (本步得分/kScoreScale + V(afterstate_t+1)) − V(afterstate_t)
//     W[用到的那些下标] += α · δ
//
// 本步得分是**真实的分数信号** —— 这正是这条路线成立的原因：
// 不依赖任何手写系数，游戏自己会告诉我们哪一步好。
//
// ## 为什么用 afterstate 而不是"完整盘面"
//
// 完整盘面（含刚生成的新方块）的价值要额外对随机的方块位置求期望；
// afterstate 则是走子的直接结果、不含随机性，TD 目标更干净。
// 需要完整盘面的价值时，取所有合法走子的 afterstate 最大值即可。
//
// ## 策略
//
// C1 阶段用**随机走子**：目的是验证"能不能学"，不是打高分。
// 随机策略的盘面分布很窄（很快就死），学到的是"什么样的烂盘面有救"，
// 恰好能验证梯度方向对不对。跑通后再换 ε-greedy 自对弈（C2）。

#ifndef AI2048_AI_LEARN_TRAINER_H_
#define AI2048_AI_LEARN_TRAINER_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ai/learn/value_network.h"

namespace ai2048::learn {

/** 一次训练运行的配置。 */
struct TrainConfig {
  int games = 1000;  // 自我对弈局数
  double learning_rate = 0.002;
  double discount = 0.98;  // γ：越接近残局，越看重后续得分

  /**
   * n-step 回报的步数。1 = 退化成 TD(0)。
   *
   * ## 为什么需要它
   *
   * TD(0) 的目标是 `r_t + γ·V(s_{t+1})` —— 只有**一步**真实奖励，
   * 剩下的全靠当前（还很差的）价值估计。训练早期 V 基本是噪声，
   * 于是真实信号被稀释，信用分配只能一步一步往回渗。
   *
   * n-step 用 `r_t + γr_{t+1} + … + γⁿ⁻¹r_{t+n-1} + γⁿV(s_{t+n})`
   * 把 n 步真实奖励一次灌进去。2048 的奖励是**稀疏且局部**的
   * （分数只在合并时产生），真实奖励的密度正是 n-step 能帮上忙的地方。
   *
   * 代价是每步更新要重算 n 次评估，且因为奖励必须从真实轨迹里取，
   * 不能像 TD(0) 那样只留前一步 —— 所以重放路径要维护一个 n 长的环形缓冲。
   * 这部分实现见 trainer.cpp 的 ApplyGame。
   *
   * 取 1 是保守默认（与优化前逐位一致）；3~5 是通常的甜点区。
   */
  int nstep = 1;
  std::uint64_t seed = 12345;
  int difficulty = 1;  // 与 core::Difficulty 对齐（1 = normal 标准规则）

  /**
   * 走子策略的探索率：以 ε 的概率随机走，否则按当前网络贪心走。
   *
   * **必须用网络自己的策略，不能纯随机。** 踩过这个坑：
   * 纯随机走子 100 步就死、局均约 1,000 分，盘面分布太窄 ——
   * 网络只学到"烂盘面值 1 分"，学不到"好盘面值多少"，
   * 于是贪心评估始终停在 1,000 分上下，训练 200 局没有任何提升。
   *
   * 自对弈让策略与价值互相抬升：网络变好 → 走得更远 → 见到更好的盘面
   * → 学到更高的价值。这是 TD 学习能work的前提。
   *
   * ε 从 1.0 衰减到 epsilon_end：先充分探索，后期收敛到贪心。
   *
   * ## 为什么是**指数**衰减到固定局数，而不是按总局数线性衰减
   *
   * 原先用「按 games 线性衰减」，毛病是衰减速率取决于总跑多少局。
   * 跑 300 万局时，到 40 万局 ε 还有 0.88 —— 也就是**九成训练时间
   * 都花在接近随机的走子上**，而低 ε 的强对局才是真正长棋力的数据。
   * 换个总局数探索预算就完全不同，超参之间也没法横向对照。
   *
   * 指数衰减在 epsilon_decay_games 局时降到 epsilon_end，之后保持。
   * 无论总跑多少局，"在哪里衰减完"是固定的，可以直接对比。
   */
  double epsilon_start = 1.0;
  double epsilon_end = 0.1;

  /**
   * ε 衰减到 epsilon_end 所需的局数。0 = 退回旧行为（按 games 线性衰减）。
   *
   * 30 万是"够探索、又不把预算浪费在随机走子上"的起点。
   * 换网络规模时这个值要跟着调：容量越大越需要样本才能填满。
   */
  int epsilon_decay_games = 300000;

  /** 每隔多少局评估一次（评估用**贪心**策略，看网络有没有真的变强）。 */
  int eval_every = 100;
  int eval_games = 20;
  /** 每局最多走多少步，防止出现不死的循环。 */
  int max_steps_per_game = 6000;

  /**
   * 并行线程数。<=0 表示用满硬件线程（std::thread::hardware_concurrency）。
   *
   * 默认值是 1，不是"用满核"：单元测试要的是可复现的慢，不是快。
   * CLI 的 `train` 会显式传 0（用满核）—— 训练是唯一的重度用户。
   *
   * ## 并行是**确定性**的
   *
   * 每局的种子与它收集到的走子序列只取决于局号，与线程调度无关；
   * 而梯度更新全部在收集结束之后、由主线程按局号顺序串行执行。
   * 所以 threads=1 与 threads=12 得到**逐位相同**的权重，
   * 改变线程数只是为了快。这个性质必须保住 ——
   * 否则"这次分数涨了"到底是算法变好还是调度撞运气就说不清了。
   */
  int threads = 1;

  /**
   * 每批收集多少局。
   *
   * 一批之内所有线程只**读**网络（评估走子），批末由主线程统一写梯度，
   * 因此不需要任何锁。批量越大，线程启停的开销摊得越薄；
   * 但一批的轨迹要全部驻留内存，太大就会吃掉几个 GB。
   * 512 局的轨迹约 470 MB —— 够摊薄开销，又不至于挤压权重表。
   */
  int batch_games = 512;
};

/** 一次评估的结果。 */
struct EvalResult {
  double mean_score = 0.0;
  int best_score = 0;
  int reach_256 = 0;
  int reach_512 = 0;
  int reach_1024 = 0;
};

/** 训练过程中的一行日志。 */
struct TrainLog {
  int games_done = 0;
  EvalResult eval;              // 用当前网络贪心走子的成绩
  double mean_train_score = 0;  // 训练期间（随机走子）的平均分
};

/**
 * 训练。
 *
 * @param network 待训练的网络（会被就地修改）
 * @param config  配置
 * @param log     每次评估回调（用于打印进度）
 */
void Train(ValueNetwork* network, const TrainConfig& config,
           const std::function<void(const TrainLog&)>& log);

/**
 * 用当前网络**贪心**走子跑若干局，返回成绩。
 *
 * 这是判断"网络有没有在学"的唯一硬指标：训练用的随机走子分数不能说明问题，
 * 网络自己选路走出来的分数才行。
 *
 * @param threads <=0 表示用满硬件线程。切分方式是按下标连续分段，
 *   所以任何线程数下的成绩都逐位相同（见 TrainConfig::threads）。
 */
[[nodiscard]] EvalResult EvaluateGreedy(const ValueNetwork& network, int games, std::uint64_t seed,
                                        int difficulty, int max_steps, int threads = 1);

}  // namespace ai2048::learn

#endif  // AI2048_AI_LEARN_TRAINER_H_
