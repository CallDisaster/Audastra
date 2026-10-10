#include "McpProjectSearchTools.h"
#include "McpToolInternal.h"
#include "McpWorkspaceObjectsState.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Text/SourceEncoding.h"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace War3Map::Mcp {
namespace ProjectSearchDetail {

// 内部元数据准备；独立CPU夹具调用同一入口，不提供新的MCP权限或自动来源读取。
Result<std::vector<std::string>> PrepareImportedPaths(std::span<const WorkspaceFile> inventory,
    std::size_t maxExamined) {
    constexpr std::size_t kPathBytes = 2048, kTotalBytes = 1024 * 1024, kRows = 4097;
    const auto count = std::min(inventory.size(), std::min(maxExamined, kRows - 1) + 1);
    auto result = Result<std::vector<std::string>>::Success({});
    auto& paths = *result.value;
    paths.reserve(count);
    std::unordered_set<std::string> identities;
    std::size_t bytes = 0;
    const auto partial = [&](const char* code, const char* message) {
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "MCP.Search", code, message, {}});
    };
    for (std::size_t index = 0; index < count; ++index) {
        const auto& native = inventory[index].relativePath.native();
        // native().size()不复制或转换；巨路径必须在GenericPathUtf8之前拒绝。
        if (native.size() > kPathBytes) {
            partial("PROJECT_SEARCH_IMPORT_NATIVE_PATH_LIMIT", "目录路径超过2048个本机代码单元；未转换该路径或扫描其后缀，输入覆盖为部分。");
            break;
        }
        if (native.empty() || native.find(std::filesystem::path::value_type{}) != native.npos) {
            paths.clear();
            partial("PROJECT_SEARCH_IMPORT_PATH_INVALID", "目录元数据含空路径或NUL，无法证明窗口身份；未选第一条或生成替代路径。");
            break;
        }
        if (native.size() > kTotalBytes - bytes) {
            partial("PROJECT_SEARCH_IMPORT_PATH_TOTAL_LIMIT", "目录路径累计字节预算不足；未转换下一个路径或扫描后缀，输入覆盖为部分。");
            break;
        }
        std::string path;
        try { path = IO::GenericPathUtf8(inventory[index].relativePath); }
        catch (const std::exception&) {
            paths.clear();
            partial("PROJECT_SEARCH_IMPORT_PATH_INVALID", "目录路径不能转换为可信UTF8；未选择第一条身份或读取外盘补齐。");
            break;
        }
        if (path.size() > kPathBytes) {
            partial("PROJECT_SEARCH_IMPORT_PATH_BYTES_LIMIT", "目录路径UTF8编码超过2048字节；未截断路径或扫描后缀，输入覆盖为部分。");
            break;
        }
        if (path.size() > kTotalBytes - bytes) {
            partial("PROJECT_SEARCH_IMPORT_PATH_TOTAL_LIMIT", "目录路径累计UTF8编码超过1MiB；未加入该路径或扫描后缀，输入覆盖为部分。");
            break;
        }
        bool valid = !path.empty();
        for (std::uint32_t offset = 0; valid && offset < path.size();) {
            const auto scalar = Text::ReadSourceScalar(path, offset, Text::SourceEncoding::Utf8);
            if (!scalar.Succeeded() || !scalar.value || !scalar.value->bytes || scalar.value->codepoint == 0) valid = false;
            else offset += scalar.value->bytes;
        }
        if (!valid || !identities.insert(path).second) {
            paths.clear();
            partial("PROJECT_SEARCH_IMPORT_PATH_IDENTITY_INVALID", "目录窗口含坏UTF8或重复路径身份；窗口结果清空，未选第一条身份。");
            break;
        }
        bytes += path.size();
        paths.push_back(std::move(path));
    }
    if (count < inventory.size() && result.diagnostics.empty())
        partial("PROJECT_SEARCH_IMPORT_ROW_LIMIT", "目录路径只组装最多4097行的有界窗口；未扫描剩余路径，输入覆盖为部分。");
    return result;
}

} // namespace ProjectSearchDetail

