#pragma once

#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Services/TerrainSessionResources.h"

// 仅Services实现使用。回调由同一来源工厂封闭构造，不暴露给协议或工程数据。
namespace War3Map::Services::TerrainReplayDetail {

using MemberReader = std::function<Result<std::vector<ArchiveExactMember>>(
    std::span<const std::string>, const ArchiveExactMemberLimits&)>;

struct ResourceSource {
    std::function<Result<MapData::TerrainPathingResources>(const MapData::TerrainGridSnapshot&,
        const MapData::TerrainGridSnapshot&, const MapData::DoodadPlacements&, bool, std::size_t)> water;
    std::function<Result<MapData::TerrainPathingResources>(const MapData::TerrainGridSnapshot&,
        const MapData::TerrainGridSnapshot&, const std::vector<std::byte>*, std::size_t)> native;
    std::function<Status(const MapData::TerrainPathingResources&, std::size_t)> verify;
    std::function<Result<MapData::TerrainResizeResources>(const MapData::MapDataEditSession&,
        const MapData::TerrainResizeLimits&)> resize;
    std::function<Status(const MapData::TerrainResizeResources&, const MapData::TerrainResizeLimits&)> verifyResize;
    std::function<Result<TerrainSessionResources>(const MapData::MapDataEditSession&, std::size_t)> current;

    // 仅本内部结构可把真实工厂绑定的reader与冻结typed会话交给当前结构桥。
    [[nodiscard]] static Result<TerrainSessionResources> CaptureCurrent(const MemberReader& reader,
        const MapData::MapDataEditSession& session, const std::filesystem::path& gameRoot, std::size_t maximumBytes);
};

struct Accounting {
    TerrainReplayLimits limits;
    std::size_t resourceBytes = 0, ownedResources = 0, work = 0, historyBytes = 0;
};

struct DraftPreparation {
    TerrainReplayLimits remaining;
    std::size_t descriptionBytes = 0, descriptionWork = 0;
    std::vector<MapData::TerrainStrokeIntent> intents;
};

[[nodiscard]] Result<DraftPreparation> PrepareDraft(const MapDocument& original,
    const MapData::MapDataEditSession& expected, const MapData::TerrainStrokeJournal& journal,
    const TerrainReplayLimits& limits);
[[nodiscard]] Status FinishDraft(TerrainReplayCandidate& candidate,
    const MapData::MapDataEditSession& expected, const DraftPreparation& preparation);
[[nodiscard]] bool SourceMatches(const MapDocument& original, std::span<const std::byte> terrain,
    std::span<const std::byte> pathing, std::span<const ArchiveExactMember> actual);
[[nodiscard]] Result<std::vector<MapData::TerrainPathingResources>> Replay(const ResourceSource& source,
    MapData::MapDataEditSession& candidate, const MapData::TerrainGridSnapshot& sourceTerrain,
    std::size_t pathingBytes, std::span<const MapData::TerrainStrokeIntent> strokes,
    const std::filesystem::path& gameRoot, Accounting& accounting, const std::string& sourceSha,
    const MapData::TerrainStrokeJournal* journal,
    std::vector<MapData::MapDataEditSession::EditReceipt>* receipts,
    std::vector<MapData::TerrainResizeResources>* resizeOwners = nullptr,
    std::vector<std::shared_ptr<const TerrainSessionResources>>* sessionOwners = nullptr);
[[nodiscard]] Result<TerrainReplayCandidate> CreateReplay(const ResourceSource& source,
    const MapDocument& original, std::span<const std::byte> terrain, std::span<const std::byte> pathing,
    std::span<const MapData::TerrainStrokeIntent> strokes, const std::filesystem::path& gameRoot,
    const TerrainReplayLimits& limits, const std::string& sha, const MemberReader& reader,
    const MapData::TerrainStrokeJournal* journal = nullptr, Accounting* accounting = nullptr);

} // namespace War3Map::Services::TerrainReplayDetail
