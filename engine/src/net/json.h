// 极小的 JSON 读写。
//
// ⚠️ **只覆盖本项目协议用到的子集**，不是通用 JSON 库。明确支持的：
//
//   - 对象、数组、字符串、数字、true / false / null
//   - 字符串的 \" \\ \/ \b \f \n \r \t 与 \uXXXX（含代理对）
//   - 数字按 double 解析（协议里的数都是小整数与毫秒）
//
// **不支持**（遇到会明确报错，不静默吞掉）：
//
//   - 重复键（后者覆盖前者是常见实现，但那是隐蔽的错误来源）
//   - 深度嵌套（上限 32 层，防止栈溢出）
//   - 尾随逗号、注释、NaN / Infinity（不是合法 JSON）
//
// 为什么自己写而不引库：本项目的 JSON 面极窄（见上），
// 而这块的正确性可以**与 JSON.parse 逐例对拍**验证，
// 属于"手写也可靠"的一类。通用库会带来每次编译数秒的代价，收益不成比例。
// 这个判断与 AGENTS.md 的依赖策略并不冲突 —— 那里要求的是"评估"，不是"禁止"。

#ifndef AI2048_NET_JSON_H_
#define AI2048_NET_JSON_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ai2048::net::json {

class Value;

using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

class Value {
 public:
  Value() = default;
  explicit Value(bool value);
  explicit Value(double value);
  explicit Value(int value);
  explicit Value(std::string value);
  explicit Value(Array value);
  explicit Value(Object value);

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool IsNull() const noexcept { return type_ == Type::kNull; }
  [[nodiscard]] bool IsBool() const noexcept { return type_ == Type::kBool; }
  [[nodiscard]] bool IsNumber() const noexcept { return type_ == Type::kNumber; }
  [[nodiscard]] bool IsString() const noexcept { return type_ == Type::kString; }
  [[nodiscard]] bool IsArray() const noexcept { return type_ == Type::kArray; }
  [[nodiscard]] bool IsObject() const noexcept { return type_ == Type::kObject; }

  [[nodiscard]] bool AsBool(bool fallback = false) const noexcept;
  [[nodiscard]] double AsNumber(double fallback = 0.0) const noexcept;
  [[nodiscard]] std::int64_t AsInt(std::int64_t fallback = 0) const noexcept;
  [[nodiscard]] const std::string& AsString() const noexcept;

  [[nodiscard]] const Array& AsArray() const noexcept;
  [[nodiscard]] const Object& AsObject() const noexcept;

  // 取对象成员。不存在或类型不符时返回 nullptr，调用方自行决定默认值。
  [[nodiscard]] const Value* Find(std::string_view key) const noexcept;

  // 便捷取值：不存在或类型不符时返回 fallback
  [[nodiscard]] std::int64_t GetInt(std::string_view key, std::int64_t fallback = 0) const noexcept;
  [[nodiscard]] double GetNumber(std::string_view key, double fallback = 0.0) const noexcept;
  [[nodiscard]] bool GetBool(std::string_view key, bool fallback = false) const noexcept;
  [[nodiscard]] std::string GetString(std::string_view key, std::string_view fallback = "") const;

  // 追加到数组 / 插入到对象（构建响应用）
  void Push(Value value);
  void Set(std::string key, Value value);

 private:
  Type type_ = Type::kNull;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

/** 解析结果。失败时 error 说明位置与原因。 */
struct ParseResult {
  bool ok = false;
  Value value;
  std::string error;
};

[[nodiscard]] ParseResult Parse(std::string_view text);

/** 序列化。不可控的字符串会被正确转义。 */
[[nodiscard]] std::string Serialize(const Value& value);

}  // namespace ai2048::net::json

#endif  // AI2048_NET_JSON_H_
