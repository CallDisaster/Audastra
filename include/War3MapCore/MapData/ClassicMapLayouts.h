#pragma once

#include "War3MapCore/Types.h"

namespace War3Map::MapData {

class UnitPlacements;
class DoodadPlacements;
struct CamerasDocument;
struct SoundsDocument;
struct MapSettings;

// 经典记录的格式资格与是否可读取分开。空指针表示本次不检查该成员，
// 不证明它可以缺失，也不证明记录引用、数值或整个地图可保存。
struct ClassicMapLayoutInputs {
    const UnitPlacements* units = nullptr;
    const DoodadPlacements* doodads = nullptr;
    const CamerasDocument* cameras = nullptr;
    const SoundsDocument* sounds = nullptr;
    const MapSettings* settings = nullptr;
};

// 共用于初始化预检、脚本生成与经典写出，防止只读扩展字段被静默省略。
// 同时检查布局标记和记录内容；仅更改版本/布局标记不能获得经典资格。
[[nodiscard]] Status ValidateClassicMapLayouts(const ClassicMapLayoutInputs& inputs);

} // namespace War3Map::MapData
