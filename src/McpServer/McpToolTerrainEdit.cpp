#include "McpTerrainEditTools.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {

void RegisterTerrainEditTools(McpServer& server) {
    for (auto definition : TerrainEditToolDefinitions()) {
        const auto name = definition.name;
        definition.handler = [&server, name](const QJsonObject& arguments) -> QJsonValue {
            QJsonObject error;
            auto* workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            TerrainEditToolHost host;
            std::optional<MapData::TerrainPathingResources> waterOwner;
            host.identity = [&server, workspace] {
                QJsonObject identity{{"project_identity", QString::fromStdString(server.DocumentId())},
                    {"revision", QString::fromStdString(workspace->ObjectRevision())},
                    {"transaction_active", workspace->HasActiveTransaction()},
                    {"transaction_revision", RevisionJson(workspace->Revision())}};
                if (workspace->HasActiveTransaction()) identity.insert("transaction_id", QString::number(workspace->ActiveTransactionId()));
                return identity;
            };
            host.context = [&server, workspace]() -> Result<TerrainEditToolContext> {
                const auto current = workspace->TerrainContext(server.DocumentId(), server.DocumentGeneration());
                if (!current.Succeeded()) return {{}, current.diagnostics};
                return Result<TerrainEditToolContext>::Success({current.value->session, current.value->document,
                    current.value->includesActiveTransactionOverlay, false});
            };
            host.apply = [workspace](const auto& plan, const auto& validate) { return workspace->ApplyTerrainPlan(plan, validate); };
            host.prepareBounds = [workspace, &waterOwner](const TerrainEditToolContext& context, const auto& binding,
                const MapData::TerrainBoundsEditIntent& intent) -> Result<Services::PreparedTerrainBoundsEdit> {
                auto captured = workspace->CaptureTerrainBoundsResources();
                if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
                waterOwner = std::move(*captured.value);
                return Services::PrepareTerrainBoundsEdit(*context.session, context.document, binding, *waterOwner, intent);
            };
            host.verifyBounds = [workspace, &waterOwner] {
                return waterOwner ? workspace->VerifyTerrainWaterResources(*waterOwner)
                    : Status::Failure("MCP", "TERRAIN_BOUNDS_SOURCE_REQUIRED", "缺少本笔实际范围规则。");
            };
            host.applyBounds = [](const auto&, const auto&) {
                return Status::Failure("MCP", "TERRAIN_PATCH_BOUNDS_SCRIPT_REQUIRED",
                    "独立地形目录补丁不重建镜头初始化脚本；请用save_map_edit_candidate的playable_bounds命令保存地图。");
            };
            host.prepareWater = [workspace, &waterOwner](const TerrainEditToolContext& context, const auto& binding,
                const auto& options, auto points, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto captured = workspace->CaptureTerrainWaterResources(options.enforceWaterLimits);
                if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
                waterOwner = std::move(*captured.value);
                return Services::PrepareTerrainWaterEdit(*context.session, context.document, binding,
                    *waterOwner, options, points, follow);
            };
            host.verifyWater = [workspace, &waterOwner] {
                return waterOwner ? workspace->VerifyTerrainWaterResources(*waterOwner)
                    : Status::Failure("MCP", "TERRAIN_WATER_SOURCE_REQUIRED", "缺少本笔实际水刷资源。");
            };
            host.prepareCliff = [workspace, &waterOwner](const TerrainEditToolContext& context, const auto& binding,
                const MapData::TerrainBrush& brush, auto points, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto captured = workspace->CaptureTerrainWaterResources(true);
                if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
                waterOwner = std::move(*captured.value);
                return Services::PrepareTerrainCliffEdit(*context.session, context.document, binding,
                    *waterOwner, brush, points, follow);
            };
            host.prepareCornerRamp = [workspace, &waterOwner](const TerrainEditToolContext& context, const auto& binding,
                const MapData::TerrainBrush& brush, auto points, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto captured = workspace->CaptureTerrainWaterResources(true);
                if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
                waterOwner = std::move(*captured.value);
                return Services::PrepareTerrainCornerRampEdit(*context.session, context.document, binding,
                    *waterOwner, brush, points, follow);
            };
            host.prepareStraightRamp = [workspace, &waterOwner](const TerrainEditToolContext& context, const auto& binding,
                const MapData::TerrainBrush& brush, auto points, bool follow,
                MapData::TerrainStraightRampAlgorithm algorithm) -> Result<Services::PreparedTerrainEdit> {
                auto captured = workspace->CaptureTerrainWaterResources(true);
                if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
                waterOwner = std::move(*captured.value);
                return Services::PrepareTerrainStraightRampEdit(*context.session, context.document, binding,
                    *waterOwner, brush, points, follow, {}, algorithm);
            };
            host.prepareBlock = [workspace, &waterOwner](const TerrainEditToolContext& context, const auto& binding,
                const MapData::TerrainBlockPasteIntent& intent, bool follow) -> Result<Services::PreparedTerrainEdit> {
                auto captured = workspace->CaptureTerrainWaterResources(true);
                if (!captured.Succeeded() || !captured.value) return {{}, captured.diagnostics};
                waterOwner = std::move(*captured.value);
                return Services::PrepareTerrainBlockPasteEdit(*context.session, context.document, binding,
                    *waterOwner, intent, follow);
            };
            return InvokeTerrainEditTool(name, arguments, host);
        };
        server.RegisterTool(std::move(definition));
    }
}

} // namespace War3Map::Mcp