std::string McpWorkspace::CurrentProjectSearchRevision(std::string_view documentId,
    std::uint64_t documentGeneration) const {
    if (documentId.empty() || documentGeneration == 0) return {};
    // ObjectRevision 已含事务 / overlay generation / toolEpoch 及 session source / catalog SHA。
    // 缓存可用性也属于查询来源；从未准备的域变为已冻结缓存时，旧页必须失效。
    const Services::MapDataQueryBinding binding{std::string(documentId), documentGeneration, toolEpoch_, Revision().sourceSha256};
    const QJsonObject fields{{"schema", "audastra.mcp.search-cache.v1"},
        {"document", QString::fromUtf8(documentId.data(), static_cast<qsizetype>(documentId.size()))},
        {"generation", QString::number(documentGeneration)}, {"revision", QString::fromStdString(ObjectRevision())},
        {"ui", QString::fromStdString(catalog_.Sha256())}, {"object_cache", objectState_ != nullptr},
        {"prepared_types", PreparedProjectSearchTypes() != nullptr},
        {"type_generation", QString::number(projectSearchTypesGeneration_)},
        {"map_cache", committedMapData_ != nullptr}, {"map_inventory", mapDataSource_ != nullptr},
        {"active_map", activeEdit_ && activeEdit_->mapData != nullptr},
        {"prepared_map", PreparedProjectSearchMapCache(binding) != nullptr},
        {"code_binding", "unavailable:standalone-code-index-not-connected"}};
    const auto bytes = QJsonDocument(fields).toJson(QJsonDocument::Compact);
    const auto sha = Sha256(std::as_bytes(std::span(bytes.constData(), static_cast<std::size_t>(bytes.size()))));
    return sha.Succeeded() && sha.value ? "mcp-search-cache-v1:" + *sha.value : std::string{};
}

