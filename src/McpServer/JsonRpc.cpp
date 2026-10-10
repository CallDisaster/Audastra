#include "JsonRpc.h"

namespace War3Map::Mcp {

std::optional<JsonRpcRequest> ParseRequest(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::nullopt;
    }
    const QJsonObject obj = doc.object();
    if (obj.value("jsonrpc").toString() != "2.0") {
        return std::nullopt;
    }
    JsonRpcRequest request;
    request.method = obj.value("method").toString();
    if (request.method.isEmpty()) {
        return std::nullopt;
    }
    if (obj.contains("id")) {
        const auto id = obj.value("id");
        if (!id.isString() && !id.isDouble() && !id.isNull()) return std::nullopt;
        request.id = obj.value("id");
    }
    const QJsonValue paramsVal = obj.value("params");
    if (obj.contains("params") && !paramsVal.isObject()) return std::nullopt;
    if (paramsVal.isObject()) {
        request.params = paramsVal.toObject();
    }
    return request;
}

QByteArray SerializeResponse(const JsonRpcResponse& response) {
    QJsonObject obj;
    obj["jsonrpc"] = response.jsonrpc;
    obj["id"] = response.id;
    if (response.error.has_value()) {
        obj["error"] = *response.error;
    } else {
        obj["result"] = response.result;
    }
    return QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
}

} // namespace War3Map::Mcp
