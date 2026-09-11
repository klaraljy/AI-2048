#include "ai/learn/value_network.h"

#include <cstdio>
#include <cstring>

namespace ai2048::learn {

namespace {

// 文件头：用一个短魔数 + 版本，避免把别的二进制当成权重读进来。
// 权重表会随 tuple 设计变化，版本号是**不兼容**的信号 —— 改了布局就升版本。
constexpr char kMagic[8] = {'A', '2', '0', '4', '8', 'N', 'T', '2'};

/** 16^length。length 最大 6，不会溢出 uint32。 */
[[nodiscard]] constexpr std::uint32_t Power16(int length) noexcept {
  std::uint32_t result = 1;
  for (int i = 0; i < length; ++i) result *= 16;
  return result;
}

}  // namespace

std::uint32_t Tuple::StateCount() const noexcept {
  return length <= 0 ? 0 : Power16(length);
}

std::uint32_t Tuple::Index(std::uint64_t board) const noexcept {
  // 每个格子 4 bit，按 cells 的顺序拼成下标。
  // **顺序就是权重表的位序** —— 改顺序等于改权重表的含义，
  // 会让已保存的权重全部失效（所以存盘格式里也存了 cells）。
  std::uint32_t index = 0;
  for (int i = 0; i < length; ++i) {
    index |= static_cast<std::uint32_t>(GetExponent(board, cells[static_cast<std::size_t>(i)]))
             << (4 * i);
  }
  return index;
}

Tuple MakeTuple(const std::vector<int>& cells) {
  Tuple tuple;
  const int count = static_cast<int>(cells.size());
  if (count > kMaxTupleLength) {
    // 截断而不是报错：调用方的意图仍然保留（前 kMaxTupleLength 格），
    // 但这是个编程错误，所以留个显眼的断言位置在调用方测试里查。
    tuple.length = kMaxTupleLength;
  } else {
    tuple.length = count;
  }
  for (int i = 0; i < tuple.length; ++i) {
    tuple.cells[static_cast<std::size_t>(i)] = cells[static_cast<std::size_t>(i)];
  }
  return tuple;
}

ValueNetwork::ValueNetwork(std::vector<Tuple> tuples) : tuples_(std::move(tuples)) {
  if (tuples_.empty()) {
    // 没有 tuple 的网络输出恒为 0，那是**静默失效**：训练时 loss 不动、
    // 也不会报错，只会让人以为"学不动"。放一个占位 tuple，
    // 让 parameter_count() 非零，调用方能看出问题。
    tuples_.push_back(MakeTuple({0, 1, 2, 3}));
  }

  offsets_.resize(tuples_.size());
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < tuples_.size(); ++i) {
    offsets_[i] = static_cast<std::uint32_t>(total);
    total += tuples_[i].StateCount();
  }
  weights_.assign(static_cast<std::size_t>(total), 0.0F);
}

std::vector<Tuple> ValueNetwork::RowTuples() {
  // 每一行一个 4-tuple，共 4 个。
  std::vector<Tuple> tuples;
  tuples.reserve(4);
  for (int row = 0; row < kBoardSize; ++row) {
    tuples.push_back(MakeTuple({
        row * kBoardSize + 0,
        row * kBoardSize + 1,
        row * kBoardSize + 2,
        row * kBoardSize + 3,
    }));
  }
  return tuples;
}

std::vector<Tuple> ValueNetwork::MixedTuples() {
  // 12 个 4-tuple，覆盖多种方向与形状。
  //
  // 设计意图（逐条都要能说出"它看到了别人看不到的什么"）：
  //   1-4  行：行内单调链 —— 2048 最基本的形状
  //   5-8  列：列内单调 —— 蛇形路径的另一半，行 tuple 看不到
  //   9-12 块与折线：2×2 方块、L 形、两条主对角 ——
  //        这些是"角上大牌 + 相邻递减"的直接证据
  //
  // 刻意**不用**随机挑格子：随机 tuple 在样本少时泛化更差，
  // 而结构化的形状本身就是关于游戏的知识（这正是 n-tuple 该编码的东西）。
  const std::vector<std::vector<int>> patterns = {
      // 行
      {0, 1, 2, 3},
      {4, 5, 6, 7},
      {8, 9, 10, 11},
      {12, 13, 14, 15},
      // 列
      {0, 4, 8, 12},
      {1, 5, 9, 13},
      {2, 6, 10, 14},
      {3, 7, 11, 15},
      // 2×2 方块（四个角各一个）
      {0, 1, 4, 5},
      {2, 3, 6, 7},
      {8, 9, 12, 13},
      {10, 11, 14, 15},
  };

  std::vector<Tuple> tuples;
  tuples.reserve(patterns.size());
  for (const std::vector<int>& pattern : patterns) {
    tuples.push_back(MakeTuple(pattern));
  }
  return tuples;
}

std::vector<Tuple> ValueNetwork::WithSixTuples(int six_tuple_count) {
  std::vector<Tuple> tuples = MixedTuples();
  if (six_tuple_count <= 0) return tuples;

  // 6-tuple：跨 6 格的形状。4-tuple 无论怎么摆都看不到这么长的结构，
  // 而"蛇形"恰好需要 6 格以上才能体现出来。
  //
  // 四条 6 格路径，分别从四个角出发，沿最长的蛇形走：
  //   从角出发沿边 4 格 + 折回来 2 格 —— 这是蛇形开头那一段的形状。
  const std::vector<std::vector<int>> six_patterns = {
      // 左上角出发：第一行 4 格 + 第二行前 2 格
      {0, 1, 2, 3, 4, 5},
      // 右上角出发：第一行倒着 + 第二行后 2 格
      {3, 2, 1, 0, 7, 6},
      // 左下角出发：最后一行 + 倒数第二行前 2 格
      {12, 13, 14, 15, 8, 9},
      // 右下角出发：最后一行倒着 + 倒数第二行后 2 格
      {15, 14, 13, 12, 11, 10},
  };

  const int take = std::min<int>(six_tuple_count, static_cast<int>(six_patterns.size()));
  for (int i = 0; i < take; ++i) {
    tuples.push_back(MakeTuple(six_patterns[static_cast<std::size_t>(i)]));
  }
  return tuples;
}

