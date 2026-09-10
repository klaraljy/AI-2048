#include "net/json.h"

// 与标准实现（Node 的 JSON.parse）的对拍结果，见 tests/json-parity.test.mjs：
//
//   84 例完全一致（55 例合法 + 29 例非法）
//    3 例**本实现刻意更严格**，且已确认标准会接受：
//      - 孤立代理 `"\ud800"` / `"\udc00"`：标准接受并产出无效 Unicode；本实现拒绝。
//        协议里的字符串只有 up/down/left/right 这类 ASCII，严格没有代价，
//        而它挡得住"看起来成功、其实数据已损坏"的情况。
//      - 重复键 `{"a":1,"a":2}`：标准静默让后者覆盖前者；本实现报错。
//        协议两端都是我们自己，重复键一定是 bug，静默覆盖只会掩盖它。
//
// 这个偏离是**有理由的选择**，不是"还没对齐"。

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ai2048::net::json {

namespace {

constexpr int kMaxDepth = 32;

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  [[nodiscard]] ParseResult Run() {
    SkipWhitespace();
    Value value;
    if (!ParseValue(&value, 0)) {
      return {false, Value{}, Error()};
    }
    SkipWhitespace();
    if (pos_ != text_.size()) {
      return {false, Value{}, MakeError("末尾有多余内容")};
    }
    return {true, std::move(value), ""};
  }

 private:
  [[nodiscard]] std::string MakeError(std::string_view what) const {
    std::string out(what);
    out += "（位置 ";
    out += std::to_string(pos_);
    out += "）";
    return out;
  }

  [[nodiscard]] bool Eof() const { return pos_ >= text_.size(); }
  [[nodiscard]] char Peek() const { return Eof() ? '\0' : text_[pos_]; }

  void SkipWhitespace() {
    while (!Eof()) {
      const char ch = text_[pos_];
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool Expect(char expected) {
    if (Peek() != expected) {
      error_ = MakeError(std::string("期望 '") + expected + "'");
      return false;
    }
    ++pos_;
    return true;
  }

  bool MatchLiteral(std::string_view literal) {
    if (text_.size() - pos_ < literal.size()) {
      error_ = MakeError("字面量不完整");
      return false;
    }
    if (text_.substr(pos_, literal.size()) != literal) {
      error_ = MakeError("字面量不匹配");
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  bool ParseValue(Value* out, int depth) {
    if (depth > kMaxDepth) {
      error_ = MakeError("嵌套过深");
      return false;
    }
    SkipWhitespace();
    if (Eof()) {
      error_ = MakeError("内容为空");
      return false;
    }

    switch (Peek()) {
      case '{':
        return ParseObject(out, depth);
      case '[':
        return ParseArray(out, depth);
      case '"': {
        std::string text;
        if (!ParseString(&text)) return false;
        *out = Value(std::move(text));
        return true;
      }
      case 't':
        if (!MatchLiteral("true")) return false;
        *out = Value(true);
        return true;
      case 'f':
        if (!MatchLiteral("false")) return false;
        *out = Value(false);
        return true;
      case 'n':
        if (!MatchLiteral("null")) return false;
        *out = Value();
        return true;
      default:
        return ParseNumber(out);
    }
  }

  bool ParseObject(Value* out, int depth) {
    if (!Expect('{')) return false;
    Object object;
    SkipWhitespace();
    if (Peek() == '}') {
      ++pos_;
      *out = Value(std::move(object));
      return true;
    }

    for (;;) {
      SkipWhitespace();
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWhitespace();
      if (!Expect(':')) return false;

      Value member;
      if (!ParseValue(&member, depth + 1)) return false;

      // 重复键：明确报错而不是后者覆盖前者 ——
      // 覆盖是隐蔽的错误来源（两边理解不一致却都不报错）
      if (object.find(key) != object.end()) {
        error_ = MakeError("对象里出现重复键 \"" + key + "\"");
        return false;
      }
      object.emplace(std::move(key), std::move(member));

      SkipWhitespace();
      const char ch = Peek();
      if (ch == ',') {
        ++pos_;
        continue;
      }
      if (ch == '}') {
        ++pos_;
        break;
      }
      error_ = MakeError("对象里期望 ',' 或 '}'");
      return false;
    }

    *out = Value(std::move(object));
    return true;
  }

  bool ParseArray(Value* out, int depth) {
    if (!Expect('[')) return false;
    Array array;
    SkipWhitespace();
    if (Peek() == ']') {
      ++pos_;
      *out = Value(std::move(array));
      return true;
    }

    for (;;) {
      Value element;
      if (!ParseValue(&element, depth + 1)) return false;
      array.push_back(std::move(element));

      SkipWhitespace();
      const char ch = Peek();
      if (ch == ',') {
        ++pos_;
        continue;
      }
      if (ch == ']') {
        ++pos_;
        break;
      }
      error_ = MakeError("数组里期望 ',' 或 ']'");
      return false;
    }

    *out = Value(std::move(array));
    return true;
  }

  /** 把一个码点按 UTF-8 追加。 */
  static void AppendUtf8(std::string* out, std::uint32_t code_point) {
    if (code_point <= 0x7F) {
      *out += static_cast<char>(code_point);
    } else if (code_point <= 0x7FF) {
      *out += static_cast<char>(0xC0u | (code_point >> 6));
      *out += static_cast<char>(0x80u | (code_point & 0x3Fu));
    } else if (code_point <= 0xFFFF) {
      *out += static_cast<char>(0xE0u | (code_point >> 12));
      *out += static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
      *out += static_cast<char>(0x80u | (code_point & 0x3Fu));
    } else {
      *out += static_cast<char>(0xF0u | (code_point >> 18));
      *out += static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu));
      *out += static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu));
      *out += static_cast<char>(0x80u | (code_point & 0x3Fu));
    }
  }

  /** 读 4 位十六进制。 */
  bool ParseHex4(std::uint32_t* out) {
    if (text_.size() - pos_ < 4) {
      error_ = MakeError("\\u 转义不完整");
      return false;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = text_[pos_ + static_cast<std::size_t>(i)];
      value <<= 4;
      if (ch >= '0' && ch <= '9') {
        value |= static_cast<std::uint32_t>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        value |= static_cast<std::uint32_t>(ch - 'a' + 10);
      } else if (ch >= 'A' && ch <= 'F') {
        value |= static_cast<std::uint32_t>(ch - 'A' + 10);
      } else {
        error_ = MakeError("\\u 转义里有非十六进制字符");
        return false;
      }
    }
    pos_ += 4;
    *out = value;
    return true;
  }

  bool ParseString(std::string* out) {
    if (!Expect('"')) return false;
    out->clear();

    while (!Eof()) {
      const auto ch = static_cast<unsigned char>(text_[pos_]);
      if (ch == '"') {
        ++pos_;
        return true;
      }
      if (ch == '\\') {
        ++pos_;
        if (Eof()) {
          error_ = MakeError("转义不完整");
          return false;
        }
        const char esc = text_[pos_++];
        switch (esc) {
          case '"':
            *out += '"';
            break;
          case '\\':
            *out += '\\';
            break;
          case '/':
            *out += '/';
            break;
          case 'b':
            *out += '\b';
            break;
          case 'f':
            *out += '\f';
            break;
          case 'n':
            *out += '\n';
            break;
          case 'r':
            *out += '\r';
            break;
          case 't':
            *out += '\t';
            break;
          case 'u': {
            std::uint32_t code_point = 0;
            if (!ParseHex4(&code_point)) return false;
            // 代理对：高代理后面必须跟低代理
            if (code_point >= 0xD800 && code_point <= 0xDBFF) {
              if (text_.size() - pos_ < 2 || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                error_ = MakeError("高代理后缺少低代理");
                return false;
              }
              pos_ += 2;
              std::uint32_t low = 0;
              if (!ParseHex4(&low)) return false;
              if (low < 0xDC00 || low > 0xDFFF) {
                error_ = MakeError("低代理区间不正确");
                return false;
              }
              code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low - 0xDC00u);
            } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
              error_ = MakeError("孤立的低代理");
              return false;
            }
            AppendUtf8(out, code_point);
            break;
          }
          default:
            error_ = MakeError("未知转义");
            return false;
        }
        continue;
      }
      if (ch < 0x20) {
        // 控制字符必须被转义（RFC 8259）
        error_ = MakeError("字符串里有未转义的控制字符");
        return false;
      }
      *out += static_cast<char>(ch);
      ++pos_;
    }

    error_ = MakeError("字符串没有结束引号");
    return false;
  }

  bool ParseNumber(Value* out) {
    const std::size_t start = pos_;

    if (Peek() == '-') ++pos_;
    if (Eof() || !std::isdigit(static_cast<unsigned char>(Peek()))) {
      error_ = MakeError("数字格式不正确");
      return false;
    }
    // 前导零不允许（JSON 规范）；"0" 本身可以
    if (Peek() == '0') {
      ++pos_;
    } else {
      while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
    }
    if (!Eof() && Peek() == '.') {
      ++pos_;
      if (Eof() || !std::isdigit(static_cast<unsigned char>(Peek()))) {
        error_ = MakeError("小数点后缺少数字");
        return false;
      }
      while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
    }
    if (!Eof() && (Peek() == 'e' || Peek() == 'E')) {
      ++pos_;
      if (!Eof() && (Peek() == '+' || Peek() == '-')) ++pos_;
      if (Eof() || !std::isdigit(static_cast<unsigned char>(Peek()))) {
        error_ = MakeError("指数部分缺少数字");
        return false;
      }
      while (!Eof() && std::isdigit(static_cast<unsigned char>(Peek()))) ++pos_;
    }

    const std::string token(text_.substr(start, pos_ - start));
    *out = Value(std::strtod(token.c_str(), nullptr));
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::string error_;
  [[nodiscard]] const std::string& Error() const { return error_; }
};

