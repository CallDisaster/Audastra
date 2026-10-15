#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace War3Map::MapData {
namespace {

constexpr std::size_t kMaxBytesField = 4096;
constexpr float kMaxCoordinate = 1e6f;

bool IsUnitPositionField(const MapFieldDescriptor& field) {
    return field.kind == MapDataKind::Units && (field.id == "x" || field.id == "y" || field.id == "z");
}

template<class T>
Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure("MapData.Fields", std::move(code), std::move(message));
}

template<class T>
Result<T> Forward(std::vector<Diagnostic> diagnostics) {
    Result<T> result;
    result.diagnostics = std::move(diagnostics);
    if (result.diagnostics.empty()) {
        result.diagnostics.push_back({DiagnosticSeverity::Error, "MapData.Fields", "MAP_FIELD_UPSTREAM_FAILED",
            "依赖的解析或序列化步骤未返回完整结果。"});
    }
    return result;
}

MapFieldValue Int(std::int64_t value, MapFieldType type = MapFieldType::Int32) {
    MapFieldValue result;
    result.type = type;
    result.integer = value;
    return result;
}

MapFieldValue U32(std::uint32_t value) { return Int(value, MapFieldType::UInt32); }

MapFieldValue F32(float value, MapFieldType type = MapFieldType::Float32) {
    MapFieldValue result;
    result.type = type;
    result.floatBits = std::bit_cast<std::uint32_t>(value);
    return result;
}

MapFieldValue Angle(float value) { return F32(value, MapFieldType::AngleRadians); }

MapFieldValue Text(const std::string& value) {
    MapFieldValue result;
    result.type = MapFieldType::Bytes;
    result.bytes = value;
    return result;
}

float Float(const MapFieldValue& value) { return std::bit_cast<float>(value.floatBits); }
std::int32_t I32(const MapFieldValue& value) { return static_cast<std::int32_t>(value.integer); }

// 只保留该类型有意义的分量，避免调用方残留的无关分量影响逐位比较。
MapFieldValue Normalized(const MapFieldValue& value) {
    switch (value.type) {
    case MapFieldType::Int32:
    case MapFieldType::UInt32: return Int(value.integer, value.type);
    case MapFieldType::Float32:
    case MapFieldType::AngleRadians: {
        MapFieldValue result;
        result.type = value.type;
        result.floatBits = value.floatBits;
        return result;
    }
    case MapFieldType::Bytes: return Text(value.bytes);
    }
    return value;
}

bool Coordinate(const MapFieldValue& value) {
    const float number = Float(value);
    return std::isfinite(number) && std::abs(number) <= kMaxCoordinate;
}

bool SoundVolume(const MapFieldValue& value) {
    return value.integer == -1 || (value.integer >= 0 && value.integer <= 127);
}

bool DefaultOrNonNegative(const MapFieldValue& value) {
    return value.integer >= -1 && value.integer <= std::numeric_limits<std::int32_t>::max();
}

bool CameraDistance(const MapFieldValue& value) { return Coordinate(value) && Float(value) >= 0; }
bool CameraFar(const MapFieldValue& value) { return Coordinate(value) && Float(value) > 0; }
bool CameraFov(const MapFieldValue& value) { return Coordinate(value) && Float(value) > 0 && Float(value) < 180; }
bool ColorBits(const MapFieldValue& value) { return value.integer >= 0 && value.integer <= std::numeric_limits<std::uint32_t>::max(); }
bool Weather(const MapFieldValue& value) {
    return IsEditableRegionWeatherId(value.bytes);
}

bool GameDataSet(const MapFieldValue& value) { return value.integer >= -1 && value.integer <= 2; }
bool UnitOwner(const MapFieldValue& value) { return value.integer >= 0 && value.integer <= 15; }
bool UnitLife(const MapFieldValue& value) { return value.integer >= -1 && value.integer <= 100; }
bool HeroLevel(const MapFieldValue& value) {
    return value.integer >= 1 && value.integer <= std::numeric_limits<std::int32_t>::max();
}

bool Facing(const MapFieldValue& value) {
    const float radians = Float(value);
    return std::isfinite(radians) && radians >= 0 && radians < 2 * std::numbers::pi_v<float>;
}

// 新文本须是明确的 UTF-8 字面量；既有设置引用的写入目标由共用字段事务决定。
bool Utf8Text(const MapFieldValue& value) {
    const std::string_view text = value.bytes;
    if (text.size() > kMaxBytesField) return false;
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        const std::size_t trailing = first >= 0xc2 && first <= 0xdf ? 1
            : first >= 0xe0 && first <= 0xef ? 2
            : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (trailing == 0 || trailing > text.size() - index) return false;
        const auto second = static_cast<unsigned char>(text[index]);
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0)
            || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return false;
        for (std::size_t part = 0; part < trailing; ++part) {
            const auto next = static_cast<unsigned char>(text[index++]);
            if (next < 0x80 || next > 0xbf) return false;
        }
    }
    return true;
}

bool Utf8Literal(const MapFieldValue& value) {
    return !value.bytes.empty() && !value.bytes.starts_with("TRIGSTR_") && Utf8Text(value);
}

bool OptionalUtf8Literal(const MapFieldValue& value) {
    return !value.bytes.starts_with("TRIGSTR_") && Utf8Text(value);
}

template<class Record>
struct FieldImpl {
    MapFieldDescriptor descriptor;
    MapFieldValue (*read)(const Record&) = nullptr;
    void (*write)(Record&, const MapFieldValue&) = nullptr;
    bool (*valid)(const MapFieldValue&) = nullptr;
    bool (*present)(const Record&) = nullptr;
};

using K = MapDataKind;
using T = MapFieldType;
using V = MapFieldValue;

template<bool MapSettingsLayout::*Member>
bool HasSettingsField(const MapSettings& settings) {
    const auto* layout = settings.Layout();
    return layout && layout->*Member;
}

