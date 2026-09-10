#include "net/websocket.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <sstream>

#include "net/sha1.h"

namespace ai2048::net {

namespace {

/** 大小写不敏感地比较（HTTP 头名是大小写不敏感的）。 */
[[nodiscard]] bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string Trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
  return std::string(text.substr(begin, end - begin));
}

}  // namespace

WsDecodeStatus DecodeFrame(std::string* buffer, WsFrame* frame, std::string* error) {
  if (buffer->size() < 2) return WsDecodeStatus::kIncomplete;

  const auto byte0 = static_cast<unsigned char>((*buffer)[0]);
  const auto byte1 = static_cast<unsigned char>((*buffer)[1]);

  frame->fin = (byte0 & 0x80u) != 0;
  const std::uint8_t rsv = static_cast<std::uint8_t>(byte0 & 0x70u);
  const auto opcode = static_cast<WsOpcode>(byte0 & 0x0Fu);
  const bool masked = (byte1 & 0x80u) != 0;
  std::uint64_t payload_length = byte1 & 0x7Fu;

  // 保留位必须为 0（没有协商任何扩展）
  if (rsv != 0) {
    *error = "RSV 位非零：未协商扩展";
    return WsDecodeStatus::kProtocolError;
  }

  // 客户端发来的帧必须带掩码
  if (!masked) {
    *error = "客户端帧缺少掩码";
    return WsDecodeStatus::kProtocolError;
  }

  // 分片与二进制帧不在本项目的子集里：明确拒绝，不静默忽略
  if (!frame->fin) {
    *error = "不支持分片帧";
    return WsDecodeStatus::kProtocolError;
  }
  switch (opcode) {
    case WsOpcode::kText:
    case WsOpcode::kClose:
    case WsOpcode::kPing:
    case WsOpcode::kPong:
      break;
    case WsOpcode::kBinary:
      *error = "不支持二进制帧";
      return WsDecodeStatus::kProtocolError;
    case WsOpcode::kContinuation:
      *error = "不支持分片帧（continuation）";
      return WsDecodeStatus::kProtocolError;
    default:
      *error = "未知 opcode";
      return WsDecodeStatus::kProtocolError;
  }

  std::size_t offset = 2;
  if (payload_length == 126) {
    if (buffer->size() < offset + 2) return WsDecodeStatus::kIncomplete;
    payload_length =
        (static_cast<std::uint64_t>(static_cast<unsigned char>((*buffer)[offset])) << 8) |
        static_cast<unsigned char>((*buffer)[offset + 1]);
    offset += 2;
  } else if (payload_length == 127) {
    if (buffer->size() < offset + 8) return WsDecodeStatus::kIncomplete;
    payload_length = 0;
    for (int i = 0; i < 8; ++i) {
      payload_length = (payload_length << 8) |
                       static_cast<unsigned char>((*buffer)[offset + static_cast<std::size_t>(i)]);
    }
    offset += 8;
  }

  if (payload_length > kMaxFramePayload) {
    *error = "帧过大";
    return WsDecodeStatus::kTooLarge;
  }

  // 掩码键 4 字节
  if (buffer->size() < offset + 4) return WsDecodeStatus::kIncomplete;
  const std::array<unsigned char, 4> mask = {
      static_cast<unsigned char>((*buffer)[offset]),
      static_cast<unsigned char>((*buffer)[offset + 1]),
      static_cast<unsigned char>((*buffer)[offset + 2]),
      static_cast<unsigned char>((*buffer)[offset + 3]),
  };
  offset += 4;

  const auto total = offset + static_cast<std::size_t>(payload_length);
  if (buffer->size() < total) return WsDecodeStatus::kIncomplete;

  frame->opcode = opcode;
  frame->payload.resize(static_cast<std::size_t>(payload_length));
  for (std::size_t i = 0; i < frame->payload.size(); ++i) {
    // unmask：payload[i] ^= mask[i % 4]。漏掉这一步请求体全是乱码。
    frame->payload[i] =
        static_cast<char>(static_cast<unsigned char>((*buffer)[offset + i]) ^ mask[i % 4]);
  }

  buffer->erase(0, total);
  return WsDecodeStatus::kOk;
}

std::string EncodeFrame(WsOpcode opcode, std::string_view payload) {
  std::string out;
  out.reserve(payload.size() + 10);

  out += static_cast<char>(0x80u | static_cast<std::uint8_t>(opcode));  // FIN + opcode

  const std::size_t length = payload.size();
  if (length < 126) {
    // 服务端发出的帧**不加掩码**，所以最高位是 0
    out += static_cast<char>(length);
  } else if (length <= 0xFFFF) {
    out += static_cast<char>(126);
    out += static_cast<char>((length >> 8) & 0xFFu);
    out += static_cast<char>(length & 0xFFu);
  } else {
    out += static_cast<char>(127);
    for (int i = 7; i >= 0; --i) {
      out += static_cast<char>((static_cast<std::uint64_t>(length) >> (i * 8)) & 0xFFu);
    }
  }

  out.append(payload);
  return out;
}

