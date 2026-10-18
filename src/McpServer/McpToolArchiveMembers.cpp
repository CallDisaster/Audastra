#include "McpToolInternal.h"

#include "War3MapCore/Services/ArchiveExpand.h"
#include "War3MapCore/Services/ArchiveMemberEdit.h"

#include <QJsonArray>

#include <map>
#include <string>
#include <vector>

namespace War3Map::Mcp {
namespace {

QJsonObject SourceSchema() {
    return QJsonObject{
        {"source_map_path", QJsonObject{{"type", "string"}, {"minLength", 1}, {"description", "源地图绝对路径"}}},
        {"expected_source_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
        {"output_path", QJsonObject{{"type", "string"}, {"minLength", 1}, {"description", "新候选的绝对路径，必须尚不存在"}}}};
}

QJsonObject ReceiptJson(const Services::ArchiveMemberEditReceipt& receipt) {
    QJsonArray added, removed;
    for (const auto& member : receipt.added) added.append(QString::fromStdString(member));
    for (const auto& [member, records] : receipt.removed)
        removed.append(QJsonObject{{"archive_path", QString::fromStdString(member)},
            {"import_records_removed", static_cast<qint64>(records)}});
    QJsonObject result{{"ok", true}, {"output_path", QString::fromStdWString(receipt.candidate.wstring())},
        {"candidate_sha256", QString::fromStdString(receipt.candidateSha256)},
        {"source_sha256", QString::fromStdString(receipt.sourceSha256)},
        {"member_count", static_cast<qint64>(receipt.memberCount)}};
    if (!receipt.added.empty()) result.insert("added", added);
    if (!receipt.removed.empty()) {
        result.insert("removed", removed);
        result.insert("references_checked", false);
    }
    return result;
}

bool OnlyFields(const QJsonObject& parameters, const QStringList& allowed, QJsonObject& error) {
    for (auto it = parameters.begin(); it != parameters.end(); ++it)
        if (!allowed.contains(it.key())) {
            error = Failure("MCP_ARCHIVE_MEMBER_FIELD_UNSUPPORTED", "不接受的参数：" + it.key().toStdString());
            return false;
        }
    return true;
}

// 共同的路径与 SHA 解析；成功时 error 为空。
bool ParseSource(const QJsonObject& parameters, std::filesystem::path& source, std::filesystem::path& output,
    std::string& sha, QJsonObject& error) {
    auto sourcePath = ParseAbsolutePath(parameters, "source_map_path");
    if (!sourcePath.Succeeded()) { error = Failure(sourcePath); return false; }
    auto outputPath = ParseAbsolutePath(parameters, "output_path");
    if (!outputPath.Succeeded()) { error = Failure(outputPath); return false; }
    const auto shaValue = parameters.value("expected_source_sha256");
    if (!shaValue.isString() || !IsLowerHexSha256(shaValue.toString().toStdString())) {
        error = Failure("MCP_ARCHIVE_MEMBER_SHA_INVALID", "expected_source_sha256 必须是 64 位小写 SHA-256。");
        return false;
    }
    source = std::move(*sourcePath.value);
    output = std::move(*outputPath.value);
    sha = shaValue.toString().toStdString();
    return true;
}

} // namespace

void RegisterArchiveMemberTools(McpServer& server) {
    {
        ToolDefinition tool;
        tool.name = "archive_members_add";
        tool.description = "把本机文件作为导入成员加入地图，同时登记到 war3map.imp，写出新的候选地图；源图不改。"
            "不覆盖已有成员（含未列入文件清单的）和编辑器管理的地图数据文件，不重建脚本。一次 1..256 个，单个不超过 64 MiB，合计不超过 256 MiB。"
            "与 CLI archive-member-add 共用同一服务。";
        tool.permission = ToolPermission::Save;
        auto properties = SourceSchema();
        properties.insert("files", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 256},
            {"items", BasicSchema({{"disk_path", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024},
                    {"description", "本机文件的绝对路径"}}},
                {"archive_path", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024},
                    {"description", "地图内路径，例如 war3mapImported\\hero.mdx"}}}}, {"disk_path", "archive_path"})}});
        tool.inputSchema = BasicSchema(properties, {"source_map_path", "expected_source_sha256", "output_path", "files"});
        tool.handler = [](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            if (!OnlyFields(parameters, {"source_map_path", "expected_source_sha256", "output_path", "files"}, error)) return error;
            std::filesystem::path source, output;
            std::string sha;
            if (!ParseSource(parameters, source, output, sha, error)) return error;
            std::vector<Services::ArchiveMemberAddition> additions;
            for (const auto& item : parameters.value("files").toArray()) {
                const auto file = item.toObject();
                const QJsonObject diskField{{"disk_path", file.value("disk_path")}};
                auto disk = ParseAbsolutePath(diskField, "disk_path");
                if (!disk.Succeeded()) return Failure(disk);
                if (!file.value("archive_path").isString())
                    return Failure("MCP_ARCHIVE_MEMBER_ARGUMENT_INVALID", "archive_path 必须是字符串。");
                additions.push_back({file.value("archive_path").toString().toStdString(), std::move(*disk.value)});
            }
            auto result = Services::AddArchiveMembers(source, sha, output, additions);
            if (!result.Succeeded() || !result.value) return Failure(result);
            return ReceiptJson(*result.value);
        };
        server.RegisterTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "archive_members_remove";
        tool.description = "删除地图里已有的导入成员及解析到它的全部 war3map.imp 记录，写出新的候选地图；源图不改。"
            "不在导入清单里的成员也可删，地图数据文件不能删，不重建脚本。不检查模型、脚本或物编是否仍引用这些文件，删除前须先确认。"
            "与 CLI archive-member-remove 共用同一服务。";
        tool.permission = ToolPermission::Save;
        auto properties = SourceSchema();
        properties.insert("archive_paths", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 256},
            {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}});
        tool.inputSchema = BasicSchema(properties, {"source_map_path", "expected_source_sha256", "output_path", "archive_paths"});
        tool.handler = [](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            if (!OnlyFields(parameters, {"source_map_path", "expected_source_sha256", "output_path", "archive_paths"}, error)) return error;
            std::filesystem::path source, output;
            std::string sha;
            if (!ParseSource(parameters, source, output, sha, error)) return error;
            std::vector<std::string> members;
            for (const auto& item : parameters.value("archive_paths").toArray()) {
                if (!item.isString()) return Failure("MCP_ARCHIVE_MEMBER_ARGUMENT_INVALID", "archive_paths 每项必须是字符串。");
                members.push_back(item.toString().toStdString());
            }
            auto result = Services::RemoveArchiveMembers(source, sha, output, members);
            if (!result.Succeeded() || !result.value) return Failure(result);
            return ReceiptJson(*result.value);
        };
        server.RegisterTool(std::move(tool));
    }
    {
        ToolDefinition tool;
        tool.name = "archive_expand";
        tool.description = "把 .w3x/.w3m 的具名成员展开到一个新目录，之后用 war3-map-mcp --workspace <该目录> 重新启动服务即可"
            "查询和修改触发器、物编；改完用 save_map_edit_candidate 带 source_map_path、transaction_id 写回新的地图包，"
            "没有展开的成员从原图保留。源图不改，目录必须尚不存在。data_only=true 只写地图数据文件、不写导入资源。"
            "与 CLI archive-expand 共用同一服务。";
        tool.permission = ToolPermission::Save;
        auto properties = SourceSchema();
        properties["output_path"] = QJsonObject{{"type", "string"}, {"minLength", 1},
            {"description", "新展开目录的绝对路径，必须尚不存在，上级目录必须存在"}};
        properties.insert("data_only", QJsonObject{{"type", "boolean"}, {"default", false}});
        tool.inputSchema = BasicSchema(properties, {"source_map_path", "expected_source_sha256", "output_path"});
        tool.handler = [](const QJsonObject& parameters) -> QJsonValue {
            QJsonObject error;
            if (!OnlyFields(parameters, {"source_map_path", "expected_source_sha256", "output_path", "data_only"}, error)) return error;
            std::filesystem::path source, output;
            std::string sha;
            if (!ParseSource(parameters, source, output, sha, error)) return error;
            Services::ArchiveExpandOptions options;
            if (parameters.contains("data_only")) {
                if (!parameters.value("data_only").isBool())
                    return Failure("MCP_ARCHIVE_MEMBER_ARGUMENT_INVALID", "data_only 必须是布尔值。");
                options.dataOnly = parameters.value("data_only").toBool();
            }
            auto result = Services::ExpandArchive(source, sha, output, options);
            if (!result.Succeeded() || !result.value) return Failure(result);
            const auto& receipt = *result.value;
            // 跳过的导入资源只计数；其余跳过项逐条列出，最多 256 条。
            std::map<std::string, qint64> counts;
            QJsonArray skipped;
            for (const auto& item : receipt.skipped) {
                ++counts[item.reason];
                if (item.reason != "import" && skipped.size() < 256)
                    skipped.append(QJsonObject{{"archive_path", QString::fromStdString(item.member)},
                        {"reason", QString::fromStdString(item.reason)}});
            }
            QJsonObject skippedCounts;
            for (const auto& [reason, count] : counts) skippedCounts.insert(QString::fromStdString(reason), count);
            return QJsonObject{{"ok", true}, {"output_path", QString::fromStdWString(receipt.directory.wstring())},
                {"source_sha256", QString::fromStdString(receipt.sourceSha256)},
                {"written_members", static_cast<qint64>(receipt.writtenMembers)},
                {"written_bytes", static_cast<qint64>(receipt.writtenBytes)},
                {"opaque_members", static_cast<qint64>(receipt.opaqueMembers)},
                {"trigger_pair_present", receipt.triggerPairPresent},
                {"skipped_counts", skippedCounts}, {"skipped", skipped}};
        };
        server.RegisterTool(std::move(tool));
    }
}

} // namespace War3Map::Mcp