// 目录统一声明产品的写入能力；作者授权开放的实例字段仍须另行完成原版/游戏验收。
const FieldImpl<MapSettings> kSettingsFields[] = {
    {{K::Settings, "format_version", "成员格式版本", T::Int32}, [](const MapSettings& r) { return Int(r.formatVersion); }},
    {{K::Settings, "map_name", "地图名", T::Bytes, true, "非空 UTF-8 字面量，不接受 TRIGSTR 引用"},
        [](const MapSettings& r) { return Text(r.mapName); },
        [](MapSettings& r, const V& v) { r.mapName = v.bytes; }, Utf8Literal},
    {{K::Settings, "author", "作者", T::Bytes, true, "UTF-8 字面量，最多4096字节；原值为 TRIGSTR 时保留引用并修改唯一 WTS 正文，同编号的其它引用一并受影响"},
        [](const MapSettings& r) { return Text(r.author); },
        [](MapSettings& r, const V& v) { r.author = v.bytes; }, OptionalUtf8Literal},
    {{K::Settings, "description", "描述", T::Bytes, true, "UTF-8 字面量，最多4096字节；原值为 TRIGSTR 时保留引用并修改唯一 WTS 正文，同编号的其它引用一并受影响"},
        [](const MapSettings& r) { return Text(r.description); },
        [](MapSettings& r, const V& v) { r.description = v.bytes; }, OptionalUtf8Literal},
    {{K::Settings, "recommended_players", "建议玩家", T::Bytes, true, "UTF-8 字面量，最多4096字节；原值为 TRIGSTR 时保留引用并修改唯一 WTS 正文，同编号的其它引用一并受影响"},
        [](const MapSettings& r) { return Text(r.recommendedPlayers); },
        [](MapSettings& r, const V& v) { r.recommendedPlayers = v.bytes; }, OptionalUtf8Literal},
    {{K::Settings, "playable_width", "可用宽度", T::Int32}, [](const MapSettings& r) { return Int(r.playableWidth); }},
    {{K::Settings, "playable_height", "可用高度", T::Int32}, [](const MapSettings& r) { return Int(r.playableHeight); }},
    {{K::Settings, "flags", "地图选项位", T::UInt32}, [](const MapSettings& r) { return U32(r.flags); }},
    {{K::Settings, "tileset", "地形主题", T::UInt32}, [](const MapSettings& r) { return U32(r.tileset); }},
    {{K::Settings, "game_data_set", "游戏数据集", T::Int32, true, "-1、0、1、2"},
        [](const MapSettings& r) { return Int(r.gameDataSet); },
        [](MapSettings& r, const V& v) { r.gameDataSet = I32(v); }, GameDataSet,
        [](const MapSettings& r) { return r.Layout() && !r.Layout()->roc; }},
    {{K::Settings, "roc_campaign_background", "ROC 战役背景编号", T::Int32},
        [](const MapSettings& r) { return Int(*r.rocCampaignBackground); }, nullptr, nullptr,
        [](const MapSettings& r) { return HasSettingsField<&MapSettingsLayout::roc>(r) && r.rocCampaignBackground.has_value(); }},
    {{K::Settings, "roc_loading_screen_number", "ROC 载入画面编号", T::Int32},
        [](const MapSettings& r) { return Int(*r.rocLoadingScreenNumber); }, nullptr, nullptr,
        [](const MapSettings& r) { return HasSettingsField<&MapSettingsLayout::roc>(r) && r.rocLoadingScreenNumber.has_value(); }},
    {{K::Settings, "game_version_major", "游戏主版本", T::Int32},
        [](const MapSettings& r) { return Int(r.gameVersion[0]); }, nullptr, nullptr,
        HasSettingsField<&MapSettingsLayout::gameVersion>},
    {{K::Settings, "game_version_minor", "游戏次版本", T::Int32},
        [](const MapSettings& r) { return Int(r.gameVersion[1]); }, nullptr, nullptr,
        HasSettingsField<&MapSettingsLayout::gameVersion>},
    {{K::Settings, "game_version_build", "游戏构建号", T::Int32},
        [](const MapSettings& r) { return Int(r.gameVersion[2]); }, nullptr, nullptr,
        HasSettingsField<&MapSettingsLayout::gameVersion>},
    {{K::Settings, "game_version_revision", "游戏修订号", T::Int32},
        [](const MapSettings& r) { return Int(r.gameVersion[3]); }, nullptr, nullptr,
        HasSettingsField<&MapSettingsLayout::gameVersion>},
    {{K::Settings, "script_language", "脚本语言原值", T::Int32},
        [](const MapSettings& r) { return Int(r.scriptLanguage); }, nullptr, nullptr,
        HasSettingsField<&MapSettingsLayout::scriptLanguage>},
    {{K::Settings, "graphics_modes", "图形模式位", T::UInt32},
        [](const MapSettings& r) { return U32(r.graphicsModes); }, nullptr, nullptr,
        HasSettingsField<&MapSettingsLayout::graphicsModes>},
    {{K::Settings, "game_data_version", "游戏数据版本原值", T::Int32},
        [](const MapSettings& r) { return Int(r.gameDataVersion); }, nullptr, nullptr,
        HasSettingsField<&MapSettingsLayout::graphicsModes>},
    {{K::Settings, "force_default_camera_zoom", "强制默认镜头缩放原值", T::Int32},
        [](const MapSettings& r) { return Int(*r.forceDefaultCameraZoom); }, nullptr, nullptr,
        [](const MapSettings& r) { return HasSettingsField<&MapSettingsLayout::cameraZoom>(r) && r.forceDefaultCameraZoom.has_value(); }},
    {{K::Settings, "force_max_camera_zoom", "强制最大镜头缩放原值", T::Int32},
        [](const MapSettings& r) { return Int(*r.forceMaxCameraZoom); }, nullptr, nullptr,
        [](const MapSettings& r) { return HasSettingsField<&MapSettingsLayout::cameraZoom>(r) && r.forceMaxCameraZoom.has_value(); }},
    {{K::Settings, "force_min_camera_zoom", "强制最小镜头缩放原值", T::Int32},
        [](const MapSettings& r) { return Int(*r.forceMinCameraZoom); }, nullptr, nullptr,
        [](const MapSettings& r) { return HasSettingsField<&MapSettingsLayout::minimumCameraZoom>(r) && r.forceMinCameraZoom.has_value(); }},
};

const FieldImpl<UnitPlacement> kUnitFields[] = {
    {{K::Units, "rawcode", "类型", T::Bytes}, [](const UnitPlacement& r) { return Text(r.rawcode); }},
    {{K::Units, "owner", "所属玩家", T::Int32, true, "玩家索引 0..15"},
        [](const UnitPlacement& r) { return Int(r.owner); },
        [](UnitPlacement& r, const V& v) { r.owner = I32(v); }, UnitOwner},
    {{K::Units, "x", "X", T::Float32, true, "有限世界坐标 -1000000..1000000；经典普通单位，sloc 只读"},
        [](const UnitPlacement& r) { return F32(r.position[0]); },
        [](UnitPlacement& r, const V& v) { r.position[0] = Float(v); }, Coordinate},
    {{K::Units, "y", "Y", T::Float32, true, "有限世界坐标 -1000000..1000000；经典普通单位，sloc 只读"},
        [](const UnitPlacement& r) { return F32(r.position[1]); },
        [](UnitPlacement& r, const V& v) { r.position[1] = Float(v); }, Coordinate},
    {{K::Units, "z", "Z", T::Float32, true, "有限世界坐标 -1000000..1000000；经典普通单位，sloc 只读"},
        [](const UnitPlacement& r) { return F32(r.position[2]); },
        [](UnitPlacement& r, const V& v) { r.position[2] = Float(v); }, Coordinate},
    {{K::Units, "facing", "朝向", T::AngleRadians, true, "0 ≤ 角度 < 360"},
        [](const UnitPlacement& r) { return Angle(r.rotation); },
        [](UnitPlacement& r, const V& v) { r.rotation = Float(v); }, Facing},
    {{K::Units, "variation", "变体", T::Int32}, [](const UnitPlacement& r) { return Int(r.variation); }},
    {{K::Units, "hit_points", "生命值", T::Int32, true, "原值 -1 继承默认，否则为 0..100 整数百分比", MapFieldQuantity::PercentOfMaximum},
        [](const UnitPlacement& r) { return Int(r.hitPoints); },
        [](UnitPlacement& r, const V& v) { r.hitPoints = I32(v); }, UnitLife},
    {{K::Units, "mana", "魔法值", T::Int32, true, "原值 -1 继承默认，否则为非负整数魔法值", MapFieldQuantity::Absolute},
        [](const UnitPlacement& r) { return Int(r.mana); },
        [](UnitPlacement& r, const V& v) { r.mana = I32(v); }, DefaultOrNonNegative},
    {{K::Units, "gold", "金矿储量", T::Int32}, [](const UnitPlacement& r) { return Int(r.gold); }},
    {{K::Units, "hero_level", "英雄等级", T::Int32, true, "不小于 1 的整数；只对英雄生效（初始化时 SetHeroLevel）"},
        [](const UnitPlacement& r) { return Int(r.heroLevel); },
        [](UnitPlacement& r, const V& v) { r.heroLevel = I32(v); }, HeroLevel},
    {{K::Units, "creation_id", "创建编号", T::Int32}, [](const UnitPlacement& r) { return Int(r.creationId); }},
};

// 普通装饰物使用世界坐标和变换；特殊装饰物只有网格坐标。两种形式共用类别，
// 但查询时只返回当前形式确实拥有的字段。
const FieldImpl<DoodadPlacement> kDoodadFields[] = {
    {{K::Doodads, "rawcode", "类型", T::Bytes}, [](const DoodadPlacement& r) { return Text(r.rawcode); }},
    {{K::Doodads, "variation", "变体", T::Int32}, [](const DoodadPlacement& r) { return Int(r.variation); }},
    {{K::Doodads, "x", "X", T::Float32}, [](const DoodadPlacement& r) { return F32(r.position[0]); }},
    {{K::Doodads, "y", "Y", T::Float32}, [](const DoodadPlacement& r) { return F32(r.position[1]); }},
    {{K::Doodads, "z", "Z", T::Float32}, [](const DoodadPlacement& r) { return F32(r.position[2]); }},
    {{K::Doodads, "facing", "朝向", T::AngleRadians}, [](const DoodadPlacement& r) { return Angle(r.rotation); }},
    {{K::Doodads, "scale_x", "缩放 X", T::Float32}, [](const DoodadPlacement& r) { return F32(r.scale[0]); }},
    {{K::Doodads, "scale_y", "缩放 Y", T::Float32}, [](const DoodadPlacement& r) { return F32(r.scale[1]); }},
    {{K::Doodads, "scale_z", "缩放 Z", T::Float32}, [](const DoodadPlacement& r) { return F32(r.scale[2]); }},
    {{K::Doodads, "flags", "标志位", T::UInt32}, [](const DoodadPlacement& r) { return U32(r.flags); }},
    {{K::Doodads, "life_percent", "生命百分比", T::UInt32},
        [](const DoodadPlacement& r) { return U32(r.lifePercent); }},
    {{K::Doodads, "dropped_item_table", "掉落物品表", T::Int32},
        [](const DoodadPlacement& r) { return Int(r.droppedItemTable); }},
    {{K::Doodads, "item_set_count", "物品组数量", T::UInt32},
        [](const DoodadPlacement& r) { return U32(static_cast<std::uint32_t>(r.itemSets.size())); }},
    {{K::Doodads, "creation_id", "创建编号", T::Int32},
        [](const DoodadPlacement& r) { return Int(r.creationId); }},
};

