#pragma once
#include "War3MapCore/Services/MapSaveService.h"
#include <string>

namespace War3Map::Services::Detail {
// 仅向既有进度观察者报告边界；观察者或通知分配失败不影响保存事务。
class SaveProgressStage {
public:
    SaveProgressStage(const MapSaveService::Progress& progress, std::string_view name) noexcept
        : progress_(progress), name_(name) { Notify("save-timing.begin/"); }
    ~SaveProgressStage() { Finish(); }
    SaveProgressStage(const SaveProgressStage&) = delete;
    SaveProgressStage& operator=(const SaveProgressStage&) = delete;
    void Finish() noexcept {
        if (!finished_) { Notify("save-timing.end/"); finished_ = true; }
    }
private:
    void Notify(std::string_view prefix) noexcept {
        if (!progress_) return;
        try { progress_(std::string(prefix) + std::string(name_)); } catch (...) {}
    }
    const MapSaveService::Progress& progress_;
    std::string_view name_;
    bool finished_ = false;
};
} // namespace War3Map::Services::Detail
