#pragma once

#include "War3MapCore/MapData/TerrainPatchCodec.h"
#include "War3MapCore/MapData/TerrainSavePlan.h"

namespace War3Map::Services {

struct TerrainReplayLimits {
    // 只能收紧。资源owner保留到最终复核；工作与历史按序列累计，候选另核临时峰值。
    std::size_t maximumResourceBytes = 128u * 1024u * 1024u;
    std::size_t maximumWork = 64u * 1024u * 1024u;
    std::size_t maximumCandidateBytes = 512u * 1024u * 1024u;
};

struct TerrainReplayCandidate {
    std::unique_ptr<MapData::MapDataEditSession> session;
    std::vector<MapData::TerrainPathingResources> waterOwners;
    std::vector<MapData::MapDataEditSession::EditReceipt> receipts;
};

// Qt、CLI和MCP共用的薄适配：Core捕获实际来源，ObjectData读取严格typed覆盖，
// 仍由同一个TerrainSavePlan逐格验证旧WPM、安装输出和核对最终回读。
class TerrainPathingSave {
public:
    // journal只持typed意图和身份；实际来源、资源、私有special谱系仍从owned replay重建。
    [[nodiscard]] static Result<MapData::TerrainStrokeJournal> CaptureJournal(const MapArchiveSession& source,
        const MapData::MapDataEditSession& edits);
    [[nodiscard]] static Result<MapData::TerrainStrokeJournal> CaptureJournal(const WorkspaceSnapshot& source,
        const MapData::MapDataEditSession& edits);
    // Prepare冻结前缀和无Undo历史的只读基线；worker实际读取source后补齐来源身份。
    [[nodiscard]] static Result<MapData::TerrainStrokeJournal> CaptureJournal(const MapArchiveSession& source,
        const MapData::MapDataEditSession& frozenBaseline, MapData::TerrainStrokeJournal prefix);
    [[nodiscard]] static Result<MapData::TerrainStrokeJournal> CaptureJournal(const WorkspaceSnapshot& source,
        const MapData::MapDataEditSession& frozenBaseline, MapData::TerrainStrokeJournal prefix);
    [[nodiscard]] static Result<TerrainReplayCandidate> RestoreJournal(const MapArchiveSession& source,
        const MapDocument& original, std::span<const std::byte> terrain, std::span<const std::byte> pathing,
        const MapData::TerrainStrokeJournal& journal, const MapData::MapDataEditSession& expectedDraft,
        const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits = {});
    [[nodiscard]] static Result<TerrainReplayCandidate> RestoreJournal(const WorkspaceSnapshot& source,
        const MapDocument& original, std::span<const std::byte> terrain, std::span<const std::byte> pathing,
        const MapData::TerrainStrokeJournal& journal, const MapData::MapDataEditSession& expectedDraft,
        const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits = {});
    // 从实际来源重新构造独立候选；失败不返回部分会话，不接收活动会话。
    // 1..128笔/累计4096点；每笔水刷均重新捕获实际资源和当前DOO，不接受输出身份。
    [[nodiscard]] static Result<TerrainReplayCandidate> ReplayStrokes(
        const MapArchiveSession& source, const MapDocument& original,
        std::span<const std::byte> terrain, std::span<const std::byte> pathing, std::span<const MapData::TerrainStrokeIntent> strokes,
        const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits = {});
    [[nodiscard]] static Result<TerrainReplayCandidate> ReplayStrokes(
        const WorkspaceSnapshot& source, const MapDocument& original,
        std::span<const std::byte> terrain, std::span<const std::byte> pathing, std::span<const MapData::TerrainStrokeIntent> strokes,
        const std::filesystem::path& gameRoot, const TerrainReplayLimits& limits = {});
    [[nodiscard]] static Result<MapData::TerrainSavePlan> PrepareForSave(const MapArchiveSession& source,
        const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
        const MapData::TerrainSavePlanLimits& limits = {});
    [[nodiscard]] static Result<MapData::TerrainSavePlan> PrepareForSave(const WorkspaceSnapshot& source,
        const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
        const MapData::TerrainSavePlanLimits& limits = {});
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureNative(const MapArchiveSession& source,
        const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads = nullptr);
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureNative(const WorkspaceSnapshot& source,
        const MapData::TerrainGridSnapshot& before, const MapData::TerrainGridSnapshot& after,
        const std::filesystem::path& gameRoot, std::size_t maximumBytes, const std::vector<std::byte>* currentDoodads = nullptr);
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureWaterBrushRules(const MapArchiveSession& source,
        const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
        const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<MapData::TerrainPathingResources> CaptureWaterBrushRules(const WorkspaceSnapshot& source,
        const MapData::TerrainGridSnapshot& sourceTerrain, const MapData::TerrainGridSnapshot& currentTerrain,
        const MapData::DoodadPlacements& currentDoodads, const std::filesystem::path& gameRoot,
        bool enforceWaterLimits, std::size_t maximumBytes);
    [[nodiscard]] static Result<TerrainPatch> Seal(const MapDocument& original,
        std::span<const std::byte> terrain, std::span<const std::byte> pathing,
        const WorkspaceSnapshot& source, const MapData::MapDataEditSession& current,
        std::span<const MapData::TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot = {});
};

} // namespace War3Map::Services