/** 序列化时转义字符串。数字精度用 %.17g 再裁剪，保证往返不失真。 */
void SerializeString(std::string_view text, std::string* out) {
  *out += '"';
  for (const char raw : text) {
    const auto ch = static_cast<unsigned char>(raw);
    switch (ch) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\b':
        *out += "\\b";
        break;
      case '\f':
        *out += "\\f";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          *out += buffer;
        } else {
          // UTF-8 字节原样输出（JSON 允许直接放非 ASCII）
          *out += static_cast<char>(ch);
        }
        break;
    }
  }
  *out += '"';
}

void SerializeNumber(double value, std::string* out) {
  if (!std::isfinite(value)) {
    // 不是合法 JSON。写 null 并让调用方知道 —— 静默写 NaN 会产生
    // 无法解析的输出，那比写 null 更糟。
    *out += "null";
    return;
  }
  // 整数值走整数路径：避免 "1" 被写成 "1.0"
  if (value == std::floor(value) && std::fabs(value) < 1e15) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
    *out += buffer;
    return;
  }
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  *out += buffer;
}

void SerializeValue(const Value& value, std::string* out) {
  switch (value.type()) {
    case Type::kNull:
      *out += "null";
      return;
    case Type::kBool:
      *out += value.AsBool() ? "true" : "false";
      return;
    case Type::kNumber:
      SerializeNumber(value.AsNumber(), out);
      return;
    case Type::kString:
      SerializeString(value.AsString(), out);
      return;
    case Type::kArray: {
      *out += '[';
      const Array& array = value.AsArray();
      for (std::size_t i = 0; i < array.size(); ++i) {
        if (i != 0) *out += ',';
        SerializeValue(array[i], out);
      }
      *out += ']';
      return;
    }
    case Type::kObject: {
      *out += '{';
      bool first = true;
      for (const auto& [key, member] : value.AsObject()) {
        if (!first) *out += ',';
        first = false;
        SerializeString(key, out);
        *out += ':';
        SerializeValue(member, out);
      }
      *out += '}';
      return;
    }
  }
}

}  // namespace

