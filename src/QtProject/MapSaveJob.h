#pragma once

#include "EditorProject.h"
#include "War3MapCore/Services/MapSaveService.h"

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace War3Map::MapData { class TerrainSavePlan; }
namespace War3Map::Services { class TerrainMinimapSave; }

// Qt job追加阶段文案、冻结地形计划与本次回读结果。归档、编辑输入、脚本目录及输出
// 均由无Qt的MapSaveTask自持有；执行阶段不得借用EditorProject的可变文档。
namespace War3Map::QtProject {

struct MapSaveRevisionBinding {
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    EditScopeSnapshot scopes;
    std::uint64_t luaTargetGeneration = 0;
};

struct MapSaveJob final : Services::MapSaveTask {
    static_assert(kObjectTypeCount == Services::kMapSaveObjectTypeCount);

    MapSaveJob() = default;
    MapSaveJob(const MapSaveJob&) = delete;
    MapSaveJob& operator=(const MapSaveJob&) = delete;

    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    // 准备阶段冻结的编辑身份；提交必须重新观察并逐项匹配，不能把晚到编辑标为已保存。
    std::optional<MapSaveRevisionBinding> preparedRevision;
    // 与发布前闭包共享同一冻结计划；无状态表示尚未执行本次领域回读。
    std::shared_ptr<const MapData::TerrainSavePlan> terrainSavePlan;
    // 单次请求只冻结枚举和路径；完整捕获/生成及会话复核均在worker执行。
    Services::TerrainMinimapSaveChoice minimapChoice = Services::TerrainMinimapSaveChoice::Preserve;
    std::filesystem::path minimapGameRoot;
    std::shared_ptr<const Services::TerrainMinimapSave> minimapSave;
    std::optional<Status> terrainReadbackStatus;
    std::string terrainReadbackArchiveSha256;
    // 阶段提示可从工作线程调用，由 UI 调用方排队到界面线程。
    std::function<void(const QString&)> progress;

    void Report(const QString& text) const {
        if (progress) progress(text);
    }
};

} // namespace War3Map::QtProject
