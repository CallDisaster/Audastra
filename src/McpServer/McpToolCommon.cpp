#include "McpToolInternal.h"

#include <QtGlobal>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace War3Map::Mcp {
QJsonObject DiagnosticsJson(const std::vector<Diagnostic>& diagnostics) {
    QJsonArray values;
    for (const Diagnostic& diagnostic : diagnostics) {
        QString severity = "error";
        if (diagnostic.severity == DiagnosticSeverity::Info) {
            severity = "info";
        } else if (diagnostic.severity == DiagnosticSeverity::Warning) {
            severity = "warning";
        }
        values.append(QJsonObject{
            {"severity", severity},
            {"subsystem", QString::fromUtf8(diagnostic.subsystem)},
            {"code", QString::fromUtf8(diagnostic.code)},
            {"message", QString::fromUtf8(diagnostic.message)},
        });
    }
    return QJsonObject{{"ok", false}, {"diagnostics", values}};
}
void InsertMapObjectReferenceDiagnostics(QJsonObject& result, const std::vector<Diagnostic>& diagnostics) {
    const std::vector<Diagnostic> head(diagnostics.begin(), diagnostics.begin() + static_cast<std::ptrdiff_t>((std::min)(diagnostics.size(), std::size_t{3})));
    result.insert("map_object_reference_diagnostics", DiagnosticsJson(head).value("diagnostics"));
    if (diagnostics.size() > head.size())
        result.insert("map_object_reference_diagnostics_total", static_cast<qint64>(diagnostics.size()));
}

QJsonObject Failure(std::string code, std::string message) {
    return DiagnosticsJson(std::vector<Diagnostic>{Diagnostic{
        DiagnosticSeverity::Error,
        "MCP",
        std::move(code),
        std::move(message),
        std::nullopt,
    }});
}

QJsonObject Failure(const Status& status) {
    return DiagnosticsJson(status.diagnostics);
}

QJsonObject RevisionJson(const ProjectRevision& revision) {
    return QJsonObject{
        {"generation", QString::number(revision.generation)},
        {"source_sha256", QString::fromUtf8(revision.sourceSha256)},
        {"ui_catalog_sha256", QString::fromUtf8(revision.uiCatalogSha256)},
    };
}

bool IsLowerHexSha256(std::string_view value) {
    return value.size() == 64 && std::all_of(
        value.begin(), value.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

Result<std::uint64_t> ParseUnsignedId(
    const QJsonObject& object,
    const char* field,
    bool allowZero) {
    const QJsonValue value = object.value(QString::fromLatin1(field));
    if (!value.isString()) {
        return Result<std::uint64_t>::Failure(
            "MCP", "MCP_ID_MUST_BE_STRING",
            std::string(field) + " 必须是十进制字符串，避免 JSON 浮点破坏 64 位稳定 ID。");
    }
    const std::string text = value.toString().toStdString();
    if (text.empty()) {
        return Result<std::uint64_t>::Failure(
            "MCP", "MCP_ID_INVALID", std::string(field) + " 不能为空。");
    }
    std::uint64_t parsed = 0;
    const auto converted = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 10);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
        (!allowZero && parsed == 0)) {
        return Result<std::uint64_t>::Failure(
            "MCP", "MCP_ID_INVALID", std::string(field) + " 不是有效十进制 ID。");
    }
    return Result<std::uint64_t>::Success(parsed);
}