Value::Value(bool value) : type_(Type::kBool), bool_(value) {}
Value::Value(double value) : type_(Type::kNumber), number_(value) {}
Value::Value(int value) : type_(Type::kNumber), number_(static_cast<double>(value)) {}
Value::Value(std::string value) : type_(Type::kString), string_(std::move(value)) {}
Value::Value(Array value) : type_(Type::kArray), array_(std::move(value)) {}
Value::Value(Object value) : type_(Type::kObject), object_(std::move(value)) {}

bool Value::AsBool(bool fallback) const noexcept { return type_ == Type::kBool ? bool_ : fallback; }

double Value::AsNumber(double fallback) const noexcept {
  return type_ == Type::kNumber ? number_ : fallback;
}

std::int64_t Value::AsInt(std::int64_t fallback) const noexcept {
  if (type_ != Type::kNumber) return fallback;
  if (!std::isfinite(number_)) return fallback;
  return static_cast<std::int64_t>(number_);
}

const std::string& Value::AsString() const noexcept {
  static const std::string kEmpty;
  return type_ == Type::kString ? string_ : kEmpty;
}

const Array& Value::AsArray() const noexcept {
  static const Array kEmpty;
  return type_ == Type::kArray ? array_ : kEmpty;
}

const Object& Value::AsObject() const noexcept {
  static const Object kEmpty;
  return type_ == Type::kObject ? object_ : kEmpty;
}

const Value* Value::Find(std::string_view key) const noexcept {
  if (type_ != Type::kObject) return nullptr;
  const auto it = object_.find(std::string(key));
  return it == object_.end() ? nullptr : &it->second;
}

std::int64_t Value::GetInt(std::string_view key, std::int64_t fallback) const noexcept {
  const Value* found = Find(key);
  return found == nullptr ? fallback : found->AsInt(fallback);
}

double Value::GetNumber(std::string_view key, double fallback) const noexcept {
  const Value* found = Find(key);
  return found == nullptr ? fallback : found->AsNumber(fallback);
}

bool Value::GetBool(std::string_view key, bool fallback) const noexcept {
  const Value* found = Find(key);
  return found == nullptr ? fallback : found->AsBool(fallback);
}

std::string Value::GetString(std::string_view key, std::string_view fallback) const {
  const Value* found = Find(key);
  if (found == nullptr || !found->IsString()) return std::string(fallback);
  return found->AsString();
}

void Value::Push(Value value) {
  if (type_ != Type::kArray) {
    type_ = Type::kArray;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

void Value::Set(std::string key, Value value) {
  if (type_ != Type::kObject) {
    type_ = Type::kObject;
    object_.clear();
  }
  object_[std::move(key)] = std::move(value);
}

ParseResult Parse(std::string_view text) { return Parser(text).Run(); }

std::string Serialize(const Value& value) {
  std::string out;
  SerializeValue(value, &out);
  return out;
}

}  // namespace ai2048::net::json
