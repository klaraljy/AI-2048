#include "net/protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>

#include "core/board.h"
#include "net/json.h"
#include "net/websocket.h"

namespace ai2048::net {

namespace {

// json 是命名空间，所以要用命名空间别名而不是 using 声明
namespace json = ai2048::net::json;
using json::Value;

[[nodiscard]] std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

[[nodiscard]] std::optional<ai2048::Direction> DirectionFromName(const std::string& name) {
  const std::string lower = ToLower(name);
  if (lower == "up") return ai2048::Direction::kUp;
  if (lower == "down") return ai2048::Direction::kDown;
  if (lower == "left") return ai2048::Direction::kLeft;
  if (lower == "right") return ai2048::Direction::kRight;
  return std::nullopt;
}

/**
 * 把请求里的数值棋盘转成指数棋盘。
 *
 * 前端的 board 是**数值**（0 / 2 / 4 / 8 …），而引擎内部用指数。
 * 这里的转换必须是严格的：非法值一律报错，不做"猜一个接近的值"这种事 ——
 * 静默修数据会让两边理解悄悄分叉（参考原型的规则漂移就是这么开始的）。
 */
[[nodiscard]] bool ParseBoard(const Value& board_value, std::uint64_t* out, std::string* error) {
  if (!board_value.IsArray()) {
    *error = "board 必须是数组";
    return false;
  }
  const auto& rows = board_value.AsArray();
  if (rows.size() != static_cast<std::size_t>(ai2048::kBoardSize)) {
    *error = "board 必须有 4 行";
    return false;
  }

  std::uint64_t board = 0;
  for (int row = 0; row < ai2048::kBoardSize; ++row) {
    const Value& row_value = rows[static_cast<std::size_t>(row)];
    if (!row_value.IsArray()) {
      *error = "board 的每一行必须是数组";
      return false;
    }
    const auto& cells = row_value.AsArray();
    if (cells.size() != static_cast<std::size_t>(ai2048::kBoardSize)) {
      *error = "board 的每一行必须有 4 格";
      return false;
    }

    for (int col = 0; col < ai2048::kBoardSize; ++col) {
      const Value& cell = cells[static_cast<std::size_t>(col)];
      if (!cell.IsNumber()) {
        *error = "board 的格必须是数字";
        return false;
      }
      const double raw = cell.AsNumber();
      if (raw < 0.0) {
        *error = "board 的格不能是负数";
        return false;
      }

      int exponent = 0;
      if (raw != 0.0) {
        const auto value = static_cast<std::uint64_t>(raw);
        // 必须正好相等：2.5 这种要被拒绝，而不是截断成 2
        if (static_cast<double>(value) != raw) {
          *error = "board 的格必须是整数";
          return false;
        }
        exponent = ai2048::ValueToExponent(value);
        if (exponent < 0) {
          *error = "board 的格必须是 0 或 2 的幂";
          return false;
        }
        // 越界要报错，不静默 clamp（原型就是静默 clamp 的，那会让
        // AI 模拟的规则与现实规则悄悄错位）
        if (exponent > ai2048::kMaxExponent) {
          *error = "board 的格超过引擎上限（最大 " +
                   std::to_string(ai2048::ExponentToValue(ai2048::kMaxExponent)) + "）";
          return false;
        }
      }
      board = ai2048::SetExponent(board, row * ai2048::kBoardSize + col, exponent);
    }
  }

  *out = board;
  return true;
}

/** 把请求里的 config 合并进会话配置。只认已知键，未知键忽略（便于前端先行扩展）。 */
void ApplyConfig(const Value& config, ai2048::SearchConfig* target) {
  if (!config.IsObject()) return;

  const auto int_or = [&](std::string_view key, int fallback) {
    return static_cast<int>(config.GetInt(key, fallback));
  };
  const auto num_or = [&](std::string_view key, double fallback) {
    return config.GetNumber(key, fallback);
  };

  target->base_depth = std::clamp(int_or("baseDepth", target->base_depth), 2, 20);
  target->min_depth = std::clamp(int_or("minDepth", target->min_depth), 2, 20);
  target->max_depth = std::clamp(int_or("maxDepth", target->max_depth), 2, 24);
  if (target->min_depth > target->max_depth) std::swap(target->min_depth, target->max_depth);

  target->time_budget_ms = std::max(0, int_or("timeBudgetMs", target->time_budget_ms));
  target->chance_sample_limit = std::max(0, int_or("chanceSampleLimit", target->chance_sample_limit));

  const Value* weights = config.Find("weights");
  if (weights != nullptr && weights->IsObject()) {
    ai2048::Weights& w = target->weights;
    w.empty = static_cast<float>(num_or("empty", w.empty));
    w.empty_late = static_cast<float>(num_or("emptyLate", w.empty_late));
    w.monotonicity = static_cast<float>(num_or("monotonicity", w.monotonicity));
    w.smoothness = static_cast<float>(num_or("smoothness", w.smoothness));
    w.merge = static_cast<float>(num_or("merge", w.merge));
    w.corner = static_cast<float>(num_or("corner", w.corner));
    w.snake = static_cast<float>(num_or("snake", w.snake));
    w.max_tile = static_cast<float>(num_or("maxTile", w.max_tile));
    w.corner_control = static_cast<float>(num_or("cornerControl", w.corner_control));
    w.edge_support = static_cast<float>(num_or("edgeSupport", w.edge_support));
    w.gradient = static_cast<float>(num_or("gradient", w.gradient));
    w.anchor_bias = static_cast<float>(num_or("anchorBias", w.anchor_bias));
  }
}

/**
 * 把一段**已序列化的 JSON 文本**嵌进报文，而不是当成字符串再转义一遍。
 *
 * 踩过的坑：BuildDebugInfo 返回的是 Serialize 之后的字符串，
 * 直接 Value(std::string) 会得到 "debugInfo":"{\"aiType\":...}" ——
 * 双重序列化，前端 JSON.parse 之后拿到的是一个字符串而不是对象，
 * 所有 debugInfo.xxx 全是 undefined。这种错误在 C++ 侧完全看不出来，
 * 只有真的把回包原文打出来才会暴露。
 */
[[nodiscard]] Value EmbedJson(const std::string& json_text) {
  const json::ParseResult parsed = json::Parse(json_text);
  if (parsed.ok) return parsed.value;
  // 解析不了就当普通字符串（不该发生，但别让一条 debug 信息拖垮整个响应）
  return Value(json_text);
}

/** 组装 debugInfo。前端只用它显示，字段多了不影响兼容。 */
[[nodiscard]] std::string BuildDebugInfo(const ai2048::SearchResult& result,
                                        const ai2048::SearchConfig& config, bool timed_out) {
  Value info;
  info.Set("aiType", Value(std::string("expectimax")));
  info.Set("baseDepth", Value(config.base_depth));
  info.Set("searchDepth", Value(result.stats.reached_depth));
  info.Set("nodes", Value(static_cast<double>(result.stats.nodes)));
  info.Set("chanceNodes", Value(static_cast<double>(result.stats.chance_nodes)));
  info.Set("cacheSize", Value(static_cast<double>(result.stats.tt_stores)));
  info.Set("cacheHits", Value(static_cast<double>(result.stats.tt_hits)));
  info.Set("prunedByProbability",
           Value(static_cast<double>(result.stats.pruned_by_probability)));
  info.Set("timeCostMs", Value(result.stats.elapsed_ms));
  info.Set("timedOut", Value(timed_out || result.stats.timed_out));

  Value evaluations;
  for (const ai2048::MoveEvaluation& evaluation : result.evaluations) {
    Value item;
    item.Set("direction", Value(std::string(ai2048::DirectionName(evaluation.direction))));
    item.Set("legal", Value(evaluation.legal));
    item.Set("totalScore", Value(static_cast<double>(evaluation.total_score)));
    item.Set("futureScore", Value(static_cast<double>(evaluation.future_score)));
    item.Set("quickEstimate", Value(static_cast<double>(evaluation.quick_estimate)));
    evaluations.Push(std::move(item));
  }
  info.Set("evaluatedMoves", std::move(evaluations));

  return json::Serialize(info);
}

}  // namespace

ProtocolHandler::ProtocolHandler(SocketServer* server, std::string* /*log_prefix*/)
    : server_(server) {}

void ProtocolHandler::OnOpen(ConnectionId id) {
  // 用 try_emplace 原地构造：Session 内含不可移动的 TranspositionTable，
  // 所以不能先默认构造再赋值。
  sessions_.try_emplace(id);
}

void ProtocolHandler::OnClose(ConnectionId id) { sessions_.erase(id); }

bool ProtocolHandler::OnData(ConnectionId id, const char* data, std::size_t length) {
  const auto it = sessions_.find(id);
  if (it == sessions_.end()) return false;
  Session& session = it->second;

  session.buffer.append(data, length);

  if (session.phase == Session::Phase::kHandshake) {
    return HandleHandshake(id, &session);
  }
  HandleFrames(id, &session);
  return sessions_.find(id) != sessions_.end();
}

bool ProtocolHandler::HandleHandshake(ConnectionId id, Session* session) {
  // 等完整的请求头
  const std::size_t header_end = session->buffer.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    // 防止恶意/异常的超长请求头把内存撑爆
    if (session->buffer.size() > 64 * 1024) {
      static_cast<void>(server_->Send(id, BuildHttpError(431, "Request Header Fields Too Large")));
      return false;
    }
    return true;  // 继续等
  }

