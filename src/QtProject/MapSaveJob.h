#pragma once

#include "EditorProject.h"
#include "War3MapCore/Services/MapSaveService.h"

#include <QString>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

// UI 仅持有阶段文案回调和开始时间。归档、编辑输入、脚本目录及输出均由无 Qt 的
// MapSaveTask 自持有；工作线程不得再借用 EditorProject 的可变文档。
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
    // 阶段提示可从工作线程调用，由 UI 调用方排队到界面线程。
    std::function<void(const QString&)> progress;

    void Report(const QString& text) const {
        if (progress) progress(text);
    }
};

} // namespace War3Map::QtProject
