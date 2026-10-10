#pragma once

#include "War3MapCore/Types.h"

#include <QString>

#include <vector>

namespace War3Map::Modules {

// 世界视口的鼠标交互与修订绑定相机命令共用这些边界。
inline constexpr float kMinimumDistance = 16.0f;
inline constexpr float kMaximumDistance = 400000.0f;
inline constexpr float kMaximumCoordinate = 10000000.0f;
inline constexpr float kMinimumPitch = 0.05f;
inline constexpr float kMaximumPitch = 1.5f;

QString PickDiagnostic(const std::vector<Diagnostic>& diagnostics);

} // namespace War3Map::Modules