const FieldImpl<SpecialDoodadPlacement> kSpecialDoodadFields[] = {
    {{K::Doodads, "rawcode", "类型", T::Bytes}, [](const SpecialDoodadPlacement& r) { return Text(r.rawcode); }},
    {{K::Doodads, "variation", "变体", T::Int32},
        [](const SpecialDoodadPlacement& r) { return Int(r.variation); }},
    {{K::Doodads, "grid_x", "网格 X", T::Int32},
        [](const SpecialDoodadPlacement& r) { return Int(r.position[0]); }},
    {{K::Doodads, "grid_y", "网格 Y", T::Int32},
        [](const SpecialDoodadPlacement& r) { return Int(r.position[1]); }},
};

const FieldImpl<RegionRecord> kRegionFields[] = {
    {{K::Regions, "name", "名称", T::Bytes}, [](const RegionRecord& r) { return Text(r.name); }},
    {{K::Regions, "creation_number", "创建编号", T::Int32},
        [](const RegionRecord& r) { return Int(r.creationNumber); }},
    {{K::Regions, "left", "左", T::Float32, true, "±1000000 内，且 left < right"},
        [](const RegionRecord& r) { return F32(r.left); },
        [](RegionRecord& r, const V& v) { r.left = Float(v); }, Coordinate},
    {{K::Regions, "bottom", "下", T::Float32, true, "±1000000 内，且 bottom < top"},
        [](const RegionRecord& r) { return F32(r.bottom); },
        [](RegionRecord& r, const V& v) { r.bottom = Float(v); }, Coordinate},
    {{K::Regions, "right", "右", T::Float32, true, "±1000000 内，且 left < right"},
        [](const RegionRecord& r) { return F32(r.right); },
        [](RegionRecord& r, const V& v) { r.right = Float(v); }, Coordinate},
    {{K::Regions, "top", "上", T::Float32, true, "±1000000 内，且 bottom < top"},
        [](const RegionRecord& r) { return F32(r.top); },
        [](RegionRecord& r, const V& v) { r.top = Float(v); }, Coordinate},
    {{K::Regions, "ambient_sound", "环境音效", T::Bytes}, [](const RegionRecord& r) { return Text(r.ambientSound); }},
    {{K::Regions, "weather_id", "天气 ID", T::Bytes, true, "四字节 rawcode，空字符串表示无天气"},
        [](const RegionRecord& r) { return Text(std::all_of(r.weatherId.begin(), r.weatherId.end(), [](auto b) { return b == std::byte{}; })
            ? std::string{} : std::string(reinterpret_cast<const char*>(r.weatherId.data()), 4)); },
        [](RegionRecord& r, const V& v) { r.weatherId.fill(std::byte{}); for (std::size_t i = 0; i < v.bytes.size(); ++i) r.weatherId[i] = static_cast<std::byte>(v.bytes[i]); }, Weather},
    {{K::Regions, "color_bgra", "颜色（BGRA）", T::UInt32, true, "B、G、R 位于低三字节，高位保留字节必须与原值一致"}, [](const RegionRecord& r) {
        return U32(static_cast<std::uint32_t>(r.colorBgra[0]) | static_cast<std::uint32_t>(r.colorBgra[1]) << 8
            | static_cast<std::uint32_t>(r.colorBgra[2]) << 16 | static_cast<std::uint32_t>(r.colorBgra[3]) << 24);
    }, [](RegionRecord& r, const V& v) {
        for (unsigned i = 0; i < 4; ++i) r.colorBgra[i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v.integer) >> (8 * i));
    }, ColorBits},
};

const FieldImpl<CameraRecord> kCameraFields[] = {
    {{K::Cameras, "name", "名称", T::Bytes}, [](const CameraRecord& r) { return Text(r.name); }},
    {{K::Cameras, "target_x", "目标 X", T::Float32, true, "±1000000 内"},
        [](const CameraRecord& r) { return F32(r.targetX); },
        [](CameraRecord& r, const V& v) { r.targetX = Float(v); }, Coordinate},
    {{K::Cameras, "target_y", "目标 Y", T::Float32, true, "±1000000 内"},
        [](const CameraRecord& r) { return F32(r.targetY); },
        [](CameraRecord& r, const V& v) { r.targetY = Float(v); }, Coordinate},
    // 09-30 HumanX06 原版真保存：W3C 全表字段不变；证据见 four-field-edits-20260930。
    {{K::Cameras, "z_offset", "高度偏移", T::Float32, true, "±1000000 内"},
        [](const CameraRecord& r) { return F32(r.zOffset); },
        [](CameraRecord& r, const V& v) { r.zOffset = Float(v); }, Coordinate},
    {{K::Cameras, "rotation", "旋转", T::Float32, true, "有限角度（度）"}, [](const CameraRecord& r) { return F32(r.rotation); },
        [](CameraRecord& r, const V& v) { r.rotation = Float(v); }, Coordinate},
    {{K::Cameras, "angle_of_attack", "仰角", T::Float32, true, "有限角度（度）"}, [](const CameraRecord& r) { return F32(r.angleOfAttack); },
        [](CameraRecord& r, const V& v) { r.angleOfAttack = Float(v); }, Coordinate},
    {{K::Cameras, "distance", "镜头距离", T::Float32, true, "0..1000000 内"},
        [](const CameraRecord& r) { return F32(r.targetDistance); },
        [](CameraRecord& r, const V& v) { r.targetDistance = Float(v); }, CameraDistance},
    {{K::Cameras, "roll", "横滚", T::Float32, true, "有限角度（度）"}, [](const CameraRecord& r) { return F32(r.roll); },
        [](CameraRecord& r, const V& v) { r.roll = Float(v); }, Coordinate},
    {{K::Cameras, "field_of_view", "视野", T::Float32, true, "0 < 视野 < 180 度"}, [](const CameraRecord& r) { return F32(r.fieldOfView); },
        [](CameraRecord& r, const V& v) { r.fieldOfView = Float(v); }, CameraFov},
    {{K::Cameras, "far_z", "远裁剪", T::Float32, true, "0 < 远裁剪 ≤ 1000000"}, [](const CameraRecord& r) { return F32(r.farClippingPlane); },
        [](CameraRecord& r, const V& v) { r.farClippingPlane = Float(v); }, CameraFar},
    {{K::Cameras, "near_z", "经典第十槽（只读）", T::Float32}, [](const CameraRecord& r) { return F32(r.nearClippingPlane); }},
    {{K::Cameras, "extended_local_pitch", "局部俯仰（扩展）", T::Float32},
        [](const CameraRecord& r) { return F32((*r.localRotation)[0]); }},
    {{K::Cameras, "extended_local_yaw", "局部偏航（扩展）", T::Float32},
        [](const CameraRecord& r) { return F32((*r.localRotation)[1]); }},
    {{K::Cameras, "extended_local_roll", "局部横滚（扩展）", T::Float32},
        [](const CameraRecord& r) { return F32((*r.localRotation)[2]); }},
};