std::string EncodeText(std::string_view payload) { return EncodeFrame(WsOpcode::kText, payload); }

std::string EncodeClose(std::uint16_t code) {
  std::string payload;
  payload += static_cast<char>((code >> 8) & 0xFFu);
  payload += static_cast<char>(code & 0xFFu);
  return EncodeFrame(WsOpcode::kClose, payload);
}

bool ParseClosePayload(std::string_view payload, std::uint16_t* code, std::string* reason) {
  if (payload.size() < 2) return false;
  *code = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) << 8) |
      static_cast<unsigned char>(payload[1]));
  *reason = std::string(payload.substr(2));
  return true;
}

std::string ComputeAcceptKey(std::string_view sec_websocket_key) {
  const std::string combined = std::string(sec_websocket_key) + std::string(kWebSocketGuid);
  return Base64Encode(Sha1Raw(combined));
}

UpgradeRequest ParseUpgradeRequest(std::string_view raw_request) {
  UpgradeRequest request;

  const std::size_t header_end = raw_request.find("\r\n\r\n");
  if (header_end == std::string_view::npos) {
    request.error = "请求头不完整";
    return request;
  }

  const std::string_view head = raw_request.substr(0, header_end);
  std::istringstream stream{std::string(head)};
  std::string line;

  // 请求行：GET <path> HTTP/1.1
  if (!std::getline(stream, line)) {
    request.error = "缺少请求行";
    return request;
  }
  {
    std::istringstream request_line(line);
    std::string method;
    std::string path;
    std::string version;
    request_line >> method >> path >> version;
    if (!IEquals(method, "GET")) {
      request.error = "只支持 GET 升级";
      return request;
    }
    request.path = path;
  }

  bool has_upgrade = false;
  bool has_connection_upgrade = false;

  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;

    const std::string name = Trim(std::string_view(line).substr(0, colon));
    const std::string value = Trim(std::string_view(line).substr(colon + 1));

    if (IEquals(name, "Sec-WebSocket-Key")) {
      request.sec_websocket_key = value;
    } else if (IEquals(name, "Upgrade")) {
      has_upgrade = IEquals(value, "websocket");
    } else if (IEquals(name, "Connection")) {
      // 可能是 "Upgrade" 或 "keep-alive, Upgrade"，所以查子串
      std::string lower = value;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      has_connection_upgrade = lower.find("upgrade") != std::string::npos;
    }
  }

  if (!has_upgrade) {
    request.error = "缺少 Upgrade: websocket";
    return request;
  }
  if (!has_connection_upgrade) {
    request.error = "缺少 Connection: Upgrade";
    return request;
  }
  if (request.sec_websocket_key.empty()) {
    request.error = "缺少 Sec-WebSocket-Key";
    return request;
  }

  request.valid = true;
  return request;
}

std::string BuildUpgradeResponse(std::string_view accept_key) {
  std::string out;
  out += "HTTP/1.1 101 Switching Protocols\r\n";
  out += "Upgrade: websocket\r\n";
  out += "Connection: Upgrade\r\n";
  out += "Sec-WebSocket-Accept: ";
  out += accept_key;
  out += "\r\n\r\n";
  return out;
}

std::string BuildHttpResponse(int status, std::string_view reason, std::string_view content_type,
                              std::string_view body) {
  std::string out;
  out += "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
  out += "Content-Type: " + std::string(content_type) + "\r\n";
  // Content-Length 必须是**字节数**：说明页含中文，按字符数算会截断正文。
  out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  out += "Connection: close\r\n";
  out += "\r\n";
  out += body;
  return out;
}

std::string BuildHttpError(int status, std::string_view reason) {
  std::string body = std::to_string(status);
  body += " ";
  body += std::string(reason);
  body += "\n";
  return BuildHttpResponse(status, reason, "text/plain; charset=utf-8", body);
}

bool HasUpgradeHeader(std::string_view raw_request) {
  // 逐行找头名，不在整段文本里搜 "Upgrade" ——
  // 否则请求行或正文里偶然出现这个词就会误判。
  std::size_t pos = raw_request.find("\r\n");
  if (pos == std::string_view::npos) return false;
  pos += 2;

  while (pos < raw_request.size()) {
    const std::size_t line_end = raw_request.find("\r\n", pos);
    if (line_end == std::string_view::npos) break;
    if (line_end == pos) break;  // 空行 = 头结束

    const std::string_view line = raw_request.substr(pos, line_end - pos);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && IEquals(Trim(line.substr(0, colon)), "Upgrade")) {
      return true;
    }
    pos = line_end + 2;
  }
  return false;
}

}  // namespace ai2048::net