Result<ProjectRevision> ParseRevision(const QJsonObject& parameters) {
    const QJsonValue value = parameters.value("expected_revision");
    if (!value.isObject()) {
        return Result<ProjectRevision>::Failure(
            "MCP", "MCP_REVISION_REQUIRED",
            "每个事务操作都必须携带完整 expected_revision。");
    }
    const QJsonObject object = value.toObject();
    auto generation = ParseUnsignedId(object, "generation");
    if (!generation.Succeeded()) {
        Result<ProjectRevision> result;
        result.diagnostics = generation.diagnostics;
        return result;
    }
    const QJsonValue source = object.value("source_sha256");
    const QJsonValue catalog = object.value("ui_catalog_sha256");
    if (!source.isString() || !catalog.isString() ||
        !IsLowerHexSha256(source.toString().toStdString()) ||
        !IsLowerHexSha256(catalog.toString().toStdString())) {
        return Result<ProjectRevision>::Failure(
            "MCP", "MCP_REVISION_IDENTITY_INVALID",
            "expected_revision 必须包含 64 位小写十六进制 source_sha256 与 ui_catalog_sha256。");
    }
    ProjectRevision revision;
    revision.generation = *generation.value;
    revision.sourceSha256 = source.toString().toStdString();
    revision.uiCatalogSha256 = catalog.toString().toStdString();
    return Result<ProjectRevision>::Success(std::move(revision));
}

Result<std::int32_t> ParseInt32(
    const QJsonObject& object,
    const char* field) {
    const QJsonValue value = object.value(QString::fromLatin1(field));
    if (!value.isDouble()) {
        return Result<std::int32_t>::Failure(
            "MCP", "MCP_INTEGER_REQUIRED", std::string(field) + " 必须是整数。");
    }
    const double raw = value.toDouble();
    if (!std::isfinite(raw) || std::floor(raw) != raw ||
        raw < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
        raw > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return Result<std::int32_t>::Failure(
            "MCP", "MCP_INTEGER_INVALID", std::string(field) + " 超出 int32 范围。");
    }
    return Result<std::int32_t>::Success(static_cast<std::int32_t>(raw));
}

Result<std::filesystem::path> ParseAbsolutePath(
    const QJsonObject& object, const char* field) {
    const QJsonValue value = object.value(QString::fromLatin1(field));
    if (!value.isString() || value.toString().isEmpty() ||
        value.toString().contains(QChar::Null)) {
        return Result<std::filesystem::path>::Failure(
            "MCP", "MCP_MAP_PATH_INVALID",
            std::string(field) + " 必须是非空且无 NUL 的绝对路径。");
    }
    const std::filesystem::path path(value.toString().toStdWString());
    if (!path.is_absolute()) {
        return Result<std::filesystem::path>::Failure(
            "MCP", "MCP_MAP_PATH_INVALID",
            std::string(field) + " 必须是绝对路径，避免候选保存依赖进程当前目录。");
    }
    return Result<std::filesystem::path>::Success(path);
}

Result<std::filesystem::path> HostConfiguredSaveRoot(const char* name) {
    const QString value = qEnvironmentVariable(name);
    if (value.isEmpty() || value.contains(QChar::Null)) {
        return Result<std::filesystem::path>::Failure(
            "MCP", "MCP_SAVE_HOST_CONFIG_MISSING",
            std::string("可信宿主必须设置绝对目录环境变量 ") + name + "。");
    }
    const std::filesystem::path path(value.toStdWString());
    if (!path.is_absolute()) {
        return Result<std::filesystem::path>::Failure(
            "MCP", "MCP_SAVE_HOST_ROOT_INVALID",
            std::string(name) + " 必须是绝对目录；tools/call 不能覆盖宿主工具链配置。");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(path, error) || error) {
        return Result<std::filesystem::path>::Failure(
            "MCP", "MCP_SAVE_HOST_ROOT_INVALID",
            std::string(name) + " 不是可访问的目录。");
    }
    return Result<std::filesystem::path>::Success(path);
}

Result<Trigger::UiDefinitionKind> ParseRootKind(const QJsonObject& object) {
    const QString kind = object.value("kind").toString().toLower();
    if (kind == "event") {
        return Result<Trigger::UiDefinitionKind>::Success(
            Trigger::UiDefinitionKind::Event);
    }
    if (kind == "condition") {
        return Result<Trigger::UiDefinitionKind>::Success(
            Trigger::UiDefinitionKind::Condition);
    }
    if (kind == "action") {
        return Result<Trigger::UiDefinitionKind>::Success(
            Trigger::UiDefinitionKind::Action);
    }
    return Result<Trigger::UiDefinitionKind>::Failure(
        "MCP", "MCP_ROOT_ECA_KIND_INVALID",
        "根 ECA kind 只能是 event、condition 或 action；call 只能嵌入参数。");
}