const FieldImpl<SoundRecord> kSoundFields[] = {
    {{K::Sounds, "name", "变量名", T::Bytes}, [](const SoundRecord& r) { return Text(r.name); }},
    {{K::Sounds, "file_path", "文件路径", T::Bytes}, [](const SoundRecord& r) { return Text(r.filePath); }},
    {{K::Sounds, "eax", "EAX 设置", T::Bytes}, [](const SoundRecord& r) { return Text(r.eaxSetting); }},
    {{K::Sounds, "flags", "标志位", T::UInt32}, [](const SoundRecord& r) { return U32(r.flags); }},
    {{K::Sounds, "volume", "音量", T::Int32, true, "-1（默认）或 0..127"},
        [](const SoundRecord& r) { return Int(r.volume); },
        [](SoundRecord& r, const V& v) { r.volume = I32(v); }, SoundVolume},
    {{K::Sounds, "pitch", "音调", T::Float32}, [](const SoundRecord& r) { return F32(r.pitch); }},
    // 同轮 HumanX06 W3S 全表字段不变；仍走共用成员/原值/布局检查。
    {{K::Sounds, "fade_in", "淡入速率", T::Int32, true, "-1（默认）或非负 int32"},
        [](const SoundRecord& r) { return Int(r.fadeInRate); },
        [](SoundRecord& r, const V& v) { r.fadeInRate = I32(v); }, DefaultOrNonNegative},
    {{K::Sounds, "fade_out", "淡出速率", T::Int32, true, "-1（默认）或非负 int32"},
        [](const SoundRecord& r) { return Int(r.fadeOutRate); },
        [](SoundRecord& r, const V& v) { r.fadeOutRate = I32(v); }, DefaultOrNonNegative},
    {{K::Sounds, "channel", "声道", T::Int32}, [](const SoundRecord& r) { return Int(r.channel); }},
    {{K::Sounds, "min_distance", "最小距离", T::Float32}, [](const SoundRecord& r) { return F32(r.minDistance); }},
    {{K::Sounds, "max_distance", "最大距离", T::Float32}, [](const SoundRecord& r) { return F32(r.maxDistance); }},
    {{K::Sounds, "distance_cutoff", "截止距离", T::Float32}, [](const SoundRecord& r) { return F32(r.distanceCutoff); }},
    {{K::Sounds, "extended_repeat_variable_name", "扩展重复变量名", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->repeatVariableName); }},
    {{K::Sounds, "extended_sound_name", "扩展声音名", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->soundName); }},
    {{K::Sounds, "extended_repeat_sound_path", "扩展重复路径", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->repeatSoundPath); }},
    {{K::Sounds, "extended_dialogue_text_key", "对话文本键", T::Int32},
        [](const SoundRecord& r) { return Int(r.extended->dialogueTextKey); }},
    {{K::Sounds, "extended_unknown2", "扩展未知文本 2", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->unknown2); }},
    {{K::Sounds, "extended_dialogue_speaker_name_key", "说话者名称键", T::Int32},
        [](const SoundRecord& r) { return Int(r.extended->dialogueSpeakerNameKey); }},
    {{K::Sounds, "extended_unknown4", "扩展未知整数 4", T::Int32},
        [](const SoundRecord& r) { return Int(r.extended->unknown4); }},
    {{K::Sounds, "extended_unknown5", "扩展条件文本 5", T::Bytes},
        [](const SoundRecord& r) { return Text(*r.extended->unknown5); }},
    {{K::Sounds, "extended_unknown6", "扩展未知文本 6", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->unknown6); }},
    {{K::Sounds, "extended_unknown7", "扩展未知文本 7", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->unknown7); }},
    {{K::Sounds, "extended_facial_animation_label", "面部动画标签", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->facialAnimationLabel); }},
    {{K::Sounds, "extended_facial_animation_group_label", "面部动画组", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->facialAnimationGroupLabel); }},
    {{K::Sounds, "extended_facial_animation_set_filepath", "面部动画集路径", T::Bytes},
        [](const SoundRecord& r) { return Text(r.extended->facialAnimationSetFilepath); }},
    {{K::Sounds, "extended_unknown11", "扩展未知整数 11", T::Int32},
        [](const SoundRecord& r) { return Int(*r.extended->unknown11); }},
};

const FieldImpl<StringTableEntry> kStringFields[] = {
    {{K::Strings, "id", "字符串编号", T::UInt32}, [](const StringTableEntry& r) { return U32(r.id); }},
    {{K::Strings, "value", "字符串内容", T::Bytes, true, "UTF-8 文字，最多4096字节；共享ID会影响其所有引用"},
        [](const StringTableEntry& r) { return Text(r.value); },
        [](StringTableEntry& r, const V& v) { r.value = v.bytes; }, Utf8Text},
};

const std::vector<MapFieldDescriptor>& Catalog() {
    static const std::vector<MapFieldDescriptor> catalog = [] {
        std::vector<MapFieldDescriptor> all;
        const auto add = [&all](const auto& fields) {
            for (const auto& field : fields) {
                const auto duplicate = std::find_if(all.begin(), all.end(), [&](const auto& existing) {
                    return existing.kind == field.descriptor.kind && existing.id == field.descriptor.id;
                });
                if (duplicate == all.end()) all.push_back(field.descriptor);
            }
        };
        add(kSettingsFields); add(kUnitFields); add(kDoodadFields); add(kSpecialDoodadFields);
        add(kRegionFields); add(kCameraFields); add(kSoundFields);
        add(kStringFields);
        return all;
    }();
    return catalog;
}

// 同一套读写入口同时服务源文档与可写副本。
struct TableView {
    const MapSettings* settings = nullptr;
    const UnitPlacements* units = nullptr;
    const DoodadPlacements* doodads = nullptr;
    const RegionsDocument* regions = nullptr;
    const CamerasDocument* cameras = nullptr;
    const SoundsDocument* sounds = nullptr;
    const StringTableDocument* strings = nullptr;
};

template<class Table>
const Table* Pointer(const std::optional<Table>& table) { return table ? &*table : nullptr; }

TableView View(const MapDocument& document) {
    return {Pointer(document.Settings()), Pointer(document.Units()), Pointer(document.Doodads()), Pointer(document.Regions()),
        Pointer(document.Cameras()), Pointer(document.Sounds()), Pointer(document.Strings())};
}

TableView View(const MapFieldTables& tables) {
    return {Pointer(tables.settings), Pointer(tables.units), nullptr, Pointer(tables.regions),
        Pointer(tables.cameras), Pointer(tables.sounds), Pointer(tables.strings)};
}

template<class Record, class Rows>
Record* RowPointer(Rows& rows, std::size_t row) { return row < rows.size() ? &rows[row] : nullptr; }

// fn(fields, record)；类别不支持字段或行不存在时 record 为空。
template<class Fn>
auto DispatchRow(const TableView& view, MapDataKind kind, std::size_t row, Fn&& fn) {
    switch (kind) {
    case K::Settings:
        return fn(std::span(kSettingsFields), view.settings && row == 0 ? view.settings : nullptr);
    case K::Units:
        return fn(std::span(kUnitFields), view.units ? RowPointer<const UnitPlacement>(view.units->units, row) : nullptr);
    case K::Doodads:
        if (!view.doodads || row < view.doodads->doodads.size())
            return fn(std::span(kDoodadFields),
                view.doodads ? RowPointer<const DoodadPlacement>(view.doodads->doodads, row) : nullptr);
        return fn(std::span(kSpecialDoodadFields),
            RowPointer<const SpecialDoodadPlacement>(view.doodads->specialDoodads,
                row - view.doodads->doodads.size()));
    case K::Regions:
        return fn(std::span(kRegionFields),
            view.regions ? RowPointer<const RegionRecord>(view.regions->entries, row) : nullptr);
    case K::Cameras:
        return fn(std::span(kCameraFields),
            view.cameras ? RowPointer<const CameraRecord>(view.cameras->entries, row) : nullptr);
    case K::Sounds:
        return fn(std::span(kSoundFields),
            view.sounds ? RowPointer<const SoundRecord>(view.sounds->entries, row) : nullptr);
    case K::Strings:
        return fn(std::span(kStringFields),
            view.strings ? RowPointer<const StringTableEntry>(view.strings->entries, row) : nullptr);
    default:
        return fn(std::span<const FieldImpl<MapSettings>>(), static_cast<const MapSettings*>(nullptr));
    }
}

template<class Fn>
auto DispatchMutableRow(MapFieldTables& tables, MapDataKind kind, std::size_t row, Fn&& fn) {
    switch (kind) {
    case K::Settings: return fn(std::span(kSettingsFields), tables.settings && row == 0 ? &*tables.settings : nullptr);
    case K::Units:
        return fn(std::span(kUnitFields), tables.units ? RowPointer<UnitPlacement>(tables.units->units, row) : nullptr);
    case K::Regions:
        return fn(std::span(kRegionFields),
            tables.regions ? RowPointer<RegionRecord>(tables.regions->entries, row) : nullptr);
    case K::Cameras:
        return fn(std::span(kCameraFields),
            tables.cameras ? RowPointer<CameraRecord>(tables.cameras->entries, row) : nullptr);
    case K::Sounds:
        return fn(std::span(kSoundFields), tables.sounds ? RowPointer<SoundRecord>(tables.sounds->entries, row) : nullptr);
    case K::Strings:
        return fn(std::span(kStringFields), tables.strings ? RowPointer<StringTableEntry>(tables.strings->entries, row) : nullptr);
    default:
        return fn(std::span<const FieldImpl<MapSettings>>(), static_cast<MapSettings*>(nullptr));
    }
}

