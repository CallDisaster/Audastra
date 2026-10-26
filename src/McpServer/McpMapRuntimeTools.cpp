#include "McpMapRuntimeTools.h"
#include "McpToolInternal.h"
#include <QJsonArray>
#include <QRegularExpression>

namespace War3Map::Mcp {
std::vector<ToolDefinition> MapRuntimeToolDefinitions() {
    const QJsonObject identity{{"project_identity", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
        {"expected_revision", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}}};
    std::vector<ToolDefinition> result;
    const auto add = [&](const char* name, const char* description, ToolPermission permission, QJsonObject properties, QJsonArray required) {
        ToolDefinition tool; tool.name = name; tool.description = QString::fromUtf8(description); tool.permission = permission;
        tool.inputSchema = BasicSchema(std::move(properties), std::move(required));
        tool.maxRequestIdBytes = 128; tool.maxResponseBytes = 64 * 1024;
        result.push_back(std::move(tool));
    };
    add("map_runtime_query", "查询本图运行库设置、本工程批准和可信包身份；不自动选包或批准原生代码。",
        ToolPermission::Query, identity, {});
    auto properties = identity;
    properties.insert("mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"automatic", "enabled", "disabled"}}});
    properties.insert("package_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}});
    properties.insert("approve_native_runtime", QJsonObject{{"type", "boolean"},
        {"description", "作者已明确授权本图使用所选原生运行库时才传true；不能用地图声明替代授权。"}});
    add("map_runtime_set", "按当前修订明确设置运行库策略，复用同一撤销事务；启用须明确批准固定包身份。不会保存或启动游戏。",
        ToolPermission::Edit, properties, {"project_identity", "expected_revision", "mode"});
    return result;
}

QJsonObject InvokeMapRuntimeTool(const QString& name, const QJsonObject& args, const MapRuntimeToolHost& host) {
    const bool write = name == QStringLiteral("map_runtime_set");
    if (!write && name != QStringLiteral("map_runtime_query")) return Failure("RUNTIME_TOOL_UNKNOWN", "未知的运行库工具。");
    QStringList allowed{"project_identity", "expected_revision"};
    if (write) allowed.append({"mode", "package_sha256", "approve_native_runtime"});
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!allowed.contains(it.key())) return Failure("RUNTIME_ARGUMENT_INVALID", "运行库工具不接受额外字段或本机包路径。");
    const bool bound = args.contains("project_identity") || args.contains("expected_revision");
    if (write || bound) {
        const auto project = args.value("project_identity");
        const auto revision = args.value("expected_revision");
        bool numeric = false; const auto revisionNumber = revision.toString().toULongLong(&numeric);
        if (!project.isString() || project.toString().isEmpty() || project.toString().size() > 128 || project.toString().contains(QChar::Null)
            || !revision.isString() || !numeric || revision.toString() != QString::number(revisionNumber))
            return Failure("RUNTIME_ARGUMENT_INVALID", "需要完整的活动工程身份与十进制修订身份。");
    }
    if (write) {
        const auto mode = args.value("mode").toString();
        if (mode != "automatic" && mode != "enabled" && mode != "disabled")
            return Failure("RUNTIME_ARGUMENT_INVALID", "未知的运行库策略。");
        if (args.contains("approve_native_runtime") && !args.value("approve_native_runtime").isBool())
            return Failure("RUNTIME_ARGUMENT_INVALID", "原生代码批准必须为明确布尔值。");
        if (mode == "disabled") {
            if (args.contains("package_sha256") || args.value("approve_native_runtime").toBool())
                return Failure("RUNTIME_ARGUMENT_INVALID", "关闭运行库时不得同时选择包或授予批准。");
        } else {
            if (!args.value("package_sha256").isString() || args.value("package_sha256").toString().size() != 64
                || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(args.value("package_sha256").toString()).hasMatch())
                return Failure("RUNTIME_ARGUMENT_INVALID", "启用必须提供查询返回的固定包SHA-256。");
            if (!args.value("approve_native_runtime").toBool())
                return Failure("RUNTIME_CONSENT_REQUIRED", "需要作者明确批准本图使用所选原生代码；查询和地图声明不授予批准。");
        }
    }
    if (!host.invoke) return Failure("RUNTIME_LIVE_HOST_REQUIRED", "请连接打开该地图的编辑器端口；独立stdio不能模拟工程批准。");
    return host.invoke(name, args);
}

void RegisterMapRuntimeTools(McpServer& server) {
    for (auto tool : MapRuntimeToolDefinitions()) {
        const auto name = tool.name;
        tool.handler = [name](const QJsonObject& args) { return InvokeMapRuntimeTool(name, args, {}); };
        server.RegisterTool(std::move(tool));
    }
}
} // namespace War3Map::Mcp
