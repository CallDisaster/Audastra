#pragma once
#include "War3MapCore/Services/MapSaveService.h"
#include <string>

namespace War3Map::Services::Detail {
// 显式包装的观察者才接收计时边界，普通Progress保留原有阶段序列。
struct SaveTimingObserver {
    MapSaveService::Progress callback;
    void operator()(std::string_view stage) const { if (callback) callback(stage); }
};

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
        const auto* observer = progress_.target<SaveTimingObserver>();
        if (!observer || !observer->callback) return;
        try { observer->callback(std::string(prefix) + std::string(name_)); } catch (...) {}
    }
    const MapSaveService::Progress& progress_;
    std::string_view name_;
    bool finished_ = false;
};
} // namespace War3Map::Services::Detail
