#include "McpServer.h"

#include "McpWorkspace.h"
#include "McpTerrain.h"
#include "AudastraVersion.generated.h"

#include "War3MapCore/EditorSession.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace War3Map::Mcp {
namespace {

std::optional<QString> ValidateJsonValue(
    const QJsonValue& value,
    const QJsonObject& schema,
    const QString& path) {
    const QString type = schema.value("type").toString();
    if (type == "object") {
        if (!value.isObject()) {
            return path + QStringLiteral(" must be an object");
        }
        const QJsonObject object = value.toObject();
        const QJsonObject properties = schema.value("properties").toObject();
        for (const QJsonValue& required : schema.value("required").toArray()) {
            const QString name = required.toString();
            if (name.isEmpty() || !object.contains(name)) {
                return path + QStringLiteral(" is missing required property '") +
                    name + QStringLiteral("'");
            }
        }
        if (schema.value("additionalProperties").isBool() &&
            !schema.value("additionalProperties").toBool()) {
            for (auto it = object.begin(); it != object.end(); ++it) {
                if (!properties.contains(it.key())) {
                    return path + QStringLiteral(" contains unknown property '") +
                        it.key() + QStringLiteral("'");
                }
            }
        }
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!properties.contains(it.key())) {
                continue;
            }
            auto nested = ValidateJsonValue(
                it.value(), properties.value(it.key()).toObject(),
                path + QStringLiteral(".") + it.key());
            if (nested.has_value()) {
                return nested;
            }
        }
    } else if (type == "string") {
        if (!value.isString()) {
            return path + QStringLiteral(" must be a string");
        }
        const QString pattern = schema.value("pattern").toString();
        if (!pattern.isEmpty()) {
            const QRegularExpression expression(pattern);
            if (!expression.isValid() ||
                !expression.match(value.toString()).hasMatch()) {
                return path + QStringLiteral(" does not match its required format");
            }
        }
    } else if (type == "integer") {
        if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
            std::floor(value.toDouble()) != value.toDouble()) {
            return path + QStringLiteral(" must be an integer");
        }
    } else if (type == "number" &&
        (!value.isDouble() || !std::isfinite(value.toDouble()))) {
        return path + QStringLiteral(" must be a finite number");
    } else if (type == "boolean" && !value.isBool()) {
        return path + QStringLiteral(" must be a boolean");
    } else if (type == "array" && !value.isArray()) {
        return path + QStringLiteral(" must be an array");
    }

    const QJsonArray allowed = schema.value("enum").toArray();
    if (!allowed.isEmpty()) {
        const bool matched = std::any_of(
            allowed.begin(), allowed.end(),
            [&value](const QJsonValue& candidate) { return candidate == value; });
        if (!matched) {
            return path + QStringLiteral(" is not an allowed value");
        }
    }
    return std::nullopt;
}

} // namespace

bool ToolPolicy::Allows(ToolPermission permission) const noexcept {
    switch (permission) {
    case ToolPermission::Query: return allowQuery;
    case ToolPermission::Edit: return allowEdit;
    case ToolPermission::Save: return allowSave;
    case ToolPermission::External: return allowExternal;
    }
    return false;
}

McpServer::McpServer() = default;
McpServer::~McpServer() = default;

bool McpServer::SupportsProtocolVersion(const QString& version) noexcept {
    return version == QStringLiteral("2024-11-05") || version == QStringLiteral("2025-03-26") ||
        version == QStringLiteral("2025-06-18") || version == QStringLiteral("2025-11-25");
}

void McpServer::SetWorkspace(const std::filesystem::path& projectRoot,
                              const std::filesystem::path& uiDataRoot) {
    projectRoot_ = projectRoot;
    uiDataRoot_ = uiDataRoot;
    terrain_.reset();
    workspace_.reset();
    documentId_.clear();
    initializationDiagnostics_.clear();
    if (terrainGeneration_ == (std::numeric_limits<std::uint64_t>::max)()) {
        initializationDiagnostics_ = Status::Failure("MCP", "MCP_DOCUMENT_GENERATION_EXHAUSTED",
            "目录会话打开代次已耗尽，请重新启动 MCP 服务。").diagnostics;
        return;
    }
    auto opened = McpWorkspace::Open(projectRoot, uiDataRoot,
        std::filesystem::path(qEnvironmentVariable("FORGE_MCP_WAR3_ROOT").toStdWString()));
    if (opened.Succeeded()) {
        workspace_ = std::move(*opened.value);
        documentId_ = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        terrain_ = std::make_unique<McpTerrain>(*workspace_, documentId_, ++terrainGeneration_);
    } else {
        initializationDiagnostics_ = std::move(opened.diagnostics);
    }
}