  const std::string head = session->buffer.substr(0, header_end + 4);
  const UpgradeRequest request = ParseUpgradeRequest(head);

  if (!request.valid) {
    std::cout << "  握手失败：" << request.error << "\n";
    static_cast<void>(server_->Send(id, BuildHttpError(400, "Bad Request")));
    return false;
  }

  static_cast<void>(server_->Send(id, BuildUpgradeResponse(ComputeAcceptKey(request.sec_websocket_key))));

  // 关键：握手请求之后可能**已经跟着第一帧**（客户端常常一起发），
  // 所以要把头部之后剩下的字节保留下来交给帧解析，不能丢弃。
  session->buffer.erase(0, header_end + 4);
  session->phase = Session::Phase::kOpen;

  std::cout << "  客户端已连接（路径 " << request.path << "）\n";

  if (!session->buffer.empty()) {
    HandleFrames(id, session);
  }
  return sessions_.find(id) != sessions_.end();
}

void ProtocolHandler::HandleFrames(ConnectionId id, Session* session) {
  for (;;) {
    WsFrame frame;
    std::string error;
    const WsDecodeStatus status = DecodeFrame(&session->buffer, &frame, &error);

    if (status == WsDecodeStatus::kIncomplete) return;

    if (status == WsDecodeStatus::kProtocolError || status == WsDecodeStatus::kTooLarge) {
      std::cout << "  协议错误：" << error << "\n";
      static_cast<void>(server_->Send(id, EncodeClose(1002)));  // 1002 = protocol error
      server_->CloseConnection(id);
      return;
    }

    switch (frame.opcode) {
      case WsOpcode::kText:
        HandleMessage(id, session, frame.payload);
        if (sessions_.find(id) == sessions_.end()) return;
        break;

      case WsOpcode::kPing:
        static_cast<void>(server_->Send(id, EncodeFrame(WsOpcode::kPong, frame.payload)));
        break;

      case WsOpcode::kClose: {
        std::uint16_t code = 1000;
        std::string reason;
        static_cast<void>(ParseClosePayload(frame.payload, &code, &reason));
        static_cast<void>(server_->Send(id, EncodeClose(code)));
        server_->CloseConnection(id);
        return;
      }

      case WsOpcode::kPong:
        break;  // 忽略

      default:
        static_cast<void>(server_->Send(id, EncodeClose(1002)));
        server_->CloseConnection(id);
        return;
    }
  }
}

