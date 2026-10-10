#pragma once

#include <QMetaType>
#include <QString>

#include <cstdint>

namespace War3Map::Modules {

// 单次插件调用的有界故障快照；仅返回码/状态，不携带地图、GPU缓冲或截图。
struct RenderPluginFailure {
    QString operation;
    std::int32_t result = 0;
    bool statusAvailable = false;
    std::int32_t statusCallResult = 0;
    std::int32_t statusResult = 0;
    QString message;
};

} // namespace War3Map::Modules

Q_DECLARE_METATYPE(War3Map::Modules::RenderPluginFailure)
