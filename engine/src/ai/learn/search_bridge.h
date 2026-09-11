// 把训练好的 n-tuple 价值网络接到搜索引擎上的适配器。
//
// ## 为什么单独一个头文件
//
// CLI（ai2048-cli）与服务端（ai2048-server）都要做同一件事：
// 把 learn::ValueNetwork 绑到 search.h 的 leaf_evaluator 函数指针上。
// 这段逻辑必须只有一份 —— 尤其是下面那条**尺度换算**，
// 两处各写一遍迟早会漂移，而漂移的后果不崩不报错，只是分数莫名下降。
//
// 放在头文件里（inline）而不是加进 ai2048_core：
// core 不许反向依赖 ai2048_train，那样会成环（见 search.h 的说明）。
// 谁需要谁就包含这个头，并把 ai2048_train 链接进来。

#ifndef AI2048_AI_LEARN_SEARCH_BRIDGE_H_
#define AI2048_AI_LEARN_SEARCH_BRIDGE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ai/learn/value_network.h"
#include "ai/search.h"

namespace ai2048::learn {

/**
 * search.h 的回调适配器。
 *
 * ⚠️ **必须乘回 kScoreScale。**
 *
 * 网络预测的是归一化分（分数/1000，量级 0~50），而搜索内部的评价值与
 * 手写启发式同量纲（原始分，量级上万）。不换算的话
 * consistency_bonus(40) / anti_oscillation_penalty(60) / direction_bias(16)
 * 会从"细微调节"变成"绝对主导"，搜索行为直接坏掉。
 *
 * 这类 bug 不崩、不报错，只会让分数莫名其妙地掉 —— 所以只允许有一份实现。
 *
 * @param context 指向 ValueNetwork 的指针（由调用方保证生命周期长于搜索）
 */
inline float EvaluateWithNetwork(void* context, std::uint64_t board, bool terminal) {
  const auto* network = static_cast<const ValueNetwork*>(context);
  return static_cast<float>(network->Evaluate(board, terminal) * ValueNetwork::kScoreScale);
}

/**
 * 按路径加载权重。
 *
 * 返回 nullopt **只表示加载失败**，不表示"没配"：没配路径时返回一个
 * 空的 shared_ptr。
 *
 * 失败必须是**致命**的，绝不能静默回退到手写启发式 ——
 * 那样用户以为在跑学习评估、实际跑的是手写启发式，而两者分数差 7 倍。
 * 这类"看起来在跑、其实换了实现"的静默降级比直接报错危险得多。
 *
 * @param path  权重文件路径。空 = 不加载。
 * @param error 失败时的原因（调用方负责打印）
 */
[[nodiscard]] inline std::optional<std::shared_ptr<ValueNetwork>> LoadNetworkFromFile(
    const std::string& path, std::string* error) {
  if (path.empty()) return std::shared_ptr<ValueNetwork>{};

  // 用**空**布局构造，让 Load 从文件里重建真正的 tuple 布局。
  // 曾经预分配 MixedTuples()，那是纯浪费：Load 会整体覆盖 tuples_，
  // 而各布局参数差距极大（mixed 约 3MB，six4 约 268MB）。
  auto network = std::make_shared<ValueNetwork>(std::vector<Tuple>{});
  if (!network->Load(path, error)) return std::nullopt;
  return network;
}

/** 把网络绑到搜索配置上。network 为空时保持手写启发式。 */
inline void AttachNetwork(const std::shared_ptr<ValueNetwork>& network, SearchConfig* config) {
  if (!network) return;
  config->leaf_evaluator = &EvaluateWithNetwork;
  config->leaf_evaluator_context = network.get();
}

}  // namespace ai2048::learn

#endif  // AI2048_AI_LEARN_SEARCH_BRIDGE_H_
