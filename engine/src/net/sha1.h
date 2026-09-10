// SHA-1 与 Base64 —— 只服务于 WebSocket 握手。
//
// 为什么手写这两个：
//   RFC 6455 的握手需要 `base64(sha1(key + GUID))`。为此拉一个加密库（OpenSSL 等）
//   不成比例 —— 这两个算法各几十行，而且**有官方测试向量可以验证**，
//   属于"手写也可靠"的那一类（与 JSON 不同，那个转义/数字格式的坑太多）。
//
// 注意：SHA-1 在这里**不是安全用途**，只是握手要求的哈希。不要用于任何安全场景。

#ifndef AI2048_NET_SHA1_H_
#define AI2048_NET_SHA1_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace ai2048::net {

// 20 字节的 SHA-1 摘要。
[[nodiscard]] std::string Sha1Raw(std::string_view input);

// 小写十六进制表示的 SHA-1。便于与测试向量对照。
[[nodiscard]] std::string Sha1Hex(std::string_view input);

// 标准 Base64 编码（带 '=' 填充，不换行）。
[[nodiscard]] std::string Base64Encode(std::string_view input);

}  // namespace ai2048::net

#endif  // AI2048_NET_SHA1_H_