template<class Fields>
auto FindImpl(const Fields& fields, std::string_view id) {
    const auto found = std::find_if(fields.begin(), fields.end(), [id](const auto& field) {
        return field.descriptor.id == id;
    });
    return found == fields.end() ? nullptr : &*found;
}

Result<MapFieldValue> ReadField(const TableView& view, MapDataKind kind, std::size_t row,
    const MapFieldDescriptor& field) {
    if (kind == K::Cameras && field.id.starts_with("extended_local_") && view.cameras
        && row < view.cameras->entries.size() && !view.cameras->entries[row].localRotation) {
        return Fail<MapFieldValue>("MAP_FIELD_NOT_APPLICABLE",
            "字段 " + std::string(field.id) + " 不存在于经典镜头记录中。");
    }
    if (kind == K::Sounds && field.id.starts_with("extended_") && view.sounds
        && row < view.sounds->entries.size()) {
        const auto& record = view.sounds->entries[row];
        if (!record.extended || (field.id == "extended_unknown5" && !record.extended->unknown5)
            || (field.id == "extended_unknown11" && !record.extended->unknown11)) {
            return Fail<MapFieldValue>("MAP_FIELD_NOT_APPLICABLE",
                "字段 " + std::string(field.id) + " 不存在于该声音记录的版本或条件分支中。");
        }
    }
    return DispatchRow(view, kind, row, [&](auto fields, const auto* record) -> Result<MapFieldValue> {
        const auto* impl = FindImpl(fields, field.id);
        if (field.kind != kind || !FindMapField(kind, field.id)) return Fail<MapFieldValue>("MAP_FIELD_UNKNOWN",
            "类别 " + std::string(MapDataKindId(kind)) + " 没有字段 " + std::string(field.id) + "。");
        if (!record) return Fail<MapFieldValue>("MAP_FIELD_ROW_MISSING",
            "类别 " + std::string(MapDataKindId(kind)) + " 的第 " + std::to_string(row) + " 行不存在或成员未解析。");
        if (!impl) return Fail<MapFieldValue>("MAP_FIELD_NOT_APPLICABLE",
            "字段 " + std::string(field.id) + " 不适用于第 " + std::to_string(row) + " 行的记录形式。");
        if (impl->present && !impl->present(*record)) return Fail<MapFieldValue>("MAP_FIELD_NOT_APPLICABLE",
            "字段 " + std::string(field.id) + " 不存在于当前记录布局中。");
        return Result<MapFieldValue>::Success(impl->read(*record));
    });
}

Result<std::vector<std::byte>> SerializeTable(const TableView& view, MapDataKind kind) {
    const auto missing = [kind] {
        return Fail<std::vector<std::byte>>("MAP_FIELD_TABLE_MISSING",
            "类别 " + std::string(MapDataKindId(kind)) + " 未加载，不能序列化。");
    };
    switch (kind) {
    case K::Settings: return view.settings ? view.settings->Serialize() : missing();
    case K::Units: return view.units ? view.units->Serialize() : missing();
    case K::Regions: return view.regions ? view.regions->Serialize() : missing();
    case K::Cameras: return view.cameras ? view.cameras->Serialize() : missing();
    case K::Sounds: return view.sounds ? view.sounds->Serialize() : missing();
    case K::Strings: return view.strings ? view.strings->Serialize() : missing();
    default: return Fail<std::vector<std::byte>>("MAP_FIELD_KIND_UNSUPPORTED",
        "类别 " + std::string(MapDataKindId(kind)) + " 没有字段目录。");
    }
}

Status CopyTable(const MapDocument& source, MapDataKind kind, MapFieldTables& tables) {
    const auto copy = [](const auto& from, auto& to) {
        if (!to && from) to = *from;
        return static_cast<bool>(to);
    };
    bool present = false;
    switch (kind) {
    case K::Settings: present = copy(source.Settings(), tables.settings); break;
    case K::Units: present = copy(source.Units(), tables.units); break;
    case K::Regions: present = copy(source.Regions(), tables.regions); break;
    case K::Cameras: present = copy(source.Cameras(), tables.cameras); break;
    case K::Sounds: present = copy(source.Sounds(), tables.sounds); break;
    case K::Strings: present = copy(source.Strings(), tables.strings); break;
    default: break;
    }
    if (present) return Status::Ok();
    return Status::Failure("MapData.Fields", "MAP_FIELD_TABLE_MISSING",
        "类别 " + std::string(MapDataKindId(kind)) + " 未加载，不能修改。");
}

std::string Hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) {
        text.push_back(digits[c >> 4]);
        text.push_back(digits[c & 15]);
    }
    return text;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