McpWorkspace* McpServer::Workspace() const noexcept {
    return workspace_.get();
}

EditorSession* McpServer::Session() const noexcept {
    return workspace_ == nullptr ? nullptr : workspace_->Session();
}

McpTerrain* McpServer::Terrain() const noexcept {
    return terrain_.get();
}

void McpServer::RegisterTool(ToolDefinition tool) {
    const QString name = tool.name;
    tools_.insert_or_assign(name, std::move(tool));
    NotifyToolsChanged();
}

bool McpServer::SetToolHandler(const QString& name, ToolHandler handler) {
    const auto found = tools_.find(name);
    if (found == tools_.end() || !handler) return false;
    found->second.handler = std::move(handler);
    return true;
}

bool McpServer::IsToolEnabled(const QString& name) const {
    const auto found = tools_.find(name);
    return found != tools_.end() && toolPolicy_.Allows(found->second.permission) &&
        !toolPolicy_.disabledTools.contains(name);
}

void McpServer::SetToolPolicy(ToolPolicy policy) {
    QSet<QString> before;
    for (const auto& [name, tool] : tools_) {
        (void)tool;
        if (IsToolEnabled(name)) before.insert(name);
    }
    toolPolicy_ = std::move(policy);
    QSet<QString> after;
    for (const auto& [name, tool] : tools_) {
        (void)tool;
        if (IsToolEnabled(name)) after.insert(name);
    }
    if (before != after) NotifyToolsChanged();
}

void McpServer::SetNotificationSink(std::function<void(const QByteArray&)> sink) {
    notificationSink_ = std::move(sink);
}

void McpServer::NotifyToolsChanged() {
    if (!initialized_ || !notificationSink_) return;
    notificationSink_(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
        {"method", "notifications/tools/list_changed"}}).toJson(QJsonDocument::Compact) + '\n');
}

void McpServer::RegisterResource(ResourceDefinition resource) {
    // 同一身份只保留一个实际来源；不得因重复注册返回旧占位。
    std::erase_if(resources_, [&resource](const ResourceDefinition& existing) {
        return existing.uri == resource.uri;
    });
    resources_.push_back(std::move(resource));
}

void McpServer::ClearResources() { resources_.clear(); }

void McpServer::RemoveResourcesByPrefix(const QString& prefix) {
    std::erase_if(resources_, [&prefix](const ResourceDefinition& resource) {
        return resource.uri.startsWith(prefix);
    });
}

std::optional<JsonRpcResponse> McpServer::HandleMessage(const QByteArray& line) {
    auto request = ParseRequest(line);
    if (!request.has_value()) {
        return JsonRpcResponse::Error(QJsonValue(), JsonRpcResponse::kParseError,
                                       "Parse error");
    }

    const auto& req = *request;
    const QJsonValue reqId = req.id.value_or(QJsonValue());

    // 通知不需要响应
    if (req.IsNotification()) {
        if (req.method == "notifications/initialized") {
            initialized_ = true;
        }
        return std::nullopt;
    }

    if (req.method == "initialize") {
        return HandleInitialize(req);
    }
    if (req.method == "tools/list") {
        return HandleToolsList(req);
    }
    if (req.method == "tools/call") {
        return HandleToolsCall(req);
    }
    if (req.method == "resources/list") {
        return HandleResourcesList(req);
    }
    if (req.method == "resources/read") {
        return HandleResourcesRead(req);
    }
    if (req.method == "ping") {
        return JsonRpcResponse::Success(reqId, QJsonObject());
    }

    return JsonRpcResponse::Error(reqId, JsonRpcResponse::kMethodNotFound,
                                   QStringLiteral("Method not found: %1").arg(req.method));
}

void McpServer::RunStdio() {
    // 设置 stdin/stdout 为二进制模式
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1) {
        std::fputs("MCP stdio binary mode initialization failed\n", stderr);
        return;
    }
#endif

    SetNotificationSink([](const QByteArray& notification) {
        std::fwrite(notification.constData(), 1, notification.size(), stdout);
        std::fflush(stdout);
    });

    QByteArray buffer;
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), stdin) != nullptr) {
        buffer.append(chunk);
        // 按行处理
        while (true) {
            const int newlinePos = buffer.indexOf('\n');
            if (newlinePos < 0) break;
            const QByteArray line = buffer.left(newlinePos).trimmed();
            buffer.remove(0, newlinePos + 1);
            if (line.isEmpty()) continue;

            auto response = HandleMessage(line);
            if (response.has_value()) {
                const QByteArray out = SerializeResponse(*response);
                std::fwrite(out.constData(), 1, out.size(), stdout);
                std::fflush(stdout);
            }
        }
    }
    SetNotificationSink({});
}