QString KindName(Trigger::UiDefinitionKind kind) {
    switch (kind) {
    case Trigger::UiDefinitionKind::Event: return "event";
    case Trigger::UiDefinitionKind::Condition: return "condition";
    case Trigger::UiDefinitionKind::Action: return "action";
    case Trigger::UiDefinitionKind::Call: return "call";
    case Trigger::UiDefinitionKind::Define: return "define";
    default: return "unknown";
    }
}

McpWorkspace* RequireWorkspace(McpServer& server, QJsonObject& error) {
    McpWorkspace* workspace = server.Workspace();
    if (workspace == nullptr) {
        error = DiagnosticsJson(server.InitializationDiagnostics());
    }
    return workspace;
}

Result<std::pair<TransactionId, ProjectRevision>> ParseEnvelope(
    const QJsonObject& parameters) {
    auto transactionId = ParseUnsignedId(parameters, "transaction_id");
    if (!transactionId.Succeeded()) {
        Result<std::pair<TransactionId, ProjectRevision>> result;
        result.diagnostics = transactionId.diagnostics;
        return result;
    }
    auto revision = ParseRevision(parameters);
    if (!revision.Succeeded()) {
        Result<std::pair<TransactionId, ProjectRevision>> result;
        result.diagnostics = revision.diagnostics;
        return result;
    }
    return Result<std::pair<TransactionId, ProjectRevision>>::Success(
        {*transactionId.value, std::move(*revision.value)});
}

QJsonObject ReceiptJson(const Trigger::EcaAppendReceipt& receipt) {
    QJsonArray ecas;
    for (const Trigger::EcaNode& eca : receipt.appendedEcas) {
        ecas.append(QString::number(eca.id));
    }
    QJsonArray arguments;
    for (const Trigger::ArgumentNode& argument : receipt.appendedArguments) {
        arguments.append(QString::number(argument.id));
    }
    return QJsonObject{
        {"ok", true},
        {"root_eca_id", QString::number(receipt.rootEcaId)},
        {"eca_ids", ecas},
        {"argument_ids", arguments},
    };
}

QJsonObject BasicSchema(
    QJsonObject properties,
    QJsonArray required) {
    const QJsonObject revisionSchema{
        {"type", "object"},
        {"properties", QJsonObject{
            {"generation", QJsonObject{
                {"type", "string"},
                {"pattern", "^[1-9][0-9]*$"},
            }},
            {"source_sha256", QJsonObject{
                {"type", "string"},
                {"pattern", "^[0-9a-f]{64}$"},
            }},
            {"ui_catalog_sha256", QJsonObject{
                {"type", "string"},
                {"pattern", "^[0-9a-f]{64}$"},
            }},
        }},
        {"required", QJsonArray{
            "generation", "source_sha256", "ui_catalog_sha256"}},
        {"additionalProperties", false},
    };
    const auto inferredSchema = [&revisionSchema](const QString& key) {
        if (key == "expected_revision") return revisionSchema;
        if (key == "category_id" || key == "value_kind") {
            return QJsonObject{{"type", "integer"}};
        }
        if (key.endsWith("_id")) {
            return QJsonObject{
                {"type", "string"},
                {"pattern", "^[1-9][0-9]*$"},
            };
        }
        return QJsonObject{{"type", "string"}};
    };
    for (const QJsonValue& name : required) {
        const QString key = name.toString();
        if (!key.isEmpty() && !properties.contains(key)) {
            properties[key] = inferredSchema(key);
        }
    }
    QJsonObject schema{{"type", "object"}, {"properties", properties}};
    if (!required.isEmpty()) {
        schema["required"] = required;
    }
    schema["additionalProperties"] = false;
    return schema;
}

} // namespace War3Map::Mcp