template<class Number>
std::optional<Number> Decimal(std::string_view text) {
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::string_view MapDataKindId(MapDataKind kind) noexcept {
    switch (kind) {
    case K::Settings: return "settings";
    case K::Units: return "units";
    case K::Doodads: return "doodads";
    case K::Regions: return "regions";
    case K::Cameras: return "cameras";
    case K::Sounds: return "sounds";
    case K::Strings: return "strings";
    case K::Count: break;
    }
    return "invalid";
}

std::optional<MapDataKind> ParseMapDataKindId(std::string_view text) noexcept {
    for (std::size_t index = 0; index < static_cast<std::size_t>(K::Count); ++index) {
        const auto kind = static_cast<MapDataKind>(index);
        if (MapDataKindId(kind) == text) return kind;
    }
    return std::nullopt;
}

std::string_view MapFieldTypeId(MapFieldType type) noexcept {
    switch (type) {
    case T::Int32: return "int32";
    case T::UInt32: return "uint32";
    case T::Float32: return "float32";
    case T::AngleRadians: return "angle";
    case T::Bytes: return "bytes";
    }
    return "invalid";
}

std::span<const MapFieldDescriptor> MapFieldCatalog() { return Catalog(); }

std::span<const MapFieldDescriptor> MapFieldCatalog(MapDataKind kind) {
    const auto& all = Catalog();
    const auto first = std::find_if(all.begin(), all.end(), [kind](const auto& field) { return field.kind == kind; });
    const auto last = std::find_if(first, all.end(), [kind](const auto& field) { return field.kind != kind; });
    return {all.data() + (first - all.begin()), static_cast<std::size_t>(last - first)};
}

const MapFieldDescriptor* FindMapField(MapDataKind kind, std::string_view id) {
    const auto fields = MapFieldCatalog(kind);
    const auto found = std::find_if(fields.begin(), fields.end(), [id](const auto& field) { return field.id == id; });
    return found == fields.end() ? nullptr : &*found;
}

Status ValidateMapFieldValue(MapDataKind kind, std::string_view fieldId, const MapFieldValue& input) {
    const auto name = std::string(MapDataKindId(kind)) + "." + std::string(fieldId);
    const auto* field = FindMapField(kind, fieldId);
    if (!field) return Status::Failure("MapData.Fields", "MAP_FIELD_UNKNOWN", "字段目录中没有 " + name + "。");
    if (!field->writable) return Status::Failure("MapData.Fields", "MAP_FIELD_READ_ONLY",
        "字段 " + name + " 尚无原版保存证据，只能读取。");
    const auto value = Normalized(input);
    if (value.type != field->type) return Status::Failure("MapData.Fields", "MAP_FIELD_VALUE_INVALID",
        "字段 " + name + " 的值类型应为 " + std::string(MapFieldTypeId(field->type)) + "。");
    return DispatchRow(TableView{}, kind, 0, [&](auto fields, const auto*) -> Status {
        const auto* impl = FindImpl(fields, fieldId);
        if (!impl || !impl->write) return Status::Failure("MapData.Fields", "MAP_FIELD_READ_ONLY",
            "字段 " + name + " 尚无原版保存证据，只能读取。");
        if (impl->valid && !impl->valid(value)) return Status::Failure("MapData.Fields", "MAP_FIELD_VALUE_INVALID",
            "字段 " + name + " 的新值不满足约束：" + std::string(field->constraint) + "。");
        return Status::Ok();
    });
}

Status ValidateMapFieldRegionBounds(float left, float bottom, float right, float top) {
    // 同一行的多个字段先写入，再整体检查；各边复用目录的坐标谓词。
    const auto invalid = [] {
        return Status::Failure("MapData.Fields", "MAP_FIELD_ROW_INVALID",
            "修改后区域行不满足行级约束（区域须 left < right、bottom < top）。");
    };
    for (const float value : {left, bottom, right, top}) {
        if (!Coordinate(F32(value))) return invalid();
    }
    if (!(left < right) || !(bottom < top)) return invalid();
    return Status::Ok();
}

std::string FormatMapFieldRaw(const MapFieldValue& value) {
    switch (value.type) {
    case T::Int32:
    case T::UInt32: return std::to_string(value.integer);
    case T::Float32:
    case T::AngleRadians: {
        constexpr char digits[] = "0123456789abcdef";
        std::string text = "f32:";
        for (int shift = 28; shift >= 0; shift -= 4) text.push_back(digits[(value.floatBits >> shift) & 15]);
        return text;
    }
    case T::Bytes: return "hex:" + Hex(value.bytes);
    }
    return {};
}

Result<MapFieldValue> ParseMapFieldRaw(MapFieldType type, std::string_view text) {
    const auto invalid = [type] {
        return Fail<MapFieldValue>("MAP_FIELD_RAW_INVALID", "原值文本不是 " + std::string(MapFieldTypeId(type))
            + " 的精确格式（整数十进制、f32:8 位小写十六进制、hex:小写十六进制）。");
    };
    switch (type) {
    case T::Int32: {
        const auto number = Decimal<std::int64_t>(text);
        if (!number || *number < std::numeric_limits<std::int32_t>::min()
            || *number > std::numeric_limits<std::int32_t>::max()) return invalid();
        return Result<MapFieldValue>::Success(Int(*number));
    }
    case T::UInt32: {
        const auto number = Decimal<std::uint64_t>(text);
        if (!number || *number > std::numeric_limits<std::uint32_t>::max()) return invalid();
        return Result<MapFieldValue>::Success(U32(static_cast<std::uint32_t>(*number)));
    }
    case T::Float32:
    case T::AngleRadians: {
        if (!text.starts_with("f32:") || text.size() != 12) return invalid();
        std::uint32_t bits = 0;
        for (const char c : text.substr(4)) {
            const int digit = HexDigit(c);
            if (digit < 0) return invalid();
            bits = bits << 4 | static_cast<std::uint32_t>(digit);
        }
        MapFieldValue value;
        value.type = type;
        value.floatBits = bits;
        return Result<MapFieldValue>::Success(std::move(value));
    }
    case T::Bytes: {
        if (!text.starts_with("hex:")) return invalid();
        const auto hex = text.substr(4);
        if (hex.size() % 2 != 0 || hex.size() > kMaxBytesField * 2) return invalid();
        std::string bytes;
        bytes.reserve(hex.size() / 2);
        for (std::size_t index = 0; index < hex.size(); index += 2) {
            const int high = HexDigit(hex[index]), low = HexDigit(hex[index + 1]);
            if (high < 0 || low < 0) return invalid();
            bytes.push_back(static_cast<char>(high * 16 + low));
        }
        return Result<MapFieldValue>::Success(Text(bytes));
    }
    }
    return invalid();
}

Result<MapFieldValue> ParseMapFieldInput(const MapFieldDescriptor& field, std::string_view text) {
    if (field.quantity != MapFieldQuantity::None && (text == "默认" || text == "default"))
        return Result<MapFieldValue>::Success(Int(-1));
    if (text.starts_with("f32:") || text.starts_with("hex:")) return ParseMapFieldRaw(field.type, text);
    const auto invalid = [&field] {
        return Fail<MapFieldValue>("MAP_FIELD_INPUT_INVALID", "字段 " + std::string(field.id)
            + " 的新值无法按 " + std::string(MapFieldTypeId(field.type)) + " 解析。");
    };
    switch (field.type) {
    case T::Int32:
    case T::UInt32: return ParseMapFieldRaw(field.type, text);
    case T::Float32: {
        if (IsUnitPositionField(field)) {
            // 先验证输入的 double，不能让超界十进制舍入成边界 float 后获准。
            const auto number = Decimal<double>(text);
            if (!number || !std::isfinite(*number) || std::abs(*number) > static_cast<double>(kMaxCoordinate))
                return invalid();
            return Result<MapFieldValue>::Success(F32(static_cast<float>(*number)));
        }
        const auto number = Decimal<float>(text);
        if (!number || !std::isfinite(*number)) return invalid();
        return Result<MapFieldValue>::Success(F32(*number));
    }
    case T::AngleRadians: {
        // 与原版编辑器输入一致：先按 float 取角度，再以 double 换算弧度。
        const auto degrees = Decimal<float>(text);
        if (!degrees || !std::isfinite(*degrees)) return invalid();
        return Result<MapFieldValue>::Success(Angle(static_cast<float>(
            static_cast<double>(*degrees) * std::numbers::pi_v<double> / 180.0)));
    }
    case T::Bytes:
        if (text.size() > kMaxBytesField) return invalid();
        return Result<MapFieldValue>::Success(Text(std::string(text)));
    }
    return invalid();
}

Result<MapFieldValue> ParseMapFieldInput(const MapFieldDescriptor& field, std::string_view text,
    const MapFieldQuantityInput& quantity) {
    if (field.quantity == MapFieldQuantity::None) return ParseMapFieldInput(field, text);
    if (text == "默认" || text == "default" || text == "-1") return Result<MapFieldValue>::Success(Int(-1));
    const bool percent = text.ends_with('%');
    const auto number = Decimal<double>(percent ? text.substr(0, text.size() - 1) : text);
    if (!number || !std::isfinite(*number) || *number < 0 || (percent && *number > 100))
        return Fail<MapFieldValue>("MAP_FIELD_INPUT_INVALID", "数量须为非负数、0..100% 或默认。");
    const bool storedPercent = field.quantity == MapFieldQuantity::PercentOfMaximum;
    if (storedPercent && percent) return Result<MapFieldValue>::Success(Int(static_cast<std::int64_t>(std::round(*number))));
    if (!storedPercent && !percent && !quantity.maximum) {
        if (*number > std::numeric_limits<std::int32_t>::max())
            return Fail<MapFieldValue>("MAP_FIELD_INPUT_INVALID", "绝对数量不能超过地图整数存储范围。");
        // W3do 魔法原值不依赖英雄上限；1.27a SetUnitState 在初始化时按当时资源上限夹取。
        auto result = Result<MapFieldValue>::Success(Int(static_cast<std::int64_t>(std::round(*number))));
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "MapData.Fields", "MAP_FIELD_RUNTIME_CLAMP",
            "保存绝对数量原值；静态上限未知，游戏初始化时按当时上限截断，不能据此预言游戏最终数值。"});
        return result;
    }
    if (!quantity.maximum || !std::isfinite(*quantity.maximum) || *quantity.maximum < 0
        || *quantity.maximum > std::numeric_limits<std::int32_t>::max() || (storedPercent && *quantity.maximum == 0))
        return Fail<MapFieldValue>("MAP_FIELD_MAXIMUM_UNKNOWN", "缺少已知物编上限，不能换算该数量；可保留默认、填写生命百分比或魔法绝对值。");
    double stored = percent ? *quantity.maximum * (*number / 100.0) : std::min(*number, *quantity.maximum);
    if (storedPercent) stored = stored / *quantity.maximum * 100.0;
    return Result<MapFieldValue>::Success(Int(static_cast<std::int64_t>(std::round(stored))));
}

std::optional<double> MapFieldNumber(const MapFieldValue& value) {
    switch (value.type) {
    case T::Int32:
    case T::UInt32: return static_cast<double>(value.integer);
    case T::Float32:
        if (std::isfinite(Float(value))) return Float(value);
        return std::nullopt;
    case T::AngleRadians:
        if (std::isfinite(Float(value))) return static_cast<double>(Float(value)) * 180.0 / std::numbers::pi_v<double>;
        return std::nullopt;
    case T::Bytes: return std::nullopt;
    }
    return std::nullopt;
}