JsonRpcResponse McpServer::HandleInitialize(const JsonRpcRequest& req) {
    initialized_ = false;
    const QJsonValue reqId = req.id.value_or(QJsonValue());
    QJsonObject result;
    // 官方 lifecycle：支持请求版本时原样回应，否则返回所支持的版本。
    // 旧 stdio 未提供版本时保留既有默认。
    const QString requested = req.params.value("protocolVersion").toString();
    result["protocolVersion"] = requested.isEmpty() ? QStringLiteral("2024-11-05") :
        (SupportsProtocolVersion(requested) ? requested : QStringLiteral("2025-11-25"));

    QJsonObject capabilities;
    QJsonObject toolsCap;
    toolsCap["listChanged"] = static_cast<bool>(notificationSink_);
    capabilities["tools"] = toolsCap;
    QJsonObject resourcesCap;
    resourcesCap["subscribe"] = false;
    resourcesCap["listChanged"] = false;
    capabilities["resources"] = resourcesCap;
    result["capabilities"] = capabilities;

    QJsonObject serverInfo;
    serverInfo["name"] = "Audastra-MCP";
    serverInfo["version"] = QString::fromUtf8(BuildInfo::Version);
    result["serverInfo"] = serverInfo;
    result["instructions"] = QStringLiteral(
        "通过 resources/list 发现版本化 Audastra Skill；先读 forge://skills/manifest，"
        "按实际 tools/list 与 schema 使用触发器（T）编辑事务。工具开关可能改变；"
        "保存、编译和游戏验证是不同出口。");

    return JsonRpcResponse::Success(reqId, result);
}