Result<Services::ProjectSearchPage> McpWorkspace::SearchCachedProject(std::string_view documentId,
    std::uint64_t documentGeneration, std::string_view expectedContextRevision, std::string_view query,
    const Services::ProjectSearchOptions& options, std::size_t pageLimit,
    const Services::ProjectSearchCursor* cursor) const {
    using Output = Result<Services::ProjectSearchPage>;
    using State = Services::ProjectSearchState;
    const auto revision = CurrentProjectSearchRevision(documentId, documentGeneration);
    if (revision.empty() || revision != expectedContextRevision)
        return Output::Failure("MCP.Search", "PROJECT_SEARCH_STALE_CONTEXT", "当前缓存工程身份或复合修订已变化。");
    if (publishing_) return Output::Failure("MCP.Search", "PROJECT_SEARCH_BUSY", "工作区正在发布归档候选。");
    if (cursor && (cursor->contextRevision != revision || cursor->queryUtf8 != query))
        return Output::Failure("MCP.Search", "PROJECT_SEARCH_CURSOR_STALE", "游标的修订或查询已变化；未组装搜索来源。");
    Services::ProjectSearchContext context;
    context.revision = revision; context.triggers = &VisibleDocument();
    context.types = PreparedProjectSearchTypes();
    context.triggerState = context.types ? State::Ready : State::Partial;
    std::array<Services::ProjectSearchObjectSource, 7> objects;
    for (std::size_t index = 0; index < objects.size(); ++index) {
        auto& source = objects[index]; source.type = static_cast<ObjectData::ObjectType>(index);
        if (!objectState_) continue;
        const auto& input = objectState_->mapInputs[index];
        const auto* document = activeEdit_ && activeEdit_->objects[index]
            ? activeEdit_->objects[index].get() : input.committed.get();
        if (!document) continue;
        source.records = document->Objects();
        source.origin = input.nativeSource ? Services::ProjectSearchObjectOrigin::Native : Services::ProjectSearchObjectOrigin::Lni;
        source.state = !document->IsStructurallySafe() ? State::Invalid
            : !input.sourcePresent && source.records.empty() ? State::Missing : State::Ready;
    }
    context.objects = objects;
    if (objectState_) {
        context.standardObjects = &objectState_->standard;
        // 私有缓存未提供“七类完整”证明。非空数量也不证明完整，不能为每页 Count 七次
        // 扫过所有 entries；只查询现有目录，并如实保留 Partial。
        context.standardState = State::Partial;
    }
    const Services::MapDataQueryBinding binding{std::string(documentId), documentGeneration, toolEpoch_, Revision().sourceSha256};
    const auto map = PreparedProjectSearchMapCache(binding);
    if (map) {
        // 只消费先前显式查询交回的强 owner snapshot；每页没有 Capture / typed body 摘要。
        context.mapData = map.get(); context.mapDataBinding = binding;
        const auto document = map->OwnedDocument();
        if (document->Strings()) context.strings = &*document->Strings();
    }
    std::vector<std::string> paths;
    std::vector<Services::ProjectSearchImportedFile> files;
    std::vector<Diagnostic> importInputDiagnostics;
    if (mapDataSource_) {
        // 只能证明打开时的物理目录路径，尚无缓存 IMP 语义和资源写覆盖：始终 Partial。
        // 明确 usesImportedFiles，空目录也不回退不存在的 ImportTable。
        const auto& inventory = mapDataSource_->Snapshot().files;
        auto prepared = ProjectSearchDetail::PrepareImportedPaths(inventory, options.maxExaminedPerCategory);
        paths = std::move(*prepared.value);
        importInputDiagnostics = std::move(prepared.diagnostics);
        files.reserve(paths.size());
        for (const auto& path : paths) files.push_back({path, path});
        context.currentFiles = files; context.usesImportedFiles = true; context.importState = State::Partial;
    }
    context.codeState = State::Unavailable;
    context.codeBinding = "unavailable:standalone-code-index-not-connected";
    auto result = Services::SearchProjectPage(context, revision, query, options, pageLimit, cursor);
    if (CurrentProjectSearchRevision(documentId, documentGeneration) != revision)
        return Output::Failure("MCP.Search", "PROJECT_SEARCH_STALE_CONTEXT", "搜索期间缓存修订变化；请重新查询。");
    if (result.Succeeded()) {
        result.diagnostics.insert(result.diagnostics.end(), importInputDiagnostics.begin(), importInputDiagnostics.end());
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "MCP.Search", "PROJECT_SEARCH_CACHED_SOURCES",
            "只查询当前已冻结缓存与事务覆盖，source_checked=false；缺失缓存域未读取磁盘补齐。", {}});
        if (!context.types) result.diagnostics.push_back({DiagnosticSeverity::Warning, "MCP.Search", "PROJECT_SEARCH_TYPES_UNPREPARED",
            "类型系统缓存尚未准备、预算拒绝或 Catalog owner 已变化；触发器/变量域保留部分覆盖，查询没有遍历函数目录补建。", {}});
        if (!map) result.diagnostics.push_back({DiagnosticSeverity::Warning, "MCP.Search", "PROJECT_SEARCH_MAP_CACHE_UNAVAILABLE",
            "当前地图快照尚未显式准备或 binding 已变化；地图实体域 Unavailable，搜索没有读盘或计算正文摘要补齐。", {}});
        if (mapDataSource_) result.diagnostics.push_back({DiagnosticSeverity::Warning, "MCP.Search", "PROJECT_SEARCH_IMPORTS_PARTIAL",
            "imported_file 只覆盖已冻结目录的有界物理路径，未接入 IMP 清单语义、资源覆盖或重新核验文件；代码声明未接入。", {}});
    }
    return result;
}

void RegisterProjectSearchTools(McpServer& server) {
    for (auto definition : ProjectSearchToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error; auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            // EnvironmentStatus 会重扫 / 哈希整个来源并重载 UI Catalog；只读搜索使用明确的
            // opened-cache 合同，before / after 只比较可信当前身份，不冒充磁盘复验。
            const auto before = workspace->CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration());
            ProjectSearchToolHost host;
            host.identity = [&server, workspace] {
                return QJsonObject{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"context_revision", QString::fromStdString(workspace->CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration()))}};
            };
            host.query = [&server, workspace](std::string_view revision, std::string_view query,
                const Services::ProjectSearchOptions& options, std::size_t pageLimit,
                const Services::ProjectSearchCursor* cursor) {
                return workspace->SearchCachedProject(server.DocumentId(), server.DocumentGeneration(), revision, query, options, pageLimit, cursor);
            };
            auto result = InvokeProjectSearchTool(name, arguments, host);
            if (workspace != server.Workspace() || before != workspace->CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration()))
                return Failure("PROJECT_SEARCH_STALE", "搜索期间独立宿主工作区或缓存修订变化。");
            result.insert("scope", "standalone-opened-cache"); result.insert("source_checked", false);
            return result;
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