bool IsMapFieldDefault(const MapFieldDescriptor& field, const MapFieldValue& value) noexcept {
    if (field.quantity != MapFieldQuantity::None && value.type == T::Int32) return value.integer == -1;
    if (field.kind != MapDataKind::Sounds || field.type != value.type) return false;
    if (field.id.starts_with("extended_")) return false;
    if (value.type == MapFieldType::Int32)
        return value.integer == kSoundDefaultInteger;
    if (value.type == MapFieldType::Float32)
        return value.floatBits == kSoundDefaultRealBits;
    return false;
}

std::optional<double> MapFieldDisplayNumber(const MapFieldDescriptor& field,
    const MapFieldValue& value) {
    if (IsMapFieldDefault(field, value)) return std::nullopt;
    return MapFieldNumber(value);
}

Result<std::size_t> MapFieldRowCount(const MapDocument& document, MapDataKind kind) {
    const auto count = [](std::size_t value) { return Result<std::size_t>::Success(value); };
    switch (kind) {
    case K::Settings: if (document.Settings()) return count(1); break;
    case K::Units: if (document.Units()) return count(document.Units()->units.size()); break;
    case K::Doodads:
        if (document.Doodads())
            return count(document.Doodads()->doodads.size() + document.Doodads()->specialDoodads.size());
        break;
    case K::Regions: if (document.Regions()) return count(document.Regions()->entries.size()); break;
    case K::Cameras: if (document.Cameras()) return count(document.Cameras()->entries.size()); break;
    case K::Sounds: if (document.Sounds()) return count(document.Sounds()->entries.size()); break;
    case K::Strings: if (document.Strings()) return count(document.Strings()->entries.size()); break;
    case K::Count: break;
    }
    return Fail<std::size_t>("MAP_FIELD_TABLE_MISSING", "类别 " + std::string(MapDataKindId(kind)) + " 未加载。");
}

Result<std::string> MapFieldRowKey(const MapDocument& document, MapDataKind kind, std::size_t row) {
    const auto rows = MapFieldRowCount(document, kind);
    if (!rows.Succeeded() || !rows.value) return Forward<std::string>(rows.diagnostics);
    if (row >= *rows.value) return Fail<std::string>("MAP_FIELD_ROW_MISSING",
        "类别 " + std::string(MapDataKindId(kind)) + " 的第 " + std::to_string(row) + " 行不存在。");
    std::string key;
    switch (kind) {
    case K::Settings: key = "w3i"; break;
    case K::Units: {
        const auto& unit = document.Units()->units[row];
        key = "unit:" + Hex(unit.rawcode) + ":" + std::to_string(unit.creationId);
        break;
    }
    case K::Doodads: {
        const auto& doodads = *document.Doodads();
        if (row < doodads.doodads.size()) {
            const auto& doodad = doodads.doodads[row];
            key = "doodad:" + Hex(doodad.rawcode) + ":" + std::to_string(doodad.creationId);
        } else {
            const auto& special = doodads.specialDoodads[row - doodads.doodads.size()];
            key = "special:" + Hex(special.rawcode) + ":" + std::to_string(special.position[0])
                + ":" + std::to_string(special.position[1]);
        }
        break;
    }
    case K::Regions: {
        const auto& region = document.Regions()->entries[row];
        key = "region:" + std::to_string(region.creationNumber) + ":" + Hex(region.name);
        break;
    }
    case K::Cameras: key = "camera:" + Hex(document.Cameras()->entries[row].name); break;
    case K::Sounds: key = "sound:" + Hex(document.Sounds()->entries[row].name); break;
    case K::Strings: key = "string:" + std::to_string(document.Strings()->entries[row].id); break;
    case K::Count: break;
    }
    return Result<std::string>::Success(std::move(key));
}

Result<MapFieldValue> ReadMapField(const MapDocument& document, MapDataKind kind,
    std::size_t row, const MapFieldDescriptor& field) {
    return ReadField(View(document), kind, row, field);
}

Result<MapFieldValue> ReadMapField(const MapFieldTables& tables, MapDataKind kind,
    std::size_t row, const MapFieldDescriptor& field) {
    return ReadField(View(tables), kind, row, field);
}

Result<std::vector<std::byte>> SerializeMapFieldTable(const MapDocument& document, MapDataKind kind) {
    return SerializeTable(View(document), kind);
}

Result<std::vector<std::byte>> SerializeMapFieldTable(const MapFieldTables& tables, MapDataKind kind) {
    return SerializeTable(View(tables), kind);
}

Result<MapFieldTables> ParseMapFieldTable(MapDataKind kind, std::span<const std::byte> bytes) {
    MapFieldTables tables;
    const auto take = [&tables](auto parsed, auto& slot) -> Result<MapFieldTables> {
        if (!parsed.Succeeded() || !parsed.value) return Forward<MapFieldTables>(std::move(parsed.diagnostics));
        slot = std::move(*parsed.value);
        return Result<MapFieldTables>::Success(std::move(tables));
    };
    switch (kind) {
    case K::Settings: return take(MapSettings::Parse(bytes), tables.settings);
    case K::Units: return take(ParseUnitPlacements(bytes), tables.units);
    case K::Regions: return take(ParseRegions(bytes), tables.regions);
    case K::Cameras: return take(ParseCameras(bytes), tables.cameras);
    case K::Sounds: return take(ParseSounds(bytes), tables.sounds);
    case K::Strings: return take(ParseStringTable(bytes), tables.strings);
    default: return Fail<MapFieldTables>("MAP_FIELD_KIND_UNSUPPORTED",
        "类别 " + std::string(MapDataKindId(kind)) + " 没有字段目录。");
    }
}

bool MapFieldLayoutAllowsWrite(const MapDocument& document, const MapFieldDescriptor& field) {
    const auto* known = FindMapField(field.kind, field.id);
    if (!known || !known->writable) return false;
    if (field.kind == K::Strings) return document.Member(K::Strings).state == MapDataState::Ready
        && document.Member(K::Strings).sources.size() == 1 && document.Strings().has_value();
    if (field.kind == K::Settings && field.id != "map_name" && field.id != "game_data_set")
        return document.Member(K::Settings).state == MapDataState::Ready
            && document.Member(K::Settings).sources.size() == 1 && document.Settings()
            && document.Settings()->HasClassicLayout() && document.Settings()->FullyParsed();
    if (IsUnitPositionField(*known)) {
        const auto& units = document.Units();
        return document.Member(K::Units).state == MapDataState::Ready && units
            && units->version == 8 && units->subversion == 11
            && units->recordLayout == PlacementRecordLayout::Classic;
    }
    return field.kind != K::Settings || (document.Settings() && document.Settings()->HasClassicLayout());
}

bool MapFieldRowAllowsWrite(const MapDocument& document, std::size_t row, const MapFieldDescriptor& field) {
    if (!MapFieldLayoutAllowsWrite(document, field)) return false;
    const auto count = MapFieldRowCount(document, field.kind);
    if (!count.Succeeded() || !count.value || row >= *count.value) return false;
    // 起点 sloc 的 XYZ 只经 W3I 关联编辑；起点不是英雄，也不开放英雄等级。
    const bool startLocationLocked = IsUnitPositionField(field) || (field.kind == K::Units && field.id == "hero_level");
    return !startLocationLocked || document.Units()->units[row].rawcode != "sloc";
}

namespace {
Result<std::size_t> SettingsStringRow(const MapDocument& source, std::string_view reference) {
    const auto& member = source.Member(K::Strings);
    if (!source.Strings() || member.sources.size() != 1 || member.memberName != "war3map.wts")
        return Fail<std::size_t>("MAP_FIELD_SOURCE_UNSAFE", "设置字段引用的 WTS 必须来自唯一、已有且完整可解析的原生来源，不能重建缺失字符串表。");
    // 与显示解析共用严格引用语义；缺失、重复、错误编号均不得转成 W3I 字面量。
    const auto resolved = source.Strings()->Resolve(reference);
    if (!resolved.Succeeded() || !resolved.value) return Forward<std::size_t>(resolved.diagnostics);
    if (member.state != MapDataState::Ready)
        return Fail<std::size_t>("MAP_FIELD_SOURCE_UNSAFE", "设置字段引用的 WTS 来源不完整或有重复编号，保留原引用。");
    const auto wire = source.Strings()->Serialize();
    if (!wire.Succeeded() || !wire.value || *wire.value != member.sources.front().data)
        return Fail<std::size_t>("MAP_FIELD_ROUNDTRIP_UNSAFE", "设置字段引用的 WTS 未通过完整原字节往返，保留原引用。");
    std::uint32_t id = 0;
    const auto digits = reference.substr(8);
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
        return Fail<std::size_t>("WTS_REFERENCE_INVALID", "TRIGSTR 引用必须包含完整十进制 ID。");
    const auto& entries = source.Strings()->entries;
    const auto found = std::find_if(entries.begin(), entries.end(), [id](const auto& entry) { return entry.id == id; });
    if (found == entries.end()) return Fail<std::size_t>("WTS_REFERENCE_MISSING", "引用的 WTS 字符串编号不存在。");
    return Result<std::size_t>::Success(static_cast<std::size_t>(found - entries.begin()));
}
} // namespace