JsonRpcResponse McpServer::HandleToolsList(const JsonRpcRequest& req) {
    const QJsonValue reqId = req.id.value_or(QJsonValue());
    QJsonArray toolsArray;
    std::vector<QString> names;
    names.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        (void)tool;
        if (IsToolEnabled(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        const auto& tool = tools_.at(name);
        QJsonObject toolObj;
        toolObj["name"] = tool.name;
        toolObj["description"] = tool.description;
        toolObj["inputSchema"] = tool.inputSchema;
        toolsArray.append(toolObj);
    }
    QJsonObject result;
    result["tools"] = toolsArray;
    return JsonRpcResponse::Success(reqId, result);
}

JsonRpcResponse McpServer::HandleToolsCall(const JsonRpcRequest& req) {
    const QJsonValue reqId = req.id.value_or(QJsonValue());
    if (req.params.size() != 2 || !req.params.value("name").isString() ||
        !req.params.value("arguments").isObject()) {
        return JsonRpcResponse::Error(
            reqId, JsonRpcResponse::kInvalidParams,
            "tools/call params must contain only string name and object arguments");
    }
    const QString toolName = req.params.value("name").toString();
    const QJsonObject arguments = req.params.value("arguments").toObject();

    auto it = tools_.find(toolName);
    if (it == tools_.end()) {
        return JsonRpcResponse::Error(reqId, JsonRpcResponse::kInvalidParams,
                                       QStringLiteral("Unknown tool: %1").arg(toolName));
    }
    if (!IsToolEnabled(toolName)) {
        return JsonRpcResponse::Error(reqId, JsonRpcResponse::kInvalidParams,
            QStringLiteral("工具已被宿主权限或单项开关禁用：%1").arg(toolName),
            QJsonObject{{"code", "MCP_TOOL_DISABLED"}, {"tool", toolName}});
    }

    if (it->second.maxRequestIdBytes != 0) {
        const auto encodedId = QJsonDocument(QJsonArray{reqId}).toJson(QJsonDocument::Compact);
        if (static_cast<std::size_t>(encodedId.size() - 2) > it->second.maxRequestIdBytes)
            return JsonRpcResponse::Error(QJsonValue(), JsonRpcResponse::kInvalidParams,
                "JSON-RPC request id exceeds this tool's pre-execution byte budget",
                QJsonObject{{"code", "MCP_TOOL_REQUEST_ID_BUDGET"}, {"tool", toolName}});
    }

    const auto schemaError = ValidateJsonValue(
        arguments, it->second.inputSchema, QStringLiteral("arguments"));
    if (schemaError.has_value()) {
        return JsonRpcResponse::Error(
            reqId, JsonRpcResponse::kInvalidParams, *schemaError);
    }

    try {
        QJsonValue toolResult = it->second.handler(arguments);
        QJsonObject imageContent;
        if (it->second.pngImageResult && toolResult.isObject()) {
            auto body = toolResult.toObject();
            if (body.contains("image")) {
                const auto image = body.take("image").toObject();
                const auto data = image.value("data").toString();
                const auto decoded = data.size() <= 6 * 1024 * 1024
                    ? QByteArray::fromBase64Encoding(data.toLatin1(), QByteArray::AbortOnBase64DecodingErrors)
                    : QByteArray::FromBase64Result{};
                if (!body.value("ok").toBool() || image.value("mime_type") != QJsonValue("image/png")
                    || image.value("encoding") != QJsonValue("base64") || data.isEmpty()
                    || !decoded || decoded.decoded.size() > 4 * 1024 * 1024
                    || !decoded.decoded.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) {
                    body = {{"ok", false}, {"diagnostics", QJsonArray{QJsonObject{{"code", "MCP_IMAGE_RESULT_INVALID"},
                        {"message", "宿主图像结果不是有界PNG，未交付图像。"}}}}};
                } else imageContent = {{"type", "image"}, {"mimeType", "image/png"}, {"data", data}};
                toolResult = body;
            }
        }
        QJsonObject result;
        QJsonArray content;
        QJsonObject textContent;
        textContent["type"] = "text";
        textContent["text"] = QString::fromUtf8(QJsonDocument(
            toolResult.isObject() ? toolResult.toObject() : QJsonObject{{"value", toolResult}}
        ).toJson(QJsonDocument::Compact));
        content.append(textContent);
        if (!imageContent.isEmpty()) content.append(imageContent);
        result["content"] = content;
        result["isError"] = toolResult.isObject() &&
            toolResult.toObject().value("ok").isBool() &&
            !toolResult.toObject().value("ok").toBool();
        auto response = JsonRpcResponse::Success(reqId, result);
        if (it->second.maxResponseBytes != 0 &&
            static_cast<std::size_t>(SerializeResponse(response).size()) > it->second.maxResponseBytes) {
            // content[0].text 在外层 JSON-RPC 再转义一次；仅检查工具内部 JSON 不足以约束传输。
            const QJsonObject failure{{"ok", false}, {"diagnostics", QJsonArray{QJsonObject{
                {"code", "MCP_TOOL_RESPONSE_BUDGET"}, {"message", "工具响应超过传输字节预算；请缩小查询页。"}}}}};
            textContent["text"] = QString::fromUtf8(QJsonDocument(failure).toJson(QJsonDocument::Compact));
            result["content"] = QJsonArray{textContent};
            result["isError"] = true;
            response = JsonRpcResponse::Success(reqId, result);
            if (static_cast<std::size_t>(SerializeResponse(response).size()) > it->second.maxResponseBytes)
                return JsonRpcResponse::Error(QJsonValue(), JsonRpcResponse::kInvalidParams,
                    "JSON-RPC request id exceeds this tool's response budget");
        }
        return response;
    } catch (const std::exception& ex) {
        QJsonObject result;
        QJsonArray content;
        QJsonObject textContent;
        textContent["type"] = "text";
        textContent["text"] = QStringLiteral("Error: %1").arg(ex.what());
        content.append(textContent);
        result["content"] = content;
        result["isError"] = true;
        return JsonRpcResponse::Success(reqId, result);
    }
}

JsonRpcResponse McpServer::HandleResourcesList(const JsonRpcRequest& req) {
    const QJsonValue reqId = req.id.value_or(QJsonValue());
    QJsonArray resArray;
    for (const auto& res : resources_) {
        QJsonObject resObj;
        resObj["uri"] = res.uri;
        resObj["name"] = res.name;
        resObj["description"] = res.description;
        resObj["mimeType"] = res.mimeType;
        resArray.append(resObj);
    }
    QJsonObject result;
    result["resources"] = resArray;
    return JsonRpcResponse::Success(reqId, result);
}

JsonRpcResponse McpServer::HandleResourcesRead(const JsonRpcRequest& req) {
    const QJsonValue reqId = req.id.value_or(QJsonValue());
    const QString uri = req.params.value("uri").toString();

    for (const auto& res : resources_) {
        if (res.uri == uri || (res.uri.endsWith('/') && uri.startsWith(res.uri))) {
            const QJsonValue data = res.handler(uri);
            QJsonObject result;
            QJsonArray contents;
            QJsonObject contentObj;
            contentObj["uri"] = uri;
            contentObj["mimeType"] = res.mimeType;
            contentObj["text"] = data.isString() ? data.toString() : QString::fromUtf8(QJsonDocument(
                data.isObject() ? data.toObject() : QJsonObject{{"value", data}}
            ).toJson(QJsonDocument::Indented));
            contents.append(contentObj);
            result["contents"] = contents;
            return JsonRpcResponse::Success(reqId, result);
        }
    }

    return JsonRpcResponse::Error(reqId, JsonRpcResponse::kInvalidParams,
                                   QStringLiteral("Resource not found: %1").arg(uri));
}

} // namespace War3Map::Mcp
