#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

#include <optional>

namespace War3Map::Mcp {

/// JSON-RPC 2.0 请求
struct JsonRpcRequest {
    QString jsonrpc = "2.0";
    std::optional<QJsonValue> id; // null for notifications
    QString method;
    QJsonObject params;

    [[nodiscard]] bool IsNotification() const { return !id.has_value(); }
};

/// JSON-RPC 2.0 响应
struct JsonRpcResponse {
    QString jsonrpc = "2.0";
    QJsonValue id;
    QJsonValue result;
    std::optional<QJsonObject> error;

    static JsonRpcResponse Success(const QJsonValue& requestId, const QJsonValue& res) {
        JsonRpcResponse r;
        r.id = requestId;
        r.result = res;
        return r;
    }

    static JsonRpcResponse Error(const QJsonValue& requestId, int code, const QString& message,
                                  const QJsonValue& data = QJsonValue()) {
        JsonRpcResponse r;
        r.id = requestId;
        QJsonObject err;
        err["code"] = code;
        err["message"] = message;
        if (!data.isNull()) {
            err["data"] = data;
        }
        r.error = err;
        return r;
    }

    // 标准错误码
    static constexpr int kParseError = -32700;
    static constexpr int kInvalidRequest = -32600;
    static constexpr int kMethodNotFound = -32601;
    static constexpr int kInvalidParams = -32602;
    static constexpr int kInternalError = -32603;
};

/// 解析一行 JSON 为请求
std::optional<JsonRpcRequest> ParseRequest(const QByteArray& line);

/// 序列化响应为 JSON 行
QByteArray SerializeResponse(const JsonRpcResponse& response);

} // namespace War3Map::Mcp