Result<MapFieldTables> ApplyMapFieldEdits(const MapDocument& source, std::span<const MapFieldEdit> edits,
    std::size_t maximumEdits) {
    if (edits.empty() || edits.size() > maximumEdits) return Fail<MapFieldTables>("MAP_FIELD_EDIT_COUNT",
        "同一请求须包含 1 到 " + std::to_string(maximumEdits) + " 项字段修改。");
    if (std::any_of(edits.begin(), edits.end(), [](const auto& edit) { return edit.kind == MapDataKind::Sounds; })) {
        const auto checked = ValidateSoundFieldSource(source);
        if (!checked.Succeeded()) return Forward<MapFieldTables>(checked.diagnostics);
    }
    MapFieldTables tables;
    std::set<std::tuple<MapDataKind, std::size_t, std::string>> seen;
    std::set<std::pair<MapDataKind, std::size_t>> touched;
    std::map<std::uint32_t, MapFieldValue> stringWrites;
    std::set<std::uint32_t> sharedWarnings;
    std::vector<Diagnostic> diagnostics;
    for (const auto& edit : edits) {
        const auto name = std::string(MapDataKindId(edit.kind)) + "." + edit.field;
        const auto* field = FindMapField(edit.kind, edit.field);
        if (!field) return Fail<MapFieldTables>("MAP_FIELD_UNKNOWN", "字段目录中没有 " + name + "。");
        if (!field->writable) return Fail<MapFieldTables>("MAP_FIELD_READ_ONLY",
            "字段 " + name + " 尚未开放编辑，只能读取。");
        if (!MapFieldLayoutAllowsWrite(source, *field)) return Fail<MapFieldTables>("MAP_FIELD_LAYOUT_READ_ONLY",
            "字段 " + name + " 的记录布局尚不能编辑；保留原成员。");
        if (!seen.emplace(edit.kind, edit.row, edit.field).second) return Fail<MapFieldTables>(
            "MAP_FIELD_DUPLICATE", "同一请求重复修改了第 " + std::to_string(edit.row) + " 行的 " + name + "。");
        auto key = MapFieldRowKey(source, edit.kind, edit.row);
        if (!key.Succeeded() || !key.value) return Forward<MapFieldTables>(std::move(key.diagnostics));
        if (*key.value != edit.expectedRowKey) return Fail<MapFieldTables>("MAP_FIELD_ROW_STALE",
            "第 " + std::to_string(edit.row) + " 行的身份与请求不符，请重新查询。");
        if (!MapFieldRowAllowsWrite(source, edit.row, *field)) return Fail<MapFieldTables>(
            "MAP_FIELD_POSITION_READ_ONLY", IsUnitPositionField(*field)
                ? "起点 sloc 的 XYZ 须经 W3I 关联编辑，不能由字段命令修改；保留原成员。"
                : "起点 sloc 不是英雄，不能修改英雄等级；保留原成员。");
        auto before = ReadMapField(source, edit.kind, edit.row, *field);
        if (!before.Succeeded() || !before.value) return Forward<MapFieldTables>(std::move(before.diagnostics));
        const auto expected = Normalized(edit.expected), value = Normalized(edit.value);
        if (expected.type != field->type || value.type != field->type) return Fail<MapFieldTables>(
            "MAP_FIELD_VALUE_INVALID", "字段 " + name + " 的值类型应为 " + std::string(MapFieldTypeId(field->type)) + "。");
        if (expected != *before.value) return Fail<MapFieldTables>("MAP_FIELD_VALUE_STALE",
            "字段 " + name + " 的前值与请求不符（当前 " + FormatMapFieldRaw(*before.value) + "）。");
        if (value == *before.value) return Fail<MapFieldTables>("MAP_FIELD_NO_CHANGE",
            "字段 " + name + " 的新值与前值相同。");
        const auto copied = CopyTable(source, edit.kind, tables);
        if (!copied.Succeeded()) return Forward<MapFieldTables>(copied.diagnostics);
        if (const auto checked = ValidateMapFieldValue(edit.kind, field->id, value); !checked.Succeeded())
            return Forward<MapFieldTables>(checked.diagnostics);
        const bool settingsReference = edit.kind == K::Settings && before.value->bytes.starts_with("TRIGSTR_")
            && (field->id == "author" || field->id == "description" || field->id == "recommended_players");
        if (settingsReference) {
            const auto row = SettingsStringRow(source, before.value->bytes);
            if (!row.Succeeded() || !row.value) return Forward<MapFieldTables>(row.diagnostics);
            const auto& entry = source.Strings()->entries[*row.value];
            if (value.bytes == entry.value) return Fail<MapFieldTables>("MAP_FIELD_NO_CHANGE",
                "字段 " + name + " 引用的 WTS 正文与新值相同。");
            const auto [written, inserted] = stringWrites.emplace(entry.id, value);
            if (!inserted && written->second != value) return Fail<MapFieldTables>("MAP_FIELD_STRING_CONFLICT",
                "同一请求给共享 WTS 编号 " + std::to_string(entry.id) + " 指定了不同正文，不能按先后顺序覆盖。");
            const auto copiedStrings = CopyTable(source, K::Strings, tables);
            if (!copiedStrings.Succeeded()) return Forward<MapFieldTables>(copiedStrings.diagnostics);
            tables.strings->entries[*row.value].value = value.bytes;
            if (sharedWarnings.insert(entry.id).second) diagnostics.push_back({DiagnosticSeverity::Warning, "MapData.Fields", "MAP_FIELD_SHARED_STRING",
                "保留 W3I 的 " + before.value->bytes + " 引用，修改 WTS STRING " + std::to_string(entry.id)
                    + " 正文；同编号的其它地图引用也会读取新文字。"});
            continue;
        }
        if (edit.kind == K::Strings && field->id == "value") {
            const auto id = source.Strings()->entries[edit.row].id;
            const auto [written, inserted] = stringWrites.emplace(id, value);
            if (!inserted && written->second != value) return Fail<MapFieldTables>("MAP_FIELD_STRING_CONFLICT",
                "同一请求给共享 WTS 编号 " + std::to_string(id) + " 指定了不同正文，不能按先后顺序覆盖。");
        }
        const auto written = DispatchMutableRow(tables, edit.kind, edit.row, [&](auto fields, auto* record) {
            const auto* impl = FindImpl(fields, field->id);
            if (!impl || !impl->write || !record) return Status::Failure("MapData.Fields", "MAP_FIELD_ROW_MISSING",
                "字段 " + name + " 无法写入第 " + std::to_string(edit.row) + " 行。");
            impl->write(*record, value);
            return Status::Ok();
        });
        if (!written.Succeeded()) return Forward<MapFieldTables>(written.diagnostics);
        touched.emplace(edit.kind, edit.row);
    }
    for (const auto& [kind, row] : touched) {
        if (kind == K::Regions) {
            const auto& region = tables.regions->entries[row];
            if (region.colorBgra[3] != source.Regions()->entries[row].colorBgra[3])
                return Fail<MapFieldTables>("REGION_COLOR_RESERVED", "区域颜色第四字节用途未证实，不能在颜色编辑中改变。");
            if (const auto checked = ValidateMapFieldRegionBounds(region.left, region.bottom, region.right, region.top);
                !checked.Succeeded()) return Fail<MapFieldTables>("MAP_FIELD_ROW_INVALID",
                    "修改后第 " + std::to_string(row) + " 行不满足行级约束（区域须 left < right、bottom < top）。");
        }
    }
    auto result = Result<MapFieldTables>::Success(std::move(tables));
    result.diagnostics = std::move(diagnostics);
    return result;
}

} // namespace War3Map::MapData
