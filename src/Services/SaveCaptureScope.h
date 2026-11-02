#pragma once
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/MapArchiveSession.h"

namespace War3Map::Services::Detail {
// 工作阶段拥有的来源固定租约不进入交互基线；显式调用方已有的租约保持其原生命周期。
class SaveCaptureScope {
public:
    enum class Target { Source, Saved };
    SaveCaptureScope(MapSaveTask& task, Target target) : task_(task), target_(target) {}
    SaveCaptureScope(const SaveCaptureScope&) = delete;
    SaveCaptureScope& operator=(const SaveCaptureScope&) = delete;
    ~SaveCaptureScope() {
        if (owned_ && Current() == identity_) identity_->ReleaseSourceCaptureLease();
    }
    Status Acquire() {
        identity_ = Current();
        if (!identity_) return Status::Failure("MapSaveService", "SAVE_CAPTURE_SOURCE_MISSING", "保存核对缺少实际归档来源。");
        if (identity_->HasSourceCaptureLease()) return Status::Ok();
        const auto acquired = identity_->PinSourceForExactCaptures();
        owned_ = acquired.Succeeded();
        return acquired;
    }
    void AdoptCreatedLease() noexcept {
        identity_ = Current();
        owned_ = identity_ && identity_->HasSourceCaptureLease();
    }
    bool OwnsLease() const noexcept { return owned_; }
private:
    MapArchiveSession* Current() const noexcept {
        return target_ == Target::Source ? task_.archive.get() : task_.nextSession.get();
    }
    MapSaveTask& task_;
    Target target_;
    MapArchiveSession* identity_ = nullptr;
    bool owned_ = false;
};
} // namespace War3Map::Services::Detail
