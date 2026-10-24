#include "McpToolInternal.h"
#include "McpTerrain.h"

#include "War3MapCore/Services/TerrainBlightSaveService.h"
#include "War3MapCore/Services/MapEditSaveService.h"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <filesystem>
#include <system_error>
#include <utility>

namespace War3Map::Mcp {
void RegisterTerrainTools(McpServer& server) {
    const QJsonObject shaSchema{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    const QJsonObject unsignedSchema{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}};
    const auto bindingSchema = BasicSchema({
        {"document_id", QJsonObject{{"type", "string"}}},
        {"document_generation", unsignedSchema}, {"revision", unsignedSchema},
        {"source_sha256", shaSchema}, {"terrain_sha256", shaSchema}},
        {"document_id", "document_generation", "revision", "source_sha256", "terrain_sha256"});
    const QJsonObject integerSchema{{"type", "integer"}};
    const auto rectangleSchema = BasicSchema({{"x", integerSchema}, {"y", integerSchema},
        {"width", integerSchema}, {"height", integerSchema}}, {"x", "y", "width", "height"});
    const auto limitsSchema = BasicSchema({{"max_tiles", integerSchema},
        {"max_vertices", integerSchema}, {"max_json_bytes", integerSchema}});

    ToolDefinition snapshot;
    snapshot.name = "get_terrain_snapshot";
    snapshot.description = "只读获取已打开目录 MCP 会话的 W3E 身份和格网范围；无 MPQ/Qt 窗口连接，活动事务拒绝";
    snapshot.inputSchema = BasicSchema({}, {"expected_revision"});
    snapshot.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
        auto expected = ParseRevision(parameters);
        if (!expected.Succeeded()) return Failure(expected);
        if (server.Terrain() == nullptr) return server.InitializationDiagnostics().empty()
            ? Failure("MCP_TERRAIN_UNAVAILABLE", "尚未打开有效的目录 MCP 工作区。")
            : DiagnosticsJson(server.InitializationDiagnostics());
        auto loaded = server.Terrain()->Snapshot(*expected.value);
        if (!loaded.Succeeded()) return Failure(loaded);
        auto result = TerrainSnapshotJson(*loaded.value);
        result["revision"] = RevisionJson(loaded.value->revision);
        if (!loaded.diagnostics.empty()) {
            result["diagnostics"] = DiagnosticsJson(loaded.diagnostics).value("diagnostics");
        }
        if (static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) > TerrainMcpMaxJsonBytes) {
            return Failure("MCP_TERRAIN_JSON_BUDGET", "地形快照元信息超过 JSON 输出预算。");
        }
        return result;
    };
    server.RegisterTool(std::move(snapshot));

    ToolDefinition query;
    query.name = "query_terrain_block";
    query.description = "按目录会话 revision 和完整 terrain_binding 查询原始 W3E 区块（最多 256 格）；不返回截图或原版悬崖/坡道网格";
    query.inputSchema = BasicSchema({{"expected_terrain_binding", bindingSchema},
        {"tiles", rectangleSchema}, {"limits", limitsSchema}},
        {"expected_revision", "expected_terrain_binding", "tiles"});
    query.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
        auto expected = ParseRevision(parameters);
        if (!expected.Succeeded()) return Failure(expected);
        const auto binding = parameters.value("expected_terrain_binding").toObject();
        const auto documentId = binding.value("document_id").toString().toStdString();
        if (documentId.empty() || documentId.size() > 128 || documentId.find('\0') != std::string::npos) {
            return Failure("MCP_TERRAIN_BINDING_INVALID", "document_id 必须为 1..128 字节且不含 NUL。");
        }
        auto generation = ParseUnsignedId(binding, "document_generation");
        auto revision = ParseUnsignedId(binding, "revision", true);
        if (!generation.Succeeded()) return Failure(generation);
        if (!revision.Succeeded()) return Failure(revision);
        const auto tiles = parameters.value("tiles").toObject();
        auto x = ParseInt32(tiles, "x"), y = ParseInt32(tiles, "y");
        auto width = ParseInt32(tiles, "width"), height = ParseInt32(tiles, "height");
        if (!x.Succeeded()) return Failure(x);
        if (!y.Succeeded()) return Failure(y);
        if (!width.Succeeded()) return Failure(width);
        if (!height.Succeeded()) return Failure(height);
        if (*x.value < 0 || *y.value < 0 || *width.value <= 0 || *height.value <= 0) {
            return Failure("MCP_TERRAIN_RECTANGLE_INVALID", "tile 起点必须非负，宽高必须为正整数。");
        }
        std::array<std::size_t, 3> budgets{TerrainMcpMaxTiles, TerrainMcpMaxVertices, TerrainMcpMaxJsonBytes};
        const auto limits = parameters.value("limits").toObject();
        constexpr std::array names{"max_tiles", "max_vertices", "max_json_bytes"};
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (!limits.contains(QLatin1String(names[index]))) continue;
            auto budget = ParseInt32(limits, names[index]);
            if (!budget.Succeeded()) return Failure(budget);
            if (*budget.value <= 0 || static_cast<std::size_t>(*budget.value) > budgets[index]) {
                return Failure("MCP_TERRAIN_LIMITS_INVALID", "预算必须为正且只能收紧工具公布的硬上限。");
            }
            budgets[index] = static_cast<std::size_t>(*budget.value);
        }
        if (server.Terrain() == nullptr) return server.InitializationDiagnostics().empty()
            ? Failure("MCP_TERRAIN_UNAVAILABLE", "尚未打开有效的目录 MCP 工作区。")
            : DiagnosticsJson(server.InitializationDiagnostics());
        Render::TerrainInspectionRequest request{
            {documentId, *generation.value, *revision.value,
                binding.value("source_sha256").toString().toStdString(),
                binding.value("terrain_sha256").toString().toStdString()},
            {static_cast<std::uint32_t>(*x.value), static_cast<std::uint32_t>(*y.value),
                static_cast<std::uint32_t>(*width.value), static_cast<std::uint32_t>(*height.value)}};
        auto block = server.Terrain()->Query(*expected.value, request, budgets[0], budgets[1]);
        if (!block.Succeeded()) return Failure(block);
        auto result = TerrainBlockJson(*block.value);
        result["revision"] = RevisionJson(*expected.value);
        if (static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) > budgets[2]) {
            return Failure("MCP_TERRAIN_JSON_BUDGET", "完整区块超过 JSON 输出预算；请缩小矩形，不返回部分数据。");
        }
        return result;
    };
    server.RegisterTool(std::move(query));

    ToolDefinition saveBlight;
    saveBlight.name = "save_terrain_blight_candidate";
    saveBlight.permission = ToolPermission::Save;
    saveBlight.description =
        "在独立 MPQ 源图上按 W3E 顶点圆形笔刷设置或清除荒芜，并写入新候选地图。"
        "必须绑定源图与 W3E SHA-256；仅修改 W3E/WPM，仍运行完整脚本与物编安全门禁。"
        "工具链与游戏根由可信宿主的 FORGE_MCP_YDWE_ROOT/FORGE_MCP_WAR3_ROOT 固定。"
        "本操作独立于目录工作区和触发器事务，不代表原版 WE/YDWE 或游戏验收。";
    saveBlight.inputSchema = BasicSchema({
        {"source_map_path", QJsonObject{{"type", "string"}}},
        {"output_path", QJsonObject{{"type", "string"}}},
        {"expected_source_sha256", shaSchema},
        {"expected_terrain_sha256", shaSchema},
        {"vertex_x", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
        {"vertex_y", QJsonObject{{"type", "integer"}, {"minimum", 0}}},
        {"radius", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 16}}},
        {"set_blight", QJsonObject{{"type", "boolean"}}},
        {"effective_layers", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 128},
            {"items", QJsonObject{{"type", "string"}}}}},
    }, {"source_map_path", "output_path",
        "expected_source_sha256", "expected_terrain_sha256", "vertex_x", "vertex_y",
        "radius", "set_blight"});
    saveBlight.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
        if (server.Workspace() != nullptr && server.Workspace()->HasActiveTransaction()) {
            return Failure("MCP_TERRAIN_TRANSACTION_ACTIVE",
                "活动触发器事务尚未结束；荒芜候选保存不会包含该 overlay。");
        }
        auto source = ParseAbsolutePath(parameters, "source_map_path");
        auto output = ParseAbsolutePath(parameters, "output_path");
        if (!source.Succeeded()) return Failure(source);
        if (!output.Succeeded()) return Failure(output);
        const auto sourceSha = parameters.value("expected_source_sha256").toString().toStdString();
        const auto terrainSha = parameters.value("expected_terrain_sha256").toString().toStdString();
        if (!IsLowerHexSha256(sourceSha) || !IsLowerHexSha256(terrainSha)) {
            return Failure("MCP_TERRAIN_IDENTITY_INVALID",
                "源图和 W3E 身份都必须是 64 位小写 SHA-256。");
        }
        auto x = ParseInt32(parameters, "vertex_x");
        auto y = ParseInt32(parameters, "vertex_y");
        auto radius = ParseInt32(parameters, "radius");
        if (!x.Succeeded()) return Failure(x);
        if (!y.Succeeded()) return Failure(y);
        if (!radius.Succeeded()) return Failure(radius);
        if (*x.value < 0 || *y.value < 0 || *radius.value < 0 || *radius.value > 16) {
            return Failure("MCP_TERRAIN_BRUSH_BOUNDS",
                "顶点坐标必须非负，半径必须在 0..16 内。");
        }
        std::error_code pathError;
        const auto targetStatus = std::filesystem::symlink_status(*output.value, pathError);
        if (!pathError && targetStatus.type() != std::filesystem::file_type::not_found) {
            return Failure("MCP_OUTPUT_EXISTS", "候选输出路径已存在；必须使用新文件。");
        }
        if (pathError && pathError != std::errc::no_such_file_or_directory) {
            return Failure("MCP_OUTPUT_PATH_INVALID", "无法确认候选输出路径是否已存在。");
        }
        auto toolRoot = HostConfiguredSaveRoot("FORGE_MCP_YDWE_ROOT");
        auto gameRoot = HostConfiguredSaveRoot("FORGE_MCP_WAR3_ROOT");
        if (!toolRoot.Succeeded()) return Failure(toolRoot);
        if (!gameRoot.Succeeded()) return Failure(gameRoot);
        Services::TerrainBlightSaveRequest request;
        request.sourceMap = *source.value;
        request.targetCandidate = *output.value;
        request.toolRoot = *toolRoot.value;
        request.gameRoot = *gameRoot.value;
        request.expectedSourceSha256 = sourceSha;
        request.expectedTerrainSha256 = terrainSha;
        request.vertexX = static_cast<std::uint32_t>(*x.value);
        request.vertexY = static_cast<std::uint32_t>(*y.value);
        request.radius = static_cast<std::uint32_t>(*radius.value);
        request.setBlight = parameters.value("set_blight").toBool();
        if (parameters.contains("effective_layers")) {
            const auto supplied = parameters.value("effective_layers");
            if (!supplied.isArray() || supplied.toArray().isEmpty() || supplied.toArray().size() > 128) {
                return Failure("MCP_UI_LAYERS_INVALID", "有效函数库层序须为 1..128 个名称。");
            }
            request.effectiveLayers.emplace();
            for (const auto& entry : supplied.toArray()) {
                if (!entry.isString() || entry.toString().isEmpty() || entry.toString().size() > 256) {
                    return Failure("MCP_UI_LAYERS_INVALID", "函数库层名称为空、过长或不是字符串。");
                }
                request.effectiveLayers->push_back(entry.toString().toStdString());
            }
        }
        const auto saved = Services::SaveTerrainBlightCandidate(request);
        if (!saved.Succeeded() || !saved.value) return Failure(saved);
        const auto& receipt = *saved.value;
        QJsonObject result{
            {"ok", true}, {"candidate_kind", "mpq-map-archive"},
            {"output_path", QString::fromStdWString(receipt.candidate.wstring())},
            {"source_sha256", QString::fromStdString(receipt.sourceSha256)},
            {"candidate_sha256", QString::fromStdString(receipt.candidateSha256)},
            {"original_terrain_sha256", QString::fromStdString(receipt.originalTerrainSha256)},
            {"saved_terrain_sha256", QString::fromStdString(receipt.savedTerrainSha256)},
            {"saved_pathing_sha256", QString::fromStdString(receipt.savedPathingSha256)},
            {"member_count", static_cast<qint64>(receipt.memberCount)},
            {"script_seconds", receipt.scriptSeconds},
            {"script_rebuilt", true},
            {"native_editor_verified", false},
            {"game_runtime_verified", false},
        };
        QJsonArray effectiveLayers;
        for (const auto& layer : receipt.effectiveLayers) {
            effectiveLayers.push_back(QString::fromStdString(layer));
        }
        result["effective_layers"] = effectiveLayers;
        if (!saved.diagnostics.empty()) {
            result["diagnostics"] = DiagnosticsJson(saved.diagnostics).value("diagnostics");
        }
        return result;
    };
    server.RegisterTool(std::move(saveBlight));

    ToolDefinition shadows;
    shadows.name = "save_terrain_shadow_candidate";
    shadows.permission = ToolPermission::Save;
    shadows.description = "从实际W3E、装饰物与游戏光方向显式计算阴影，经过共用脚本保存门写出新地图副本。"
        "须绑定源图和W3E SHA；工具与游戏目录只读宿主配置。不支持的地形/投影模型明确拒绝，"
        "不影响普通保存保留原SHD。结果不代表原版重开或实际游戏验收。";
    shadows.inputSchema = BasicSchema({
        {"source_map_path", QJsonObject{{"type", "string"}}},
        {"output_path", QJsonObject{{"type", "string"}}},
        {"expected_source_sha256", shaSchema}, {"expected_terrain_sha256", shaSchema}},
        {"source_map_path", "output_path", "expected_source_sha256", "expected_terrain_sha256"});
    shadows.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
        if (server.Workspace() && server.Workspace()->HasActiveTransaction())
            return Failure("MCP_TERRAIN_TRANSACTION_ACTIVE", "活动事务尚未结束，阴影副本保存不会包含该overlay。");
        auto source = ParseAbsolutePath(parameters, "source_map_path");
        auto output = ParseAbsolutePath(parameters, "output_path");
        if (!source.Succeeded()) return Failure(source);
        if (!output.Succeeded()) return Failure(output);
        const auto sourceSha = parameters.value("expected_source_sha256").toString().toStdString();
        const auto terrainSha = parameters.value("expected_terrain_sha256").toString().toStdString();
        if (!IsLowerHexSha256(sourceSha) || !IsLowerHexSha256(terrainSha))
            return Failure("MCP_TERRAIN_IDENTITY_INVALID", "源图和W3E都必须绑定64位小写SHA-256。");
        std::error_code pathError;
        const auto targetStatus = std::filesystem::symlink_status(*output.value, pathError);
        if (!pathError && targetStatus.type() != std::filesystem::file_type::not_found)
            return Failure("MCP_OUTPUT_EXISTS", "候选路径已存在，请使用新文件。");
        if (pathError && pathError != std::errc::no_such_file_or_directory)
            return Failure("MCP_OUTPUT_PATH_INVALID", "无法确认候选路径状态。");
        auto toolRoot = HostConfiguredSaveRoot("FORGE_MCP_YDWE_ROOT");
        auto gameRoot = HostConfiguredSaveRoot("FORGE_MCP_WAR3_ROOT");
        if (!toolRoot.Succeeded()) return Failure(toolRoot);
        if (!gameRoot.Succeeded()) return Failure(gameRoot);
        Services::MapEditSaveRequest request;
        request.sourceMap = *source.value; request.targetCandidate = *output.value;
        request.toolRoot = *toolRoot.value; request.gameRoot = *gameRoot.value;
        request.expectedSourceSha256 = sourceSha; request.expectedTerrainSha256 = terrainSha;
        request.rebuildTerrainShadow = true;
        const auto saved = Services::SaveMapEditCandidate(request);
        if (!saved.Succeeded() || !saved.value) return Failure(saved);
        QJsonObject result{{"ok", true}, {"candidate_kind", "mpq-map-archive"},
            {"output_path", QString::fromStdWString(saved.value->candidate.wstring())},
            {"source_sha256", QString::fromStdString(saved.value->sourceSha256)},
            {"candidate_sha256", QString::fromStdString(saved.value->candidateSha256)},
            {"shadow_rebuilt", true}, {"native_editor_verified", false}, {"game_runtime_verified", false}};
        if (!saved.diagnostics.empty()) result["diagnostics"] = DiagnosticsJson(saved.diagnostics).value("diagnostics");
        return result;
    };
    server.RegisterTool(std::move(shadows));
}

} // namespace War3Map::Mcp
