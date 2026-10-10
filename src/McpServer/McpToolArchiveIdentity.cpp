#include "McpToolInternal.h"

#include "War3MapCore/MapArchiveSession.h"

#include <QJsonDocument>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace War3Map::Mcp {
namespace {

constexpr std::size_t kMaxResponseBytes = 1024u * 1024u;

} // namespace

void RegisterArchiveIdentityTools(McpServer& server) {
    ToolDefinition tool;
    tool.name = "query_map_archive_member_identities";
    tool.description = "按整图 SHA 绑定，从独立 MPQ 源图精确查询指定成员的存在状态、字节数与 SHA-256。"
        "不依赖 listfile、不导出或解析成员正文、不读取工作区 overlay；真实空成员与缺失成员分别报告。"
        "最多 256 个成员名，每名最多 1024 个 UTF-8 字节，成员内容累计读取预算 64 MiB。";
    tool.maxResponseBytes = kMaxResponseBytes;
    tool.inputSchema = BasicSchema({
        {"source_path", QJsonObject{{"type", "string"}, {"minLength", 1}}},
        {"expected_source_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
        {"member_names", QJsonObject{{"type", "array"}, {"maxItems", 256},
            {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}}}},
        {"source_path", "expected_source_sha256", "member_names"});
    tool.handler = [](const QJsonObject& parameters) -> QJsonValue {
        for (auto it = parameters.begin(); it != parameters.end(); ++it) {
            if (it.key() != QStringLiteral("source_path") &&
                it.key() != QStringLiteral("expected_source_sha256") &&
                it.key() != QStringLiteral("member_names")) {
                return Failure("MCP_ARCHIVE_IDENTITY_FIELD_UNSUPPORTED", "归档成员身份查询包含未知字段。");
            }
        }
        const auto path = ParseAbsolutePath(parameters, "source_path");
        if (!path.Succeeded()) return Failure(path);
        const auto shaValue = parameters.value("expected_source_sha256");
        const auto expectedSha = shaValue.toString().toStdString();
        if (!shaValue.isString() || !IsLowerHexSha256(expectedSha)) {
            return Failure("MCP_ARCHIVE_IDENTITY_SHA_INVALID", "expected_source_sha256 必须是 64 位小写 SHA-256。");
        }
        const auto namesValue = parameters.value("member_names");
        if (!namesValue.isArray()) {
            return Failure("MCP_ARCHIVE_IDENTITY_NAMES_INVALID", "member_names 必须是成员名字符串数组。");
        }
        const auto nameArray = namesValue.toArray();
        const ArchiveExactMemberLimits limits;
        if (static_cast<std::size_t>(nameArray.size()) > limits.memberCount) {
            return Failure("MCP_ARCHIVE_IDENTITY_NAMES_BUDGET", "member_names 超过 256 项预算。");
        }
        std::vector<std::string> names;
        names.reserve(static_cast<std::size_t>(nameArray.size()));
        for (const auto value : nameArray) {
            if (!value.isString()) {
                return Failure("MCP_ARCHIVE_IDENTITY_NAME_INVALID", "每个成员名必须是字符串。");
            }
            const auto text = value.toString();
            const auto bytes = text.toUtf8();
            if (bytes.isEmpty() || static_cast<std::size_t>(bytes.size()) > limits.pathBytes ||
                bytes.contains('\0') || QString::fromUtf8(bytes) != text) {
                return Failure("MCP_ARCHIVE_IDENTITY_NAME_INVALID", "成员名须为有效且无 NUL 的 UTF-8，长度为 1..1024 字节。");
            }
            names.push_back(bytes.toStdString());
        }
        // 安全成员路径、重复/保留名与完整读取预算只由共用 Core 服务判定。
        const auto opened = MapArchiveSession::Open(*path.value);
        if (!opened.Succeeded() || !opened.value) return Failure(opened);
        const auto queried = opened.value->DescribeExactMemberIdentities(expectedSha, names, limits);
        auto diagnostics = opened.diagnostics;
        diagnostics.insert(diagnostics.end(), queried.diagnostics.begin(), queried.diagnostics.end());
        if (!queried.Succeeded() || !queried.value) return DiagnosticsJson(diagnostics);
        QJsonArray members;
        for (const auto& member : queried.value->members) {
            members.append(QJsonObject{
                {"name", QString::fromUtf8(member.name)},
                {"state", member.present ? "present" : "missing"},
                {"bytes", member.byteSize
                    ? QJsonValue(QString::fromStdString(std::to_string(*member.byteSize)))
                    : QJsonValue(QJsonValue::Null)},
                {"sha256", member.sha256
                    ? QJsonValue(QString::fromUtf8(*member.sha256))
                    : QJsonValue(QJsonValue::Null)},
            });
        }
        QJsonObject response{{"ok", true}, {"schema_version", 1},
            {"source_sha256", QString::fromUtf8(queried.value->sourceSha256)}, {"members", members}};
        if (!diagnostics.empty()) response["diagnostics"] = DiagnosticsJson(diagnostics).value("diagnostics");
        if (static_cast<std::size_t>(QJsonDocument(response).toJson(QJsonDocument::Compact).size()) > kMaxResponseBytes) {
            return Failure("MCP_ARCHIVE_IDENTITY_JSON_BUDGET", "归档成员身份查询回执超过 1 MiB 输出预算。");
        }
        return response;
    };
    server.RegisterTool(std::move(tool));
}

} // namespace War3Map::Mcp