void ProtocolHandler::HandleMessage(ConnectionId id, Session* session, const std::string& text) {
  // 只接受对象型报文
  const json::ParseResult parsed = json::Parse(text);
  if (!parsed.ok) {
    SendError(id, 0, "JSON 解析失败：" + parsed.error);
    ++session->errors;
    return;
  }
  const Value& request = parsed.value;
  if (!request.IsObject()) {
    SendError(id, 0, "报文必须是 JSON 对象");
    ++session->errors;
    return;
  }

  const std::int64_t request_id = request.GetInt("id", 0);
  const std::string type = request.GetString("type");
  const Value* payload = request.Find("payload");

  ++session->requests;

  if (type == "configure") {
    if (payload != nullptr) {
      const Value* config = payload->Find("config");
      if (config != nullptr) ApplyConfig(*config, &session->config);
    }
    SendResult(id, request_id, "{\"ok\":true}");
    return;
  }

  if (type == "best-move") {
    if (payload == nullptr) {
      SendError(id, request_id, "best-move 缺少 payload");
      ++session->errors;
      return;
    }
    const Value* state = payload->Find("state");
    const Value* board_value = state == nullptr ? nullptr : state->Find("board");
    if (board_value == nullptr) {
      SendError(id, request_id, "best-move 缺少 payload.state.board");
      ++session->errors;
      return;
    }

    std::uint64_t board = 0;
    std::string board_error;
    if (!ParseBoard(*board_value, &board, &board_error)) {
      SendError(id, request_id, "棋盘不合法：" + board_error);
      ++session->errors;
      return;
    }

    // lastMove 用于抑制来回摆动；前端传 null 表示无上一步
    std::optional<ai2048::Direction> last_move;
    if (const Value* options = payload->Find("options")) {
      const Value* last = options->Find("lastMove");
      if (last != nullptr && last->IsString()) {
        last_move = DirectionFromName(last->AsString());
      }
    }

    const ai2048::SearchResult result =
        ai2048::SearchBestMove(board, session->config, &session->table, last_move);

    Value response;
    // 只有"确实无合法走子"才是 null。
    // 其余任何情况（包括超时、内部异常路径）都必须给出一个合法方向 ——
    // 前端会拿 null 当作"AI 无步可走"并停止演示。
    if (result.move.has_value()) {
      response.Set("move", Value(std::string(ai2048::DirectionName(*result.move))));
    } else {
      response.Set("move", Value());
    }
    response.Set("debugInfo",
                 EmbedJson(BuildDebugInfo(result, session->config, result.stats.timed_out)));
    SendResult(id, request_id, json::Serialize(response));
    return;
  }

  if (type == "ping") {
    SendResult(id, request_id, "{\"ok\":true}");
    return;
  }

  SendError(id, request_id, "未知的 type: " + type);
  ++session->errors;
}

void ProtocolHandler::SendResult(ConnectionId id, std::int64_t request_id,
                                 const std::string& payload_json) {
  // payload 已经是序列化好的 JSON，这里手工拼接信封而不是再解析一次 ——
  // 少一次解析，也少一次出错机会。
  std::string message = "{\"id\":";
  message += std::to_string(request_id);
  message += ",\"type\":\"result\",\"payload\":";
  message += payload_json;
  message += "}";
  static_cast<void>(server_->Send(id, EncodeText(message)));
}

void ProtocolHandler::SendError(ConnectionId id, std::int64_t request_id,
                                const std::string& message_text) {
  Value payload;
  payload.Set("message", Value(message_text));
  std::string message = "{\"id\":";
  message += std::to_string(request_id);
  message += ",\"type\":\"error\",\"payload\":";
  message += json::Serialize(payload);
  message += "}";
  static_cast<void>(server_->Send(id, EncodeText(message)));
}

}  // namespace ai2048::net
