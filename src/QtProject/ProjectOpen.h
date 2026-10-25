#pragma once

#include "EditorProject.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

class QTemporaryDir;

namespace War3Map::QtProject {

// 预检期间不修改当前工程；取消或失败自动回收仅属于候选的提取目录。
struct PreparedProjectOpen {
    PreparedProjectOpen();
    ~PreparedProjectOpen();
    PreparedProjectOpen(const PreparedProjectOpen&) = delete;
    PreparedProjectOpen& operator=(const PreparedProjectOpen&) = delete;

    std::unique_ptr<QTemporaryDir> extraction;
    std::filesystem::path root;
    std::filesystem::path sourcePath;
    std::string sourceSha256;
    bool map = false;
    bool recovery = false;
    // 清单记录的作者原地图；仅当它仍与基线逐字节相同时 sourceMapMatchesBaseline 为真。
    std::filesystem::path sourceMapPath;
    bool sourceMapMatchesBaseline = false;
    std::filesystem::path uiRoot;
    std::filesystem::path compilerRoot;
    std::string journalJson;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, kObjectTypeCount> objects;
    std::unique_ptr<Trigger::TriggerDocument> triggers;
    Trigger::GuiConversionSnapshots guiConversionSnapshots;
    std::unique_ptr<Trigger::UiCatalog> catalog;
    std::vector<PendingImport> imports;
    std::vector<AssetEdit> assetEdits;
    std::string runtimeApprovedPackageSha256;
    std::unique_ptr<MapData::MapDataEditSession> mapDataEdits;
    std::vector<MapData::MapDataEditSession::EditReceipt> terrainReceipts;
    std::size_t terrainCurrentIndex = 0;
    std::optional<std::size_t> terrainSavedIndex;
    std::string terrainJournalWarning;

    [[nodiscard]] std::filesystem::path BaselinePath() const;
};

[[nodiscard]] Result<std::unique_ptr<PreparedProjectOpen>> PrepareProjectOpen(
    const std::filesystem::path& path, const std::filesystem::path& gameRoot = {});

} // namespace War3Map::QtProject
