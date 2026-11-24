#pragma once
#include "War3MapCore/Render/ModelAnimation.h"

#include <QThreadPool>

#include <memory>

namespace War3Map::Modules {
// 被替换的整帧可达数十 MB；唯一持有时交给后台线程析构，其他持有者仍在时只放手引用。
inline void ReleaseAnimationFrameInBackground(std::shared_ptr<const Render::AnimationFrame> frame) {
    if (!frame || frame.use_count() != 1) return;
    QThreadPool::globalInstance()->start([frame = std::move(frame)]() mutable { frame.reset(); });
}
} // namespace War3Map::Modules
