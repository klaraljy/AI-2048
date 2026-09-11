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
   * ε 从 1.0 线性衰减到 kFinalEpsilon：先充分探索，后期收敛到贪心。
   */
  double epsilon_start = 1.0;
  double epsilon_end = 0.1;

  /** 每隔多少局评估一次（评估用**贪心**策略，看网络有没有真的变强）。 */
  int eval_every = 100;
  int eval_games = 20;
  /** 每局最多走多少步，防止出现不死的循环。 */
  int max_steps_per_game = 6000;
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
 */
[[nodiscard]] EvalResult EvaluateGreedy(const ValueNetwork& network, int games, std::uint64_t seed,
                                        int difficulty, int max_steps);

}  // namespace ai2048::learn

#endif  // AI2048_AI_LEARN_TRAINER_H_
