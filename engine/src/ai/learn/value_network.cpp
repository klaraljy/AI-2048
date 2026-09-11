#include "ai/learn/value_network.h"

#include <cstdio>
#include <cstring>

namespace ai2048::learn {

namespace {

// 文件头：用一个短魔数 + 版本，避免把别的二进制当成权重读进来。
// 权重表会随 tuple 设计变化，版本号是**不兼容**的信号 —— 改了布局就升版本。
constexpr char kMagic[8] = {'A', '2', '0', '4', '8', 'N', 'T', '1'};

}  // namespace

std::uint32_t Tuple::Index(std::uint64_t board) const noexcept {
  // 4 个格子各 4 bit，拼成一个 16 bit 下标。
  // 用移位而不是乘法：cells 的顺序就是下标的位序，改顺序等于改权重表的含义。
  std::uint32_t index = 0;
  for (int i = 0; i < 4; ++i) {
    index |= static_cast<std::uint32_t>(GetExponent(board, cells[i])) << (4 * i);
  }
  return index;
}

ValueNetwork::ValueNetwork(std::vector<Tuple> tuples) : tuples_(std::move(tuples)) {
  if (tuples_.empty()) {
    // 没有 tuple 的网络输出恒为 0，那是**静默失效**：训练时 loss 不动、
    // 也不会报错，只会让人以为"学不动"。这里直接补一个空 tuple 之外的做法：
    // 保留一个占位 tuple，使参数非空，调用方能从 parameter_count() 看出来。
    tuples_.push_back(Tuple{});
  }
  weights_.assign(tuples_.size() * kTupleStates, 0.0F);
}

std::vector<Tuple> ValueNetwork::RowTuples() {
  // 每一行一个 tuple：(r,0) (r,1) (r,2) (r,3)，行优先共 4 个。
  std::vector<Tuple> tuples;
  tuples.reserve(4);
  for (int row = 0; row < kBoardSize; ++row) {
    Tuple tuple;
    for (int col = 0; col < kBoardSize; ++col) {
      tuple.cells[static_cast<std::size_t>(col)] = row * kBoardSize + col;
    }
    tuples.push_back(tuple);
  }
  return tuples;
}

void ValueNetwork::Reset() noexcept {
  std::memset(weights_.data(), 0, weights_.size() * sizeof(float));
}

ValueNetwork::Trace ValueNetwork::EvaluateWithTrace(std::uint64_t board) const noexcept {
  Trace trace;
  double sum = 0.0;
  for (std::size_t i = 0; i < tuples_.size(); ++i) {
    const std::uint32_t index = tuples_[i].Index(board);
    const std::uint32_t offset = static_cast<std::uint32_t>(i) * kTupleStates + index;
    sum += weights_[offset];
    if (trace.count < static_cast<int>(trace.offsets.size())) {
      trace.offsets[static_cast<std::size_t>(trace.count)] = offset;
      ++trace.count;
    }
  }
  trace.value = sum;
  return trace;
}

double ValueNetwork::Evaluate(std::uint64_t board) const noexcept {
  double sum = 0.0;
  for (std::size_t i = 0; i < tuples_.size(); ++i) {
    const std::uint32_t index = tuples_[i].Index(board);
    sum += weights_[i * kTupleStates + index];
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
  for (Tuple& tuple : tuples) {
    ok = ok && std::fread(tuple.cells.data(), sizeof(int), tuple.cells.size(), file) ==
                   tuple.cells.size();
  }
  ok = ok && weight_count == static_cast<std::uint64_t>(tuple_count) * kTupleStates;
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
  return true;
}

}  // namespace ai2048::learn