void ValueNetwork::Reset() noexcept { std::memset(weights_.data(), 0, weights_.size() * sizeof(float)); }

ValueNetwork::Trace ValueNetwork::EvaluateWithTrace(std::uint64_t board, bool terminal) const noexcept {
  Trace trace;
  // 终局：之后再也拿不到分，价值就是 0。**这是必须的信号** ——
  // 它让 TD 链知道"这条路的终点到了"，否则最后一步的价值会朝着
  // 自己的奖励收敛，偏置顺着链污染整张表（实测就是分数卡住不动）。
  if (terminal) return trace;

  double sum = 0.0;
  for (std::size_t i = 0; i < tuples_.size(); ++i) {
    const std::uint32_t offset = offsets_[i] + tuples_[i].Index(board);
    sum += weights_[offset];
    if (trace.count < static_cast<int>(trace.offsets.size())) {
      trace.offsets[static_cast<std::size_t>(trace.count)] = offset;
      ++trace.count;
    }
  }
  trace.value = sum;
  return trace;
}

double ValueNetwork::Evaluate(std::uint64_t board, bool terminal) const noexcept {
  if (terminal) return 0.0;
  double sum = 0.0;
  for (std::size_t i = 0; i < tuples_.size(); ++i) {
    sum += weights_[offsets_[i] + tuples_[i].Index(board)];
  }
  return sum;
}

void ValueNetwork::ApplyGradient(const Trace& trace, double delta, double learning_rate) noexcept {
  const float step = static_cast<float>(delta * learning_rate);
  for (int i = 0; i < trace.count; ++i) {
    weights_[trace.offsets[static_cast<std::size_t>(i)]] += step;
  }
}

bool ValueNetwork::Save(const std::string& path, std::string* error) const {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    *error = "无法写入 " + path;
    return false;
  }

  const auto tuple_count = static_cast<std::uint32_t>(tuples_.size());
  const auto weight_count = static_cast<std::uint64_t>(weights_.size());

  bool ok = std::fwrite(kMagic, 1, sizeof(kMagic), file) == sizeof(kMagic);
  ok = ok && std::fwrite(&tuple_count, sizeof(tuple_count), 1, file) == 1;
  ok = ok && std::fwrite(&weight_count, sizeof(weight_count), 1, file) == 1;
  // tuple 布局也要存：否则加载时不知道权重表对应的位置组合，
  // 会静默地把权重用错格子上（分数会变差但没有任何报错）。
  for (const Tuple& tuple : tuples_) {
    ok = ok && std::fwrite(&tuple.length, sizeof(tuple.length), 1, file) == 1;
    ok = ok && std::fwrite(tuple.cells.data(), sizeof(int), tuple.cells.size(), file) ==
                   tuple.cells.size();
  }
  ok = ok && std::fwrite(weights_.data(), sizeof(float), weights_.size(), file) == weights_.size();
  std::fclose(file);

  if (!ok) {
    *error = "写入 " + path + " 时出错";
    return false;
  }
  return true;
}

bool ValueNetwork::Load(const std::string& path, std::string* error) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    *error = "无法读取 " + path;
    return false;
  }

  char magic[sizeof(kMagic)] = {};
  std::uint32_t tuple_count = 0;
  std::uint64_t weight_count = 0;
  bool ok = std::fread(magic, 1, sizeof(magic), file) == sizeof(magic) &&
            std::memcmp(magic, kMagic, sizeof(kMagic)) == 0;
  if (!ok) {
    std::fclose(file);
    *error = path + " 不是本项目的权重文件（魔数不匹配）";
    return false;
  }

  ok = std::fread(&tuple_count, sizeof(tuple_count), 1, file) == 1 &&
       std::fread(&weight_count, sizeof(weight_count), 1, file) == 1;
  if (!ok || tuple_count == 0) {
    std::fclose(file);
    *error = path + " 的文件头不完整";
    return false;
  }

  std::vector<Tuple> tuples(tuple_count);
  std::uint64_t expected = 0;
  for (Tuple& tuple : tuples) {
    ok = ok && std::fread(&tuple.length, sizeof(tuple.length), 1, file) == 1;
    if (!ok || tuple.length <= 0 || tuple.length > kMaxTupleLength) {
      std::fclose(file);
      *error = path + " 的 tuple 长度非法";
      return false;
    }
    ok = ok && std::fread(tuple.cells.data(), sizeof(int), tuple.cells.size(), file) ==
                   tuple.cells.size();
    expected += tuple.StateCount();
  }
  ok = ok && weight_count == expected;
  if (!ok) {
    std::fclose(file);
    *error = path + " 的 tuple 布局与权重数量不一致";
    return false;
  }

  std::vector<float> weights(static_cast<std::size_t>(weight_count));
  ok = std::fread(weights.data(), sizeof(float), weights.size(), file) == weights.size();
  std::fclose(file);

  if (!ok) {
    *error = path + " 的权重数据不完整";
    return false;
  }

  tuples_ = std::move(tuples);
  weights_ = std::move(weights);

  offsets_.resize(tuples_.size());
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < tuples_.size(); ++i) {
    offsets_[i] = static_cast<std::uint32_t>(total);
    total += tuples_[i].StateCount();
  }
  return true;
}

}  // namespace ai2048::learn
