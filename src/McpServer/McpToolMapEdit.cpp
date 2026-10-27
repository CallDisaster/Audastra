#include "McpToolInternal.h"
#include "McpMapRuntimeTools.h"
#include "McpTerrainTextureSlotCommand.h"
#include "McpMapCustomSettingsTools.h"
#include "War3MapCore/ApplicationPaths.h"

#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace War3Map::Mcp {
namespace {

constexpr std::size_t kMaxResponseBytes = 1024 * 1024;

bool OnlyFields(const QJsonObject& object, std::initializer_list<std::string_view> names,
    QJsonObject& error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        const auto field = it.key().toStdString();
        bool known = false;
        for (const auto name : names) known |= field == name;
        if (!known) {
            error = Failure("MCP_MAP_EDIT_FIELD_UNSUPPORTED",
                "地图修改请求包含未获支持的字段：" + field);
            return false;
        }
    }
    return true;
}

std::optional<std::string> RequiredText(const QJsonObject& object, const char* field,
    std::size_t maxBytes, QJsonObject& error) {
    const auto value = object.value(QString::fromLatin1(field));
    if (!value.isString()) {
        error = Failure("MCP_MAP_EDIT_TEXT_INVALID", std::string(field) + " 必须是字符串。");
        return std::nullopt;
    }
    const auto bytes = value.toString().toUtf8().toStdString();
    if (bytes.size() > maxBytes || bytes.find('\0') != std::string::npos) {
        error = Failure("MCP_MAP_EDIT_TEXT_INVALID", std::string(field) + " 超出长度预算或包含 NUL。");
        return std::nullopt;
    }
    return bytes;
}

std::optional<std::string> RequiredSha(const QJsonObject& object, const char* field,
    QJsonObject& error) {
    const auto value = object.value(QString::fromLatin1(field));
    const auto text = value.toString().toStdString();
    if (!value.isString() || !IsLowerHexSha256(text)) {
        error = Failure("MCP_MAP_EDIT_SHA_INVALID", std::string(field) + " 必须是 64 位小写 SHA-256。");
        return std::nullopt;
    }
    return text;
}

std::optional<std::size_t> RowIndex(const QJsonObject& object, QJsonObject& error) {
    auto parsed = ParseUnsignedId(object, "row", true);
    if (!parsed.Succeeded()) {
        error = Failure(parsed);
        return std::nullopt;
    }
    if (*parsed.value > std::numeric_limits<std::size_t>::max()) {
        error = Failure("MCP_MAP_EDIT_INDEX_INVALID", "row 超出本机索引范围。");
        return std::nullopt;
    }
    return static_cast<std::size_t>(*parsed.value);
}

std::optional<std::int32_t> Integer(const QJsonObject& object, const char* field,
    QJsonObject& error) {
    auto parsed = ParseInt32(object, field);
    if (!parsed.Succeeded()) {
        error = Failure(parsed);
        return std::nullopt;
    }
    return *parsed.value;
}

std::optional<float> FiniteFloat(const QJsonObject& object, const char* field,
    QJsonObject& error) {
    const auto value = object.value(QString::fromLatin1(field));
    const double number = value.toDouble();
    if (!value.isDouble() || !std::isfinite(number) ||
        std::abs(number) > std::numeric_limits<float>::max()) {
        error = Failure("MCP_MAP_EDIT_FLOAT_INVALID", std::string(field) + " 必须是有限浮点数。");
        return std::nullopt;
    }
    return static_cast<float>(number);
}

std::optional<double> BoundedPointCoordinate(const QJsonObject& object, const char* field,
    bool vertexSpace, QJsonObject& error) {
    const auto value = object.value(QString::fromLatin1(field));
    const double number = value.toDouble();
    if (!value.isDouble() || !std::isfinite(number) ||
        (vertexSpace ? (number < 0 || number > 65535 || std::floor(number) != number)
                     : std::abs(number) > 1000000)) {
        error = Failure("MCP_MAP_EDIT_TERRAIN_POINT_INVALID", std::string(field) +
            (vertexSpace ? " 必须是 0..65535 的整数顶点索引。" : " 必须是 ±1000000 内的有限世界坐标。"));
        return std::nullopt;
    }
    return number;
}

std::optional<Services::MapEditTerrainStroke> ParseTerrainStroke(const QJsonObject& object,
    QJsonObject& error) {
    const auto kind = object.value("kind");
    if (!kind.isString()) {
        error = Failure("MCP_MAP_EDIT_TERRAIN_KIND_INVALID", "地形笔刷 kind 必须是已支持的字符串。");
        return std::nullopt;
    }
    if (kind.toString() == "texture_slot_replace" || kind.toString() == "texture_slot_append") {
        const auto command = ParseTerrainTextureSlotCommand(object, error);
        if (!command) return {};
        Services::MapEditTerrainStroke result; result.textureSlot = *command;
        return result;
    }
    if (kind.toString() == "block_copy") {
        if (!OnlyFields(object, {"kind", "source_x", "source_y", "width", "height", "destination_x", "destination_y"}, error))
            return std::nullopt;
        const auto sx = Integer(object, "source_x", error), sy = Integer(object, "source_y", error),
            width = Integer(object, "width", error), height = Integer(object, "height", error),
            dx = Integer(object, "destination_x", error), dy = Integer(object, "destination_y", error);
        if (!sx || !sy || !width || !height || !dx || !dy) return std::nullopt;
        if (*sx < 0 || *sy < 0 || *width < 1 || *height < 1 || *dx < 0 || *dy < 0
            || *sx > 2048 || *sy > 2048 || *width > MapData::TerrainBlockMaximumSide || *height > MapData::TerrainBlockMaximumSide || *dx > 2048 || *dy > 2048
            || *sx + *width > 2048 || *sy + *height > 2048 || *dx + *width > 2048 || *dy + *height > 2048) {
            error = Failure("MCP_MAP_EDIT_BLOCK_INVALID", "地形复制需要0..2048内非空源/目标地格半开矩形，每边不超过256地格。");
            return std::nullopt;
        }
        Services::MapEditTerrainStroke result;
        result.blockPaste = MapData::TerrainBlockPasteIntent{1,
            {static_cast<std::uint32_t>(*sx), static_cast<std::uint32_t>(*sy),
                static_cast<std::uint32_t>(*sx + *width), static_cast<std::uint32_t>(*sy + *height)},
            {static_cast<std::uint32_t>(*dx), static_cast<std::uint32_t>(*dy)}, {}};
        return result;
    }
    if (kind.toString() == "canvas_resize") {
        if (!OnlyFields(object, {"kind", "west", "south", "east", "north"}, error)) return std::nullopt;
        const auto west = Integer(object, "west", error), south = Integer(object, "south", error),
            east = Integer(object, "east", error), north = Integer(object, "north", error);
        if (!west || !south || !east || !north) return std::nullopt;
        const auto inDomain = [](std::int64_t value) {
            return value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max();
        };
        if (!inDomain(*west) || !inDomain(*south) || !inDomain(*east) || !inDomain(*north)
            || *west >= *east || *south >= *north) {
            error = Failure("MCP_MAP_EDIT_RESIZE_INVALID", "地图尺寸需要相对当前西南角的非空有符号32位整数地格半开矩形；实际尺寸限制由来源规则校验。");
            return std::nullopt;
        }
        Services::MapEditTerrainStroke result;
        result.resize = MapData::TerrainResizeIntent{1, {static_cast<std::int32_t>(*west), static_cast<std::int32_t>(*south),
            static_cast<std::int32_t>(*east), static_cast<std::int32_t>(*north)}};
        return result;
    }
    if (kind.toString() == "playable_bounds") {
        if (!OnlyFields(object, {"kind", "west", "south", "east", "north"}, error)) return std::nullopt;
        const auto west = Integer(object, "west", error), south = Integer(object, "south", error),
            east = Integer(object, "east", error), north = Integer(object, "north", error);
        if (!west || !south || !east || !north) return std::nullopt;
        if (*west < 0 || *south < 0 || *west >= *east || *south >= *north || *east > 2048 || *north > 2048) {
            error = Failure("MCP_MAP_EDIT_BOUNDS_INVALID", "可玩区需要0..2048内非空整数地格半开矩形，仍须以实际地形和规则校验。");
            return std::nullopt;
        }
        Services::MapEditTerrainStroke result;
        result.bounds = MapData::TerrainBoundsEditIntent{{static_cast<std::uint32_t>(*west), static_cast<std::uint32_t>(*south),
            static_cast<std::uint32_t>(*east), static_cast<std::uint32_t>(*north)}};
        return result;
    }
    const bool blight = kind.toString() == "blight";
    const bool boundary = kind.toString() == "boundary";
    const bool pathing = kind.toString() == "pathing_restriction";
    const bool ramp = kind.toString() == "ramp";
    const bool corner = kind.toString() == "corner_ramp";
    const bool straight = kind.toString() == "straight_ramp";
    const bool nativeRamp = corner || straight;
    const bool texture = kind.toString() == "texture", water = kind.toString() == "water_height";
    const bool removeWater = kind.toString() == "remove_water";
    const bool cliffLevel = kind.toString() == "cliff_level", cliffTexture = kind.toString() == "cliff_texture";
    const bool cliffBrush = cliffLevel || cliffTexture;
    const bool newWater = kind.toString() == "shallow_water" || kind.toString() == "deep_water";
    Services::MapEditTerrainStroke stroke;
    MapData::TerrainWaterStrokeOptions waterOptions;
    if (blight) stroke.brush.kind = MapData::TerrainBrushKind::Blight;
    else if (boundary) stroke.brush.kind = MapData::TerrainBrushKind::Boundary;
    else if (pathing) stroke.brush.kind = MapData::TerrainBrushKind::PathingRestriction;
    else if (ramp) stroke.brush.kind = MapData::TerrainBrushKind::Ramp;
    else if (corner) stroke.brush.kind = MapData::TerrainBrushKind::CornerRamp;
    else if (straight) stroke.brush.kind = MapData::TerrainBrushKind::StraightRamp;
    else if (kind.toString() == "raise") stroke.brush.kind = MapData::TerrainBrushKind::Raise;
    else if (kind.toString() == "lower") stroke.brush.kind = MapData::TerrainBrushKind::Lower;
    else if (kind.toString() == "smooth") stroke.brush.kind = MapData::TerrainBrushKind::Smooth;
    else if (texture) stroke.brush.kind = MapData::TerrainBrushKind::Texture;
    else if (water) stroke.brush.kind = MapData::TerrainBrushKind::WaterHeight;
    else if (removeWater) stroke.brush.kind = MapData::TerrainBrushKind::RemoveWater;
    else if (cliffLevel) stroke.brush.kind = MapData::TerrainBrushKind::CliffLevel;
    else if (cliffTexture) stroke.brush.kind = MapData::TerrainBrushKind::CliffTexture;
    else if (newWater) waterOptions.kind = kind.toString() == "shallow_water"
        ? MapData::TerrainWaterBrushKind::Shallow : MapData::TerrainWaterBrushKind::Deep;
    else {
        error = Failure("MCP_MAP_EDIT_TERRAIN_KIND_UNSUPPORTED",
            "当前接受 raise/lower/smooth/blight/boundary/pathing_restriction/ramp/corner_ramp/straight_ramp/texture/cliff_level/cliff_texture/water_height/remove_water/shallow_water/deep_water；通行性仍由同一保存服务核验。");
        return std::nullopt;
    }
    if (!OnlyFields(object, nativeRamp
            ? std::initializer_list<std::string_view>{"kind", "point_space", "points", "follow_doodads", "set_ramp"}
            : pathing
            ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "size",
                  "points", "follow_doodads", "blocked_type", "value"}
            : newWater
            ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "size", "cliff_texture",
                  "seed", "enforce_water_limits", "points", "follow_doodads"}
            : cliffLevel
            ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "size", "cliff_texture",
                  "level_delta", "seed", "points", "follow_doodads"}
            : cliffTexture
            ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "size", "cliff_texture",
                  "points", "follow_doodads"}
            : boundary
            ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "radius",
                  "points", "follow_doodads", "value"}
            : blight
            ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "radius",
                  "points", "follow_doodads", "set_blight"}
            : ramp
            ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "radius",
                  "points", "follow_doodads", "set_ramp"}
            : texture ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "radius",
                  "points", "follow_doodads", "ground_texture"}
            : water ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "radius",
                  "points", "follow_doodads", "water_height"}
            : removeWater ? std::initializer_list<std::string_view>{"kind", "shape", "point_space", "radius",
                  "points", "follow_doodads"}
            : std::initializer_list<std::string_view>{"kind", "shape", "point_space", "radius",
                  "points", "follow_doodads", "strength"}, error)) return std::nullopt;
    const auto shape = nativeRamp ? QJsonValue("circle") : object.value("shape");
    if (!shape.isString()) {
        error = Failure("MCP_MAP_EDIT_TERRAIN_SHAPE_INVALID", "笔刷形状必须是 circle 或 square。");
        return std::nullopt;
    }
    if (ramp && shape.toString() != "circle") {
        error = Failure("MCP_MAP_EDIT_TERRAIN_SHAPE_INVALID", "坡道笔刷形状须为circle；三点形态由共用规划器确定。");
        return std::nullopt;
    }
    if (shape.toString() == "circle" || shape.toString() == "square") {
        const auto parsed = shape.toString() == "circle"
            ? MapData::TerrainBrushShape::Circle : MapData::TerrainBrushShape::Square;
        if (newWater) waterOptions.shape = parsed;
        else stroke.brush.shape = parsed;
    }
    else {
        error = Failure("MCP_MAP_EDIT_TERRAIN_SHAPE_INVALID", "笔刷形状必须是 circle 或 square。");
        return std::nullopt;
    }
    const auto space = object.value("point_space");
    if (!space.isString()) {
        error = Failure("MCP_MAP_EDIT_TERRAIN_SPACE_INVALID", "点坐标系必须是 world 或 terrain_vertex。");
        return std::nullopt;
    }
    const bool vertexSpace = space.toString() == "terrain_vertex";
    if (nativeRamp && space.toString() != "world") {
        error = Failure("MCP_MAP_EDIT_TERRAIN_SPACE_INVALID", "实际规则坡道只接受单个规范世界坐标点，不自动吸附。");
        return std::nullopt;
    }
    if (vertexSpace) stroke.pointSpace = Services::MapEditPointSpace::TerrainVertex;
    else if (space.toString() == "world") stroke.pointSpace = Services::MapEditPointSpace::World;
    else {
        error = Failure("MCP_MAP_EDIT_TERRAIN_SPACE_INVALID", "点坐标系必须是 world 或 terrain_vertex。");
        return std::nullopt;
    }
    if (newWater) {
        const auto size = Integer(object, "size", error);
        if (!size) return std::nullopt;
        const auto cliff = Integer(object, "cliff_texture", error);
        if (!cliff) return std::nullopt;
        const auto seedValue = object.value("seed");
        const auto seed = seedValue.toDouble(-1);
        const auto enforce = object.value("enforce_water_limits");
        if (*size < 1 || *size > 8 || *cliff < 0 || *cliff > 14 || !seedValue.isDouble()
            || !std::isfinite(seed) || std::trunc(seed) != seed || seed < 0 || seed > 4294967295.0
            || !enforce.isBool()) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_WATER_BRUSH_INVALID",
                "水刷须明确指定 size=1..8、cliff_texture=0..14、uint32 seed 与 enforce_water_limits 布尔值。");
            return std::nullopt;
        }
        waterOptions.size = static_cast<std::uint32_t>(*size);
        waterOptions.cliffTexture = static_cast<std::uint8_t>(*cliff);
        waterOptions.randomSeed = static_cast<std::uint32_t>(seed);
        waterOptions.enforceWaterLimits = enforce.toBool();
        stroke.waterBrush = waterOptions;
    } else if (pathing) {
        const auto size = Integer(object, "size", error);
        if (!size) return std::nullopt;
        if (*size < 1 || *size > 8) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_PATHING_INVALID", "通行笔刷须显式指定整数 size=1..8，与界面按32单位通行格取同一足迹。");
            return std::nullopt;
        }
        stroke.brush.size = static_cast<std::uint32_t>(*size);
    } else if (cliffBrush) {
        const auto size = Integer(object, "size", error);
        if (!size) return std::nullopt;
        const auto slot = Integer(object, "cliff_texture", error);
        if (!slot) return std::nullopt;
        if (*size < 1 || *size > 8 || *slot < 0 || *slot > 14) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID", "悬崖笔刷须指定整数 size=1..8 与 cliff_texture=0..14；实际槽位仍由源图核验。");
            return std::nullopt;
        }
        stroke.brush.size = static_cast<std::uint32_t>(*size);
        stroke.brush.cliffTexture = static_cast<std::uint8_t>(*slot);
        if (cliffLevel) {
            const auto delta = Integer(object, "level_delta", error);
            if (!delta) return std::nullopt;
            const auto seedValue = object.value("seed");
            const auto seed = seedValue.toDouble(-1);
            if (*delta < -1 || *delta > 1 || !seedValue.isDouble() || !std::isfinite(seed)
                || std::trunc(seed) != seed || seed < 0 || seed > 4294967295.0) {
                error = Failure("MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID", "悬崖层级须指定整数 level_delta=-1..1 与 uint32 seed；水位限制固定启用。");
                return std::nullopt;
            }
            stroke.brush.levelDelta = *delta;
            stroke.brush.seed = static_cast<std::uint32_t>(seed);
        }
    } else if (!nativeRamp) {
        const auto radius = Integer(object, "radius", error);
        if (!radius) return std::nullopt;
        if (*radius < 0 || *radius > 16 || (ramp && *radius != 0)) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_RADIUS_INVALID", ramp
                ? "坡道笔刷半径须为0；严格干地直台阶的三点形态由共用规划器确定。"
                : "笔刷半径必须为 0..16 个顶点。");
            return std::nullopt;
        }
        stroke.brush.radius = static_cast<std::uint32_t>(*radius);
    }
    const auto follow = object.value("follow_doodads");
    if (!follow.isBool() || follow.toBool()) {
        error = Failure("MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED",
            "地物随高度移动尚无原版验收；follow_doodads 必须显式为 false。");
        return std::nullopt;
    }
    stroke.followDoodads = false;
    if (pathing) {
        const auto blockedType = object.value("blocked_type");
        const auto value = object.value("value");
        if (!blockedType.isString() || !value.isBool()) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_PATHING_INVALID", "通行笔刷须显式指定 blocked_type=walk/fly/build 与 value 布尔值。");
            return std::nullopt;
        }
        if (blockedType.toString() == "walk") stroke.brush.pathingRestrictions = 0x02;
        else if (blockedType.toString() == "fly") stroke.brush.pathingRestrictions = 0x04;
        else if (blockedType.toString() == "build") stroke.brush.pathingRestrictions = 0x08;
        else {
            error = Failure("MCP_MAP_EDIT_TERRAIN_PATHING_INVALID", "blocked_type 只接受 walk、fly 或 build；不接受原始通行位。");
            return std::nullopt;
        }
        stroke.brush.value = value.toBool();
    } else if (boundary) {
        const auto value = object.value("value");
        if (!value.isBool()) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_BOUNDARY_INVALID", "边界笔刷必须显式指定 value 布尔值。");
            return std::nullopt;
        }
        stroke.brush.value = value.toBool();
    } else if (blight || ramp || nativeRamp) {
        const auto set = object.value(blight ? "set_blight" : "set_ramp");
        if (!set.isBool()) {
            error = Failure(!blight ? "MCP_MAP_EDIT_TERRAIN_RAMP_INVALID" : "MCP_MAP_EDIT_TERRAIN_BLIGHT_INVALID",
                !blight ? "坡道笔刷必须显式指定 set_ramp 布尔值。" : "荒芜笔刷必须显式指定 set_blight 布尔值。");
            return std::nullopt;
        }
        stroke.brush.value = set.toBool();
    } else if (texture) {
        const auto id = RequiredText(object, "ground_texture", 4, error);
        if (!id) return std::nullopt;
        stroke.brush.groundTexture = *id;
        if (stroke.brush.groundTexture.size() != 4 || !std::all_of(stroke.brush.groundTexture.begin(),
            stroke.brush.groundTexture.end(), [](unsigned char c) { return c >= 33 && c <= 126; })) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_TEXTURE_INVALID", "ground_texture 必须是四字节 ASCII rawcode。");
            return std::nullopt;
        }
    } else if (water) {
        const auto height = FiniteFloat(object, "water_height", error);
        if (!height) return std::nullopt;
        if (*height < -2048 || *height > 2047.75f) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_WATER_HEIGHT_INVALID", "水面基础高度必须在 -2048..2047.75 世界单位内。");
            return std::nullopt;
        }
        stroke.brush.waterHeight = *height;
    } else if (!newWater && !cliffBrush && !removeWater) {
        auto strength = FiniteFloat(object, "strength", error);
        if (!strength) return std::nullopt;
        const bool smooth = stroke.brush.kind == MapData::TerrainBrushKind::Smooth;
        if (*strength <= 0 || *strength > (smooth ? 1.0f : 256.0f)) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_STRENGTH_INVALID",
                smooth ? "平滑强度必须在 (0,1]。" : "升高/降低强度必须在 (0,256] 世界单位；通行性门禁仍会进一步限制实际高度差。");
            return std::nullopt;
        }
        stroke.brush.strength = *strength;
    }
    const auto points = object.value("points");
    if (!points.isArray() || points.toArray().isEmpty() || points.toArray().size() > 4096) {
        error = Failure("MCP_MAP_EDIT_TERRAIN_POINTS_INVALID", "每笔必须包含 1..4096 个点。");
        return std::nullopt;
    }
    if (nativeRamp && points.toArray().size() != 1) {
        error = Failure("MCP_MAP_EDIT_TERRAIN_POINTS_INVALID", "实际规则坡道每笔必须恰好一个规范世界坐标点。");
        return std::nullopt;
    }
    for (const auto& value : points.toArray()) {
        if (!value.isObject()) {
            error = Failure("MCP_MAP_EDIT_TERRAIN_POINT_INVALID", "地形笔划点必须是 x/y 对象。");
            return std::nullopt;
        }
        const auto point = value.toObject();
        if (!OnlyFields(point, {"x", "y"}, error)) return std::nullopt;
        auto x = BoundedPointCoordinate(point, "x", vertexSpace, error);
        auto y = BoundedPointCoordinate(point, "y", vertexSpace, error);
        if (!x || !y) return std::nullopt;
        stroke.points.push_back({*x, *y});
    }
    return stroke;
}

// 新值可为字符串（精确原值或自然值）或 JSON 数字；数字按最短往返文本交给同一字段解析器。
std::optional<std::string> ValueText(const QJsonValue& value) {
    if (value.isString()) {
        const auto bytes = value.toString().toUtf8();
        if (bytes.size() > 8192 || bytes.contains('\0')) return std::nullopt;
        return bytes.toStdString();
    }
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return std::nullopt;
    std::array<char, 64> buffer{};
    const auto [end, failure] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.toDouble());
    if (failure != std::errc{}) return std::nullopt;
    return std::string(buffer.data(), end);
}

std::optional<Services::MapEditFieldOperation> ParseFieldEdit(const QJsonObject& object, QJsonObject& error) {
    if (!OnlyFields(object, {"kind", "row", "row_key", "expected_member_sha256", "field", "expected", "value"},
            error)) return std::nullopt;
    const auto sha = RequiredSha(object, "expected_member_sha256", error);
    if (!sha) return std::nullopt;
    const auto kindValue = object.value("kind"), fieldValue = object.value("field");
    const auto kind = kindValue.isString()
        ? MapData::ParseMapDataKindId(kindValue.toString().toStdString()) : std::nullopt;
    const auto* field = kind && fieldValue.isString()
        ? MapData::FindMapField(*kind, fieldValue.toString().toStdString()) : nullptr;
    if (!field) {
        error = Failure("MCP_MAP_EDIT_FIELD_UNKNOWN", "kind/field 必须是 describe_map_data 字段目录中的条目。");
        return std::nullopt;
    }
    auto row = RowIndex(object, error);
    auto rowKey = RequiredText(object, "row_key", 16 * 1024, error);
    auto expectedText = RequiredText(object, "expected", 8200, error);
    if (!row || !rowKey || !expectedText) return std::nullopt;
    const auto valueText = ValueText(object.value("value"));
    if (!valueText) {
        error = Failure("MCP_MAP_EDIT_VALUE_INVALID", "value 必须是字符串或有限数字。");
        return std::nullopt;
    }
    auto expected = MapData::ParseMapFieldRaw(field->type, *expectedText);
    if (!expected.Succeeded() || !expected.value) {
        error = Failure(expected);
        return std::nullopt;
    }
    auto value = MapData::ParseMapFieldInput(*field, *valueText);
    if (!value.Succeeded() || !value.value) {
        error = Failure(value);
        return std::nullopt;
    }
    Services::MapEditFieldOperation operation;
    operation.expectedMemberSha256 = *sha;
    operation.edit.kind = *kind;
    operation.edit.row = *row;
    operation.edit.expectedRowKey = std::move(*rowKey);
    operation.edit.field = std::string(field->id);
    operation.edit.expected = std::move(*expected.value);
    operation.edit.value = std::move(*value.value);
    return operation;
}

QJsonObject ShaSchema() { return {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}; }
QJsonObject IndexSchema() { return {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}}; }
QJsonObject FloatSchema() { return {{"type", "number"}}; }
QJsonObject FieldEditSchema() {
    QJsonArray kinds;
    for (const auto kind : {MapDataKind::Settings, MapDataKind::Units, MapDataKind::Regions,
            MapDataKind::Cameras, MapDataKind::Sounds}) kinds.append(QString::fromLatin1(MapData::MapDataKindId(kind)));
    return BasicSchema({{"kind", QJsonObject{{"type", "string"}, {"enum", kinds}}},
        {"row", IndexSchema()},
        {"row_key", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 16384}}},
        {"expected_member_sha256", ShaSchema()},
        {"field", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
        {"expected", QJsonObject{{"type", "string"}, {"maxLength", 8200}}},
        {"value", QJsonObject{{"type", QJsonArray{"string", "number"}}}}},
        {"kind", "row", "row_key", "expected_member_sha256", "field", "expected", "value"});
}

std::optional<Trigger::TriggerTextExpectation> ParseTextExpectation(const QJsonObject& edit, QJsonObject& error) {
    if (!edit.value("expected_text").isObject()) {
        error = Failure("MCP_TRIGGER_EDIT_TEXT_INVALID", "expected_text 须包含原编码与两种文本 SHA。"); return std::nullopt;
    }
    const auto object = edit.value("expected_text").toObject();
    if (!OnlyFields(object, {"encoding", "utf8_sha256", "encoded_sha256"}, error)) return std::nullopt;
    const auto encodingName = RequiredText(object, "encoding", 32, error);
    const auto utf8 = RequiredSha(object, "utf8_sha256", error), encoded = RequiredSha(object, "encoded_sha256", error);
    if (!encodingName || !utf8 || !encoded) return std::nullopt;
    const auto encoding = Trigger::ParseTriggerEditEncoding(*encodingName);
    if (!encoding) { error = Failure("MCP_TRIGGER_EDIT_ENCODING_INVALID", "encoding 须为查询返回的原编码名称。"); return std::nullopt; }
    return Trigger::TriggerTextExpectation{*encoding, *utf8, *encoded};
}

std::optional<Trigger::TriggerEditTriggerTarget> ParseGuiTarget(const QJsonObject& object, QJsonObject& error) {
    const auto trigger = ParseUnsignedId(object, "trigger_id");
    const auto sha = RequiredSha(object, "expected_source_span_sha256", error);
    if (!trigger.Succeeded()) { error = Failure(trigger); return std::nullopt; }
    if (!sha) return std::nullopt;
    const auto roots = object.value("expected_root_ids");
    if (!roots.isArray() || roots.toArray().size() > 4096) {
        error = Failure("MCP_TRIGGER_EDIT_ROOTS_INVALID", "expected_root_ids 须包含完整有序根 ID，最多 4096 项。"); return std::nullopt;
    }
    Trigger::TriggerEditTriggerTarget target{*trigger.value, {}, *sha};
    for (const auto& value : roots.toArray()) {
        const auto id = ParseUnsignedId(QJsonObject{{"id", value}}, "id");
        if (!id.Succeeded()) { error = Failure(id); return std::nullopt; }
        target.expectedRootIds.push_back(*id.value);
    }
    return target;
}

std::optional<Trigger::TriggerEditBatch> ParseTriggerEdits(const QJsonObject& object, QJsonObject& error) {
    if (!OnlyFields(object, {"expected_wtg_sha256", "expected_wct_sha256", "expected_catalog_sha256", "operations"}, error))
        return std::nullopt;
    const auto wtg = RequiredSha(object, "expected_wtg_sha256", error);
    const auto wct = RequiredSha(object, "expected_wct_sha256", error);
    const auto catalog = RequiredSha(object, "expected_catalog_sha256", error);
    if (!wtg || !wct || !catalog) return std::nullopt;
    const auto operations = object.value("operations");
    if (!operations.isArray() || operations.toArray().isEmpty()
        || operations.toArray().size() > static_cast<qsizetype>(Trigger::kMaxTriggerEditOperations)) {
        error = Failure("MCP_TRIGGER_EDITS_INVALID", "触发器批次须包含 1..256 个有界操作。");
        return std::nullopt;
    }
    Trigger::TriggerEditBatch batch{*wtg, *wct, *catalog, {}};
    for (const auto& value : operations.toArray()) {
        if (!value.isObject()) { error = Failure("MCP_TRIGGER_EDITS_INVALID", "每条触发器操作须为对象。"); return std::nullopt; }
        const auto edit = value.toObject();
        const auto op = RequiredText(edit, "op", 64, error);
        if (!op) return std::nullopt;
        if (*op == "set_argument") {
            if (!OnlyFields(edit, {"op", "trigger_id", "owner_eca_id", "argument_id", "expected_value_kind",
                    "expected_value", "expected_type", "value_kind", "value"}, error)) return std::nullopt;
            const auto trigger = ParseUnsignedId(edit, "trigger_id"), owner = ParseUnsignedId(edit, "owner_eca_id"),
                argument = ParseUnsignedId(edit, "argument_id");
            if (!trigger.Succeeded()) { error = Failure(trigger); return std::nullopt; }
            if (!owner.Succeeded()) { error = Failure(owner); return std::nullopt; }
            if (!argument.Succeeded()) { error = Failure(argument); return std::nullopt; }
            const auto beforeKind = Integer(edit, "expected_value_kind", error), afterKind = Integer(edit, "value_kind", error);
            const auto before = RequiredText(edit, "expected_value", 1024 * 1024, error),
                after = RequiredText(edit, "value", 1024 * 1024, error), type = RequiredText(edit, "expected_type", 1024, error);
            if (!beforeKind || !afterKind || !before || !after || !type) return std::nullopt;
            batch.operations.emplace_back(Trigger::TriggerSetArgumentEdit{*trigger.value, *owner.value, *argument.value,
                *beforeKind, *before, *type, *afterKind, *after});
            continue;
        }
        if (*op == "append_root" || *op == "convert_custom_text") {
            const bool append = *op == "append_root";
            if (!OnlyFields(edit, append ? std::initializer_list<std::string_view>{"op", "trigger_id", "expected_root_ids",
                    "expected_source_span_sha256", "kind", "function"}
                : std::initializer_list<std::string_view>{"op", "trigger_id", "expected_root_ids",
                    "expected_source_span_sha256", "expected_text", "text"}, error)) return std::nullopt;
            auto target = ParseGuiTarget(edit, error);
            if (!target) return std::nullopt;
            if (append) {
                const auto kind = ParseRootKind(edit);
                const auto function = RequiredText(edit, "function", 1024, error);
                if (!kind.Succeeded()) { error = Failure(kind); return std::nullopt; }
                if (!function) return std::nullopt;
                batch.operations.emplace_back(Trigger::TriggerAppendRootEdit{std::move(*target), *kind.value, *function});
            } else {
                const auto expected = ParseTextExpectation(edit, error);
                const auto text = RequiredText(edit, "text", 1024 * 1024, error);
                if (!expected || !text) return std::nullopt;
                batch.operations.emplace_back(Trigger::TriggerConvertCustomTextEdit{std::move(*target), *expected, *text});
            }
            continue;
        }
        if (*op == "set_global_code" || *op == "set_custom_text") {
            const bool global = *op == "set_global_code";
            if (!OnlyFields(edit, global ? std::initializer_list<std::string_view>{"op", "expected_text", "text"}
                : std::initializer_list<std::string_view>{"op", "trigger_id", "expected_text", "text"}, error)) return std::nullopt;
            const auto expected = ParseTextExpectation(edit, error);
            const auto text = RequiredText(edit, "text", 1024 * 1024, error);
            if (!expected || !text) return std::nullopt;
            if (global) batch.operations.emplace_back(Trigger::TriggerSetGlobalCodeEdit{*expected, *text});
            else {
                const auto trigger = ParseUnsignedId(edit, "trigger_id");
                if (!trigger.Succeeded()) { error = Failure(trigger); return std::nullopt; }
                batch.operations.emplace_back(Trigger::TriggerSetCustomTextEdit{*trigger.value, *expected, *text});
            }
            continue;
        }
        const bool enabledEdit = *op == "set_eca_enabled";
        if (*op != "remove_eca" && !enabledEdit) {
            error = Failure("MCP_TRIGGER_EDIT_UNSUPPORTED", "未知触发器操作；只支持公开 schema 中的七种类型化操作。");
            return std::nullopt;
        }
        if (!OnlyFields(edit, {"op", "trigger_id", "eca_id", "parent_eca_id", "expected_kind",
            "expected_function", "expected_enabled", "expected_child_id", "expected_argument_count",
            "expected_child_count", "expected_source_span_sha256", "enabled"}, error)) return std::nullopt;
        if (!enabledEdit && edit.contains("enabled")) {
            error = Failure("MCP_TRIGGER_EDIT_UNSUPPORTED", "删除操作不接受 enabled 新值。"); return std::nullopt;
        }
        const auto triggerId = ParseUnsignedId(edit, "trigger_id"), ecaId = ParseUnsignedId(edit, "eca_id");
        const auto parentId = ParseUnsignedId(edit, "parent_eca_id", true);
        if (!triggerId.Succeeded()) { error = Failure(triggerId); return std::nullopt; }
        if (!ecaId.Succeeded()) { error = Failure(ecaId); return std::nullopt; }
        if (!parentId.Succeeded()) { error = Failure(parentId); return std::nullopt; }
        const auto kind = Integer(edit, "expected_kind", error), enabled = Integer(edit, "expected_enabled", error);
        const auto arguments = Integer(edit, "expected_argument_count", error), children = Integer(edit, "expected_child_count", error);
        const auto function = RequiredText(edit, "expected_function", 1024, error);
        const auto spanSha = RequiredSha(edit, "expected_source_span_sha256", error);
        if (!kind || !enabled || !arguments || !children || !function || !spanSha)
            return std::nullopt;
        if (*arguments < 0 || *children < 0) {
            error = Failure("MCP_TRIGGER_EDIT_COUNT_INVALID", "参数与子节点计数不能为负。"); return std::nullopt;
        }
        Trigger::TriggerEditEcaTarget target{*triggerId.value, *ecaId.value, *parentId.value, *kind,
            *function, *enabled, {}, static_cast<std::size_t>(*arguments), static_cast<std::size_t>(*children), *spanSha};
        if (!edit.contains("expected_child_id")) {
            error = Failure("MCP_TRIGGER_EDIT_CHILD_INVALID", "expected_child_id 须明确提供整数或 null。"); return std::nullopt;
        }
        if (!edit.value("expected_child_id").isNull()) {
            const auto child = Integer(edit, "expected_child_id", error);
            if (!child) return std::nullopt;
            target.expectedChildId = *child;
        }
        if (enabledEdit) {
            if (!edit.value("enabled").isBool()) { error = Failure("MCP_TRIGGER_EDIT_ENABLED_INVALID", "enabled 须为布尔值。"); return std::nullopt; }
            batch.operations.emplace_back(Trigger::TriggerSetEcaEnabledEdit{std::move(target), edit.value("enabled").toBool()});
        } else batch.operations.emplace_back(Trigger::TriggerRemoveEcaEdit{std::move(target)});
    }
    const auto validated = Trigger::ValidateTriggerEditBatch(batch);
    if (!validated.Succeeded()) { error = Failure(validated); return std::nullopt; }
    return batch;
}

QJsonObject TriggerEditsSchema() {
    const QJsonObject id{{"type", "string"}, {"pattern", "^[0-9]+$"}, {"maxLength", 20}};
    const QJsonObject count{{"type", "integer"}, {"minimum", 0}, {"maximum", 250000}};
    const QJsonObject ecaProperties{
        {"trigger_id", id}, {"eca_id", id}, {"parent_eca_id", id},
        {"expected_kind", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 2}}},
        {"expected_function", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
        {"expected_enabled", QJsonObject{{"type", "integer"}, {"enum", QJsonArray{0, 1}}}},
        {"expected_child_id", QJsonObject{{"type", QJsonArray{"integer", "null"}}}},
        {"expected_argument_count", count}, {"expected_child_count", count},
        {"expected_source_span_sha256", ShaSchema()}};
    const QJsonArray ecaRequired{"op", "trigger_id", "eca_id", "parent_eca_id", "expected_kind", "expected_function", "expected_enabled",
        "expected_child_id", "expected_argument_count", "expected_child_count", "expected_source_span_sha256"};
    const auto eca = [&](bool enabled) {
        auto properties = ecaProperties;
        auto required = ecaRequired;
        properties["op"] = QJsonObject{{"type", "string"}, {"const", enabled ? "set_eca_enabled" : "remove_eca"}};
        if (enabled) { properties["enabled"] = QJsonObject{{"type", "boolean"}}; required.append("enabled"); }
        return BasicSchema(properties, required);
    };
    const QJsonObject textExpectation = BasicSchema({
        {"encoding", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"utf8", "utf8-bom", "gb18030", "latin1"}}}},
        {"utf8_sha256", ShaSchema()}, {"encoded_sha256", ShaSchema()}}, {"encoding", "utf8_sha256", "encoded_sha256"});
    const QJsonObject text{{"type", "string"}, {"maxLength", 1024 * 1024}};
    const QJsonObject valueKind{{"type", "integer"}, {"enum", QJsonArray{0, 1, 3}}};
    const QJsonObject rootIds{{"type", "array"}, {"maxItems", 4096}, {"items", id}};
    const QJsonObject scalar = BasicSchema({{"op", QJsonObject{{"type", "string"}, {"const", "set_argument"}}},
        {"trigger_id", id}, {"owner_eca_id", id}, {"argument_id", id}, {"expected_value_kind", valueKind},
        {"expected_value", text}, {"expected_type", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}},
        {"value_kind", valueKind}, {"value", text}},
        {"op", "trigger_id", "owner_eca_id", "argument_id", "expected_value_kind", "expected_value", "expected_type", "value_kind", "value"});
    const QJsonObject append = BasicSchema({{"op", QJsonObject{{"type", "string"}, {"const", "append_root"}}},
        {"trigger_id", id}, {"expected_root_ids", rootIds}, {"expected_source_span_sha256", ShaSchema()},
        {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"event", "condition", "action"}}}},
        {"function", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}},
        {"op", "trigger_id", "expected_root_ids", "expected_source_span_sha256", "kind", "function"});
    const QJsonObject global = BasicSchema({{"op", QJsonObject{{"type", "string"}, {"const", "set_global_code"}}},
        {"expected_text", textExpectation}, {"text", text}}, {"op", "expected_text", "text"});
    const QJsonObject custom = BasicSchema({{"op", QJsonObject{{"type", "string"}, {"const", "set_custom_text"}}},
        {"trigger_id", id}, {"expected_text", textExpectation}, {"text", text}}, {"op", "trigger_id", "expected_text", "text"});
    const QJsonObject convert = BasicSchema({{"op", QJsonObject{{"type", "string"}, {"const", "convert_custom_text"}}},
        {"trigger_id", id}, {"expected_root_ids", rootIds}, {"expected_source_span_sha256", ShaSchema()},
        {"expected_text", textExpectation}, {"text", text}},
        {"op", "trigger_id", "expected_root_ids", "expected_source_span_sha256", "expected_text", "text"});
    const QJsonObject operation{{"oneOf", QJsonArray{eca(false), eca(true), scalar, append, global, custom, convert}}};
    return BasicSchema({{"expected_wtg_sha256", ShaSchema()}, {"expected_wct_sha256", ShaSchema()},
        {"expected_catalog_sha256", ShaSchema()}, {"operations", QJsonObject{{"type", "array"}, {"minItems", 1},
            {"maxItems", 256}, {"items", operation}}}},
        {"expected_wtg_sha256", "expected_wct_sha256", "expected_catalog_sha256", "operations"});
}

QJsonObject TerrainStrokeSchema() {
    const QJsonObject cell{{"type", "integer"}, {"minimum", 0}, {"maximum", 2048}};
    const QJsonObject signedCell{{"type", "integer"}, {"minimum", -2147483648.0}, {"maximum", 2147483647.0}};
    auto resize = BasicSchema({{"kind", QJsonObject{{"type", "string"}, {"const", "canvas_resize"}}},
        {"west", signedCell}, {"south", signedCell}, {"east", signedCell}, {"north", signedCell}},
        {"kind", "west", "south", "east", "north"});
    resize["description"] = "改变地图外矩形；四边相对本笔之前地图西南角，单位地格，东/北不包含。保留原可玩区和所有世界位置；裁及内容、来源不全或规则不支持时整笔拒绝，不接受移动/删除对象或客户端规则。";
    const auto bounds = BasicSchema({{"kind", QJsonObject{{"type", "string"}, {"const", "playable_bounds"}}},
        {"west", cell}, {"south", cell}, {"east", cell}, {"north", cell}}, {"kind", "west", "south", "east", "north"});
    const QJsonObject pointSchema = BasicSchema({{"x", FloatSchema()}, {"y", FloatSchema()}},
        {"x", "y"});
    const QJsonObject pointsSchema{{"type", "array"}, {"minItems", 1}, {"maxItems", 4096},
        {"items", pointSchema}};
    const QJsonObject shapeSchema{{"type", "string"}, {"enum", QJsonArray{"circle", "square"}}};
    const QJsonObject spaceSchema{{"type", "string"}, {"enum", QJsonArray{"world", "terrain_vertex"}}};
    const QJsonObject radiusSchema{{"type", "integer"}, {"minimum", 0}, {"maximum", 16}};
    const QJsonObject noFollow{{"type", "boolean"}, {"const", false}};
    const QJsonObject shared{{"shape", shapeSchema}, {"point_space", spaceSchema},
        {"radius", radiusSchema}, {"points", pointsSchema}, {"follow_doodads", noFollow}};
    QJsonObject height = shared;
    height["kind"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"raise", "lower"}}};
    height["strength"] = QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 256}};
    QJsonObject smooth = shared;
    smooth["kind"] = QJsonObject{{"type", "string"}, {"const", "smooth"}};
    smooth["strength"] = QJsonObject{{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}};
    QJsonObject blight = shared;
    blight["kind"] = QJsonObject{{"type", "string"}, {"const", "blight"}};
    blight["set_blight"] = QJsonObject{{"type", "boolean"}};
    QJsonObject boundary = shared;
    boundary["kind"] = QJsonObject{{"type", "string"}, {"const", "boundary"}};
    boundary["value"] = QJsonObject{{"type", "boolean"}};
    QJsonObject pathing = shared;
    pathing.remove("radius");
    pathing["kind"] = QJsonObject{{"type", "string"}, {"const", "pathing_restriction"}};
    pathing["size"] = QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 8}};
    pathing["blocked_type"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"walk", "fly", "build"}}};
    pathing["value"] = QJsonObject{{"type", "boolean"}};
    auto pathingSchema = BasicSchema(pathing,
        {"kind", "shape", "point_space", "size", "points", "follow_doodads", "blocked_type", "value"});
    pathingSchema["description"] = "按32单位通行格绘制walk/fly/build限制；size=1..8与实时工具同语义。value=false仅撤回本次保存序列新增的限制，源图限制保留；不接受原始位、radius或客户端规则。";
    QJsonObject ramp = shared;
    ramp["kind"] = QJsonObject{{"type", "string"}, {"const", "ramp"}};
    ramp["shape"] = QJsonObject{{"type", "string"}, {"const", "circle"}};
    ramp["radius"] = QJsonObject{{"type", "integer"}, {"const", 0}};
    ramp["set_ramp"] = QJsonObject{{"type", "boolean"}};
    auto cornerPoints = pointsSchema; cornerPoints["maxItems"] = 1;
    const auto corner = BasicSchema({{"kind", QJsonObject{{"type", "string"}, {"const", "corner_ramp"}}},
        {"point_space", QJsonObject{{"type", "string"}, {"const", "world"}}}, {"points", cornerPoints},
        {"follow_doodads", noFollow}, {"set_ramp", QJsonObject{{"type", "boolean"}}}},
        {"kind", "point_space", "points", "follow_doodads", "set_ramp"});
    auto straight = corner;
    auto straightProperties = straight.value("properties").toObject();
    straightProperties["kind"] = QJsonObject{{"type", "string"}, {"const", "straight_ramp"}};
    straight["properties"] = straightProperties;
    straight["description"] = "实际规则一级直坡，可含水并连接相邻完整直坡；一个规范世界点，固定检查水位限制，不接受旧ramp的形状/半径参数或客户端算法版本。";
    QJsonObject texture = shared;
    texture["kind"] = QJsonObject{{"type", "string"}, {"const", "texture"}};
    texture["ground_texture"] = QJsonObject{{"type", "string"}, {"minLength", 4}, {"maxLength", 4}, {"pattern", "^[!-~]{4}$"}};
    QJsonObject water = shared;
    water["kind"] = QJsonObject{{"type", "string"}, {"const", "water_height"}};
    water["water_height"] = QJsonObject{{"type", "number"}, {"minimum", -2048}, {"maximum", 2047.75}};
    QJsonObject removeWater = shared;
    removeWater["kind"] = QJsonObject{{"type", "string"}, {"const", "remove_water"}};
    QJsonObject newWater = shared;
    newWater.remove("radius");
    newWater["kind"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"shallow_water", "deep_water"}}};
    newWater["size"] = QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 8}};
    newWater["cliff_texture"] = QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 14}};
    newWater["seed"] = QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}};
    newWater["enforce_water_limits"] = QJsonObject{{"type", "boolean"}};
    QJsonObject cliff = shared;
    cliff.remove("radius");
    cliff["size"] = QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 8}};
    cliff["cliff_texture"] = QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 14}};
    QJsonObject cliffTexture = cliff;
    cliffTexture["kind"] = QJsonObject{{"type", "string"}, {"const", "cliff_texture"}};
    cliff["kind"] = QJsonObject{{"type", "string"}, {"const", "cliff_level"}};
    cliff["level_delta"] = QJsonObject{{"type", "integer"}, {"minimum", -1}, {"maximum", 1}};
    cliff["seed"] = QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}};
    const QJsonArray required{"kind", "shape", "point_space", "radius", "points", "follow_doodads"};
    QJsonArray heightRequired = required, blightRequired = required, boundaryRequired = required, rampRequired = required,
        textureRequired = required, waterRequired = required;
    heightRequired.append("strength");
    blightRequired.append("set_blight");
    boundaryRequired.append("value");
    rampRequired.append("set_ramp");
    textureRequired.append("ground_texture"); waterRequired.append("water_height");
    auto removalSchema = BasicSchema(removeWater, required);
    removalSchema["description"] = "移除水面且保留地形高度、原始水高及所有摆放；不接受高度、种子或悬崖参数，保存仍须核对原水规则并重建通行性。";
    QJsonObject blockCopyProperties{{"kind", QJsonObject{{"type", "string"}, {"const", "block_copy"}}}};
    for (const char* index : {"source_x", "source_y", "width", "height", "destination_x", "destination_y"}) {
        const bool extent = std::string_view(index) == "width" || std::string_view(index) == "height";
        blockCopyProperties.insert(index, QJsonObject{{"type", "integer"}, {"minimum", extent ? 1 : 0},
            {"maximum", extent ? static_cast<int>(MapData::TerrainBlockMaximumSide) : 2048}});
    }
    auto blockCopy = BasicSchema(blockCopyProperties, {"kind", "source_x", "source_y", "width", "height", "destination_x", "destination_y"});
    blockCopy["description"] = "同图绝对原始值复制，包括东/北顶点，不旋转缩放；必须是唯一地形命令，源摘要由服务捕获。拒绝坡道、边界、MapEdge和特殊装饰物，悬崖与水沿实际规则。";
    QJsonArray variants{BasicSchema(height, heightRequired),
        BasicSchema(smooth, heightRequired), BasicSchema(blight, blightRequired), BasicSchema(boundary, boundaryRequired),
        BasicSchema(ramp, rampRequired), corner, straight, bounds, resize, blockCopy, pathingSchema,
        BasicSchema(texture, textureRequired), BasicSchema(water, waterRequired), removalSchema,
        BasicSchema(cliff, {"kind", "shape", "point_space", "size", "cliff_texture", "level_delta", "seed", "points", "follow_doodads"}),
        BasicSchema(cliffTexture, {"kind", "shape", "point_space", "size", "cliff_texture", "points", "follow_doodads"}),
        BasicSchema(newWater, {"kind", "shape", "point_space", "size", "cliff_texture", "seed",
            "enforce_water_limits", "points", "follow_doodads"})};
    for (const auto& schema : TerrainTextureSlotCommandSchemas()) variants.append(schema);
    return {{"oneOf", variants}};
}

bool ReadEffectiveLayers(const QJsonObject& parameters, std::optional<std::vector<std::string>>& layers, QJsonObject& error) {
    if (!parameters.contains("effective_layers")) return true;
    const auto supplied = parameters.value("effective_layers");
    if (!supplied.isArray() || supplied.toArray().isEmpty() || supplied.toArray().size() > 128) {
        error = Failure("MCP_UI_LAYERS_INVALID", "有效函数库层序须为 1..128 个名称。"); return false;
    }
    layers.emplace();
    for (const auto& value : supplied.toArray()) {
        const auto layer = value.toString().toUtf8();
        if (!value.isString() || layer.isEmpty() || layer.size() > 256 || layer.contains('\0')) {
            error = Failure("MCP_UI_LAYERS_INVALID", "函数库层名称为空、过长、含 NUL 或不是字符串。"); return false;
        }
        layers->push_back(layer.toStdString());
    }
    return true;
}

void RegisterTriggerTextQueryTool(McpServer& server) {
    ToolDefinition tool;
    tool.name = "query_map_trigger_text";
    tool.description = "按整图 SHA 从独立 MPQ 源图读取 global 或 trigger 的 WCT 文本；UTF-8 字节分页最多 65536。"
        "同时返回原编码、UTF-8 文本/原编码字节 SHA、WTG/WCT/Catalog SHA，与 save_map_edit_candidate 共享来源解析。"
        "普通 GUI 槽另给完整原树绑定，供显式 convert_custom_text/append_root；不隐式转 GUI、不读取旧目录事务 overlay。";
    tool.maxResponseBytes = kMaxResponseBytes;
    tool.inputSchema = BasicSchema({{"source_map_path", QJsonObject{{"type", "string"}}},
        {"expected_source_sha256", ShaSchema()}, {"expected_catalog_sha256", ShaSchema()},
        {"ui_root", QJsonObject{{"type", "string"}, {"minLength", 1}}},
        {"effective_layers", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 128},
            {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}},
        {"slot", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"global", "trigger"}}}},
        {"trigger_id", IndexSchema()}, {"offset", IndexSchema()},
        {"limit", QJsonObject{{"type", "integer"}, {"minimum", 4}, {"maximum", 65536}}}},
        {"source_map_path", "expected_source_sha256", "slot", "trigger_id"});
    tool.handler = [](const QJsonObject& parameters) -> QJsonValue {
        QJsonObject error;
        if (!OnlyFields(parameters, {"source_map_path", "expected_source_sha256", "expected_catalog_sha256", "ui_root",
                "effective_layers", "slot", "trigger_id", "offset", "limit"}, error)) return error;
        const auto path = ParseAbsolutePath(parameters, "source_map_path");
        if (!path.Succeeded()) return Failure(path);
        const auto sha = RequiredSha(parameters, "expected_source_sha256", error);
        if (!sha) return error;
        const auto toolRoot = HostConfiguredSaveRoot("FORGE_MCP_YDWE_ROOT");
        if (!toolRoot.Succeeded()) return Failure(toolRoot);
        Services::MapTriggerTextQueryRequest request;
        request.sourceMap = *path.value; request.toolRoot = *toolRoot.value; request.expectedSourceSha256 = *sha;
        if (parameters.contains("ui_root")) {
            const auto ui = ParseAbsolutePath(parameters, "ui_root");
            if (!ui.Succeeded()) return Failure(ui);
            request.uiRoot = *ui.value;
        }
        if (parameters.contains("expected_catalog_sha256")) {
            const auto catalog = RequiredSha(parameters, "expected_catalog_sha256", error);
            if (!catalog) return error;
            request.expectedCatalogSha256 = *catalog;
        }
        if (!ReadEffectiveLayers(parameters, request.effectiveLayers, error)) return error;
        const auto slot = RequiredText(parameters, "slot", 16, error);
        if (!slot) return error;
        if (*slot == "global") request.query.slot = Trigger::TriggerTextSlot::GlobalCode;
        else if (*slot == "trigger") request.query.slot = Trigger::TriggerTextSlot::TriggerCode;
        else return Failure("MCP_TRIGGER_TEXT_SLOT_INVALID", "slot 须为 global 或 trigger。");
        const auto id = ParseUnsignedId(parameters, "trigger_id", true);
        if (!id.Succeeded()) return Failure(id);
        request.query.triggerId = *id.value;
        if (parameters.contains("offset")) {
            const auto offset = ParseUnsignedId(parameters, "offset", true);
            if (!offset.Succeeded()) return Failure(offset);
            if (*offset.value > std::numeric_limits<std::size_t>::max()) return Failure("MCP_TRIGGER_TEXT_OFFSET_INVALID", "offset 超出本机索引范围。");
            request.query.offset = static_cast<std::size_t>(*offset.value);
        }
        if (parameters.contains("limit")) {
            const auto limit = Integer(parameters, "limit", error);
            if (!limit) return error;
            if (*limit < 4 || *limit > 65536) return Failure("MCP_TRIGGER_TEXT_LIMIT_INVALID", "limit 须在 4..65536 字节内。");
            request.query.limit = static_cast<std::size_t>(*limit);
        }
        const auto queried = Services::QueryMapTriggerText(request);
        if (!queried.Succeeded() || !queried.value) return Failure(queried);
        const auto& receipt = *queried.value;
        const auto& page = receipt.page;
        QJsonArray layers;
        for (const auto& layer : receipt.effectiveLayers) layers.append(QString::fromStdString(layer));
        QJsonObject response{{"ok", true}, {"source_sha256", QString::fromStdString(receipt.sourceSha256)},
            {"wtg_sha256", QString::fromStdString(page.wtgSha256)}, {"wct_sha256", QString::fromStdString(page.wctSha256)},
            {"catalog_sha256", QString::fromStdString(page.catalogSha256)},
            {"slot", page.slot == Trigger::TriggerTextSlot::GlobalCode ? "global" : "trigger"},
            {"trigger_id", QString::fromStdString(std::to_string(page.triggerId))},
            {"encoding", QString::fromStdString(std::string(Trigger::TriggerEditEncodingName(page.encoding)))},
            {"utf8_sha256", QString::fromStdString(page.utf8Sha256)}, {"encoded_sha256", QString::fromStdString(page.encodedSha256)},
            {"text", QString::fromStdString(page.text)}, {"total_bytes", QString::fromStdString(std::to_string(page.totalBytes))},
            {"offset", QString::fromStdString(std::to_string(page.offset))}, {"next_offset", QString::fromStdString(std::to_string(page.nextOffset))},
            {"custom_text", page.customText}, {"effective_layers", layers}, {"gui_target", QJsonValue::Null}};
        if (receipt.guiTarget) {
            QJsonArray roots;
            for (const auto root : receipt.guiTarget->expectedRootIds) roots.append(QString::fromStdString(std::to_string(root)));
            response["gui_target"] = QJsonObject{{"trigger_id", QString::fromStdString(std::to_string(receipt.guiTarget->triggerId))},
                {"expected_root_ids", roots}, {"expected_source_span_sha256", QString::fromStdString(receipt.guiTarget->expectedSourceSpanSha256)}};
        }
        if (!queried.diagnostics.empty()) response["diagnostics"] = DiagnosticsJson(queried.diagnostics).value("diagnostics");
        if (static_cast<std::size_t>(QJsonDocument(response).toJson(QJsonDocument::Compact).size()) > kMaxResponseBytes)
            return Failure("MCP_TRIGGER_TEXT_JSON_BUDGET", "文本查询回执超过输出预算。");
        return response;
    };
    server.RegisterTool(std::move(tool));
}

} // namespace

void RegisterMapEditSaveTool(McpServer& server) {
    ToolDefinition tool;
    tool.name = "save_map_edit_candidate";
    tool.permission = ToolPermission::Save;
    tool.description = "从独立 MPQ 源图按 SHA 绑定，保存字段、地形或 1..256 项通用触发器批次到新候选图。"
        "字段按 describe_map_data 的字段目录寻址（kind/row/field），须回传 query_map_data_page 给出的 row_key、"
        "成员 SHA 与字段 raw 前值；只有 writable 字段可写。地形支持荒芜、升高、降低、平滑、地面贴图、"
        "悬崖层级与纯悬崖材质；cliff_level须显式给出size=1..8、cliff_texture=0..14、level_delta=-1..1和uint32 seed，"
        "水位限制固定启用。cliff_texture只改已有palette槽位，不接受层级、种子、锚点或规则，仍须完整通行重建资格。"
        "已有水位、浅/深水、一级直坡和拐角坡道；旧ramp须circle/radius=0并保持严格干地规则，"
        "straight_ramp可连接相邻完整一级直坡；与corner_ramp只接受单个规范world落点，不自动吸附，水位限制固定启用；"
        "坡道均须明确set_ramp布尔值，几何和实际规则仍由共用规划器核验。"
        "boundary通过显式value布尔值绘制或清除地形边界；playable_bounds通过整数半开地格矩形设置可玩范围。"
        "canvas_resize用相对本笔前地图西南角的有符号整数半开地格矩形扩裁外边，固定保留原可玩区和对象世界位置；"
        "texture_slot_replace/texture_slot_append操作贴图表，槽号从0起，地面最多16槽、悬崖最多15槽；"
        "逐条核对旧rawcode或当前表长；任何命令失败时整批不保存。"
        "裁及内容或来源不完整整笔拒绝，不提供移动或删除选项。"
        "pathing_restriction须给出blocked_type=walk/fly/build、size=1..8与value布尔值，按32单位通行格取足迹；"
        "value=false仅撤回本次序列新增限制，原图限制始终保留，不能用原始通行位绕过。"
        "共用通行性门禁最终裁决。地物跟随和其他笔刷拒绝。工具/游戏根只读可信宿主环境变量；"
        "可选 ui_root 明确指定独立 UI 元数据绝对目录，仍提交到同一保存服务，不推断或回退工具/UI 来源。"
        "携带 transaction_id/expected_revision 时保存已经 preview/validate 的完整 GUI 事务，"
        "源配对与 Catalog 必须一致；成功后目录事务仍活动，不自动绑定候选。不宣称原版或游戏验证。";
    tool.maxResponseBytes = kMaxResponseBytes;
    tool.inputSchema = BasicSchema({
        {"source_map_path", QJsonObject{{"type", "string"}}},
        {"output_path", QJsonObject{{"type", "string"}}},
        {"ui_root", QJsonObject{{"type", "string"}, {"minLength", 1},
            {"description", "可选的独立 UI 元数据绝对目录，必须可访问；工具根仍由可信宿主配置提供。"
                "两者提交到同一保存服务，路径不用于推断或回退其他安装；省略沿用既有工具根提供 UI 的默认行为。"}}},
        {"expected_source_sha256", ShaSchema()},
        {"script_target", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"source", "configured_lua"}},
            {"description", "选择宿主已绑定的完整Lua保存档案；客户端不能指定检查器或更改档案。默认source保持既有保存路径。"}}},
        {"source_encodings", QJsonObject{{"type", "array"}, {"maxItems", 1024},
            {"description", "仅声明既有框架来源单文件编码；不改变工具、版本、UI层或读取资格。"},
            {"items", BasicSchema({{"path", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
                {"encoding", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"utf8", "gbk"}}}}}, {"path", "encoding"})}}},
        {"include_access", QJsonObject{{"type", "array"}, {"maxItems", 128},
            {"description", "显式普通include读取范围，不增加搜索目录、注入根或native权限；auto仍拒绝歧义编码。"},
            {"items", BasicSchema({{"path", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 4096}}},
                {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"directory", "file"}}}},
                {"encoding", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"utf8", "gbk", "auto"}}}},
                {"required", QJsonObject{{"type", "boolean"}, {"default", true}}}}, {"path", "kind", "encoding"})}}},
        {"expected_terrain_sha256", ShaSchema()},
        {"trigger_edits", TriggerEditsSchema()},
        {"effective_layers", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 128},
            {"items", QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}},
        {"field_edits", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 32},
            {"items", FieldEditSchema()}}},
        {"expected_settings_sha256", ShaSchema()},
        {"settings_custom_edits", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", 128}, {"items", MapCustomSettingsOperationSchema()}}},
        {"terrain_strokes", QJsonObject{{"type", "array"}, {"minItems", 1},
            {"maxItems", 128}, {"items", TerrainStrokeSchema()}}},
    }, {"source_map_path", "output_path", "expected_source_sha256"});
    const auto envelopeProperties = BasicSchema({}, {"transaction_id", "expected_revision"}).value("properties").toObject();
    auto saveProperties = tool.inputSchema.value("properties").toObject();
    const auto runtimeProperties = MapRuntimeSaveProperties();
    for (auto it = runtimeProperties.begin(); it != runtimeProperties.end(); ++it) saveProperties.insert(it.key(), it.value());
    saveProperties.insert("transaction_id", envelopeProperties.value("transaction_id"));
    saveProperties.insert("expected_revision", envelopeProperties.value("expected_revision"));
    tool.inputSchema["properties"] = saveProperties;
    tool.inputSchema["anyOf"] = QJsonArray{
        QJsonObject{{"required", QJsonArray{"field_edits"}}},
        QJsonObject{{"required", QJsonArray{"expected_settings_sha256", "settings_custom_edits"}}},
        QJsonObject{{"required", QJsonArray{"trigger_edits"}}},
        QJsonObject{{"required", QJsonArray{"transaction_id", "expected_revision"}}},
        QJsonObject{{"required", QJsonArray{"expected_terrain_sha256", "terrain_strokes"}}},
        QJsonObject{{"required", QJsonArray{"runtime"}}}};
    tool.handler = [&server](const QJsonObject& parameters) -> QJsonValue {
        QJsonObject error;
        if (!OnlyFields(parameters, {"source_map_path", "output_path", "expected_source_sha256",
            "ui_root", "source_encodings", "include_access", "script_target", "effective_layers", "field_edits", "trigger_edits", "expected_terrain_sha256", "terrain_strokes", "transaction_id", "expected_revision", "runtime", "runtime_purpose", "expected_settings_sha256", "settings_custom_edits"}, error)) return error;
        const bool workspaceRequest = parameters.contains("transaction_id") || parameters.contains("expected_revision");
        std::optional<std::pair<TransactionId, ProjectRevision>> envelope;
        McpWorkspace* workspace = nullptr;
        if (workspaceRequest) {
            workspace = RequireWorkspace(server, error);
            if (!workspace) return error;
            const auto parsed = ParseEnvelope(parameters);
            if (!parsed.Succeeded()) return Failure(parsed);
            envelope = *parsed.value;
        }
        if (!workspaceRequest && server.Workspace() && server.Workspace()->HasActiveTransaction()) {
            return Failure("MCP_MAP_EDIT_TRANSACTION_ACTIVE",
                "活动触发器事务尚未结束；独立地图候选保存不会包含该 overlay。");
        }
        auto source = ParseAbsolutePath(parameters, "source_map_path");
        auto output = ParseAbsolutePath(parameters, "output_path");
        if (!source.Succeeded()) return Failure(source);
        if (!output.Succeeded()) return Failure(output);
        const auto sourceSha = RequiredSha(parameters, "expected_source_sha256", error);
        if (!sourceSha) return error;
        const bool fieldRequest = parameters.contains("field_edits");
        const bool customRequest = parameters.contains("settings_custom_edits") || parameters.contains("expected_settings_sha256");
        const bool terrainRequest = parameters.contains("terrain_strokes") ||
            parameters.contains("expected_terrain_sha256");
        const bool triggerRequest = parameters.contains("trigger_edits");
        const int domainCount = static_cast<int>(fieldRequest) + static_cast<int>(terrainRequest) + static_cast<int>(triggerRequest)
                + static_cast<int>(workspaceRequest) + static_cast<int>(customRequest);
        if (domainCount != 1 && !(domainCount == 0 && parameters.contains("runtime")))
            return Failure("MCP_MAP_EDIT_OPERATION_SCOPE_INVALID", "一次请求只能提供字段、地形或触发器其中一种领域批次。");
        std::error_code pathError;
        const auto targetStatus = std::filesystem::symlink_status(*output.value, pathError);
        if (!pathError && targetStatus.type() != std::filesystem::file_type::not_found)
            return Failure("MCP_OUTPUT_EXISTS", "候选输出路径已存在；必须使用新文件。");
        if (pathError && pathError != std::errc::no_such_file_or_directory)
            return Failure("MCP_OUTPUT_PATH_INVALID", "无法确认候选输出路径是否已存在。");
        auto toolRoot = HostConfiguredSaveRoot("FORGE_MCP_YDWE_ROOT");
        auto gameRoot = HostConfiguredSaveRoot("FORGE_MCP_WAR3_ROOT");
        if (!toolRoot.Succeeded()) return Failure(toolRoot);
        if (!gameRoot.Succeeded()) return Failure(gameRoot);
        Services::MapEditSaveRequest request;
        request.sourceMap = *source.value;
        request.targetCandidate = *output.value;
        request.toolRoot = *toolRoot.value;
        request.gameRoot = *gameRoot.value;
        request.expectedSourceSha256 = *sourceSha;
        const auto runtime = DecodeMapRuntimeSaveOptions(parameters);
        if (!runtime.Succeeded()) return Failure(runtime);
        request.runtime = *runtime.value;
        const auto runtimeSources = Services::LoadInstalledMapRuntimeSources(CurrentInstallationRoot());
        if (runtimeSources.Succeeded()) request.runtimePackageSources = *runtimeSources.value;
        else request.runtimeCatalogDiagnostics = runtimeSources.diagnostics;
        if (parameters.contains("script_target")) {
            const auto target = parameters.value("script_target");
            if (!target.isString() || (target.toString() != "source" && target.toString() != "configured_lua"))
                return Failure("MCP_SCRIPT_TARGET_INVALID", "script_target仅支持source或configured_lua。");
            if (target.toString() == "configured_lua") {
                if (!server.LuaMapTarget()) return Failure("MCP_LUA_TARGET_UNCONFIGURED", "宿主尚未配置完整Lua保存档案。");
                request.luaTarget = *server.LuaMapTarget();
            }
        }
        if (parameters.contains("source_encodings")) {
            const auto declarations = parameters.value("source_encodings");
            if (!declarations.isArray() || declarations.toArray().size() > 1024)
                return Failure("MCP_SOURCE_ENCODINGS_INVALID", "source_encodings 须为最多1024项的逐文件声明数组。");
            for (const auto& value : declarations.toArray()) {
                if (!value.isObject()) return Failure("MCP_SOURCE_ENCODINGS_INVALID", "逐文件声明须为对象。");
                const auto object = value.toObject();
                if (!OnlyFields(object, {"path", "encoding"}, error)) return error;
                auto path = ParseAbsolutePath(object, "path");
                const auto encoding = RequiredText(object, "encoding", 4, error);
                if (!path.Succeeded()) return Failure(path);
                if (!encoding) return error;
                auto declaration = Services::ParseScriptSourceEncodingDeclaration(*path.value, *encoding);
                if (!declaration.Succeeded() || !declaration.value) return Failure(declaration);
                request.sourceEncodings.push_back(std::move(*declaration.value));
            }
        }
        if (parameters.contains("include_access")) {
            const auto declarations = parameters.value("include_access");
            if (!declarations.isArray() || declarations.toArray().size() > 128)
                return Failure("MCP_INCLUDE_ACCESS_INVALID", "include_access 须为最多128项的显式范围数组。");
            for (const auto& value : declarations.toArray()) {
                if (!value.isObject()) return Failure("MCP_INCLUDE_ACCESS_INVALID", "include范围项须为对象。");
                const auto object = value.toObject();
                if (!OnlyFields(object, {"path", "kind", "encoding", "required"}, error)) return error;
                auto path = ParseAbsolutePath(object, "path");
                const auto kind = RequiredText(object, "kind", 9, error);
                const auto encoding = RequiredText(object, "encoding", 4, error);
                if (!path.Succeeded()) return Failure(path);
                if (!kind || !encoding) return error;
                if (object.contains("required") && !object.value("required").isBool())
                    return Failure("MCP_INCLUDE_ACCESS_INVALID", "required 必须为布尔值。");
                auto declaration = Services::ParseScriptIncludeAccessDeclaration(*path.value, *kind, *encoding,
                    object.value("required").toBool(true));
                if (!declaration.Succeeded() || !declaration.value) return Failure(declaration);
                request.includeAccess.push_back(std::move(*declaration.value));
            }
        }
        if (parameters.contains("ui_root")) {
            auto uiRoot = ParseAbsolutePath(parameters, "ui_root");
            if (!uiRoot.Succeeded()) return Failure(uiRoot);
            std::error_code uiPathError;
            if (!std::filesystem::is_directory(*uiRoot.value, uiPathError) || uiPathError)
                return Failure("MCP_UI_ROOT_INVALID", "ui_root 必须是可访问的 UI 元数据绝对目录。");
            request.uiRoot = *uiRoot.value;
        }
        if (customRequest) {
            const QJsonObject edit{{"expected_member_sha256", parameters.value("expected_settings_sha256")}, {"operations", parameters.value("settings_custom_edits")}};
            auto parsed = Services::ParseMapCustomSettingsEdit(QJsonDocument(edit).toJson(QJsonDocument::Compact).toStdString());
            if (!parsed.Succeeded()) return Failure(parsed);
            request.settingsCustomEdits = std::move(*parsed.value);
        } else if (fieldRequest) {
            const auto edits = parameters.value("field_edits");
            if (!edits.isArray() || edits.toArray().isEmpty() || edits.toArray().size() > 32)
                return Failure("MCP_MAP_EDIT_FIELD_EDITS_INVALID", "field_edits 必须包含 1..32 项字段修改。");
            for (const auto& value : edits.toArray()) {
                if (!value.isObject()) return Failure("MCP_MAP_EDIT_FIELD_EDITS_INVALID", "每项字段修改必须是对象。");
                auto operation = ParseFieldEdit(value.toObject(), error);
                if (!operation) return error;
                request.fieldEdits.push_back(std::move(*operation));
            }
        } else if (triggerRequest) {
            if (!parameters.value("trigger_edits").isObject()) return Failure("MCP_TRIGGER_EDITS_INVALID", "trigger_edits 须为对象。");
            auto batch = ParseTriggerEdits(parameters.value("trigger_edits").toObject(), error);
            if (!batch) return error;
            request.triggerEdits = std::move(*batch);
        } else if (!workspaceRequest && terrainRequest) {
            const auto terrainSha = RequiredSha(parameters, "expected_terrain_sha256", error);
            if (!terrainSha) return error;
            request.expectedTerrainSha256 = *terrainSha;
            const auto strokes = parameters.value("terrain_strokes");
            if (!strokes.isArray() || strokes.toArray().isEmpty() || strokes.toArray().size() > 128)
                return Failure("MCP_MAP_EDIT_TERRAIN_STROKES_INVALID", "地形请求必须包含 1..128 笔有序笔划。");
            std::size_t totalPoints = 0;
            for (const auto& value : strokes.toArray()) {
                if (!value.isObject()) return Failure("MCP_MAP_EDIT_TERRAIN_STROKE_INVALID",
                    "每笔地形操作必须是类型化对象。");
                auto stroke = ParseTerrainStroke(value.toObject(), error);
                if (!stroke) return error;
                if (stroke->points.size() > 4096 - totalPoints)
                    return Failure("MCP_MAP_EDIT_TERRAIN_POINTS_INVALID", "有序笔划总点数不得超过 4096。");
                totalPoints += stroke->points.size();
                request.terrainStrokes.push_back(std::move(*stroke));
            }
            if (request.terrainStrokes.size() != 1 && std::any_of(request.terrainStrokes.begin(), request.terrainStrokes.end(),
                    [](const auto& command) { return command.blockPaste.has_value(); }))
                return Failure("MCP_MAP_EDIT_BLOCK_ORDER", "block_copy必须是唯一地形命令；不与其他笔刷或可玩区命令混用。");
        }
        if (!ReadEffectiveLayers(parameters, request.effectiveLayers, error)) return error;
        const auto saved = workspaceRequest
            ? workspace->ExportArchiveCandidate(envelope->first, envelope->second, std::move(request))
            : Services::SaveMapEditCandidate(request);
        if (!saved.Succeeded() || !saved.value) return Failure(saved);
        const auto& receipt = *saved.value;
        QJsonArray layers;
        for (const auto& layer : receipt.effectiveLayers) layers.append(QString::fromStdString(layer));
        QJsonArray editedMembers;
        for (const auto& member : receipt.editedMembers) {
            editedMembers.append(QJsonObject{{"name", QString::fromStdString(member.name)},
                {"original_sha256", QString::fromStdString(member.originalSha256)},
                {"saved_sha256", QString::fromStdString(member.savedSha256)}});
        }
        QJsonObject result{{"ok", true}, {"candidate_kind", "mpq-map-archive"},
            {"output_path", QString::fromStdWString(receipt.candidate.wstring())},
            {"source_sha256", QString::fromStdString(receipt.sourceSha256)},
            {"candidate_sha256", QString::fromStdString(receipt.candidateSha256)},
            {"original_terrain_sha256", QString::fromStdString(receipt.originalTerrainSha256)},
            {"saved_terrain_sha256", QString::fromStdString(receipt.savedTerrainSha256)},
            {"saved_pathing_sha256", QString::fromStdString(receipt.savedPathingSha256)},
            {"edited_members", editedMembers},
            {"member_count", static_cast<qint64>(receipt.memberCount)},
            {"script_seconds", receipt.scriptSeconds}, {"script_rebuilt", true},
            {"script_preserved", receipt.scriptPreserved},
            {"runtime_purpose", QString::fromUtf8(Services::MapRuntimeSavePurposeName(receipt.runtimePurpose))},
            {"runtime_packaged", receipt.runtimePackaged},
            {"applied_field_edit_count", static_cast<qint64>(receipt.appliedFieldEditCount)},
            {"applied_settings_custom_edit_count", static_cast<qint64>(receipt.appliedSettingsCustomEditCount)},
            {"applied_placement_count", static_cast<qint64>(receipt.appliedPlacementCount)},
            {"applied_terrain_stroke_count", static_cast<qint64>(receipt.appliedStrokeCount)},
            {"effective_layers", layers}, {"native_editor_verified", false},
            {"game_runtime_verified", false}};
        if (receipt.triggerTransaction) result["trigger_transaction_receipt"] = QJsonObject{
            {"transaction_id", QString::number(receipt.triggerTransaction->binding.transactionId)},
            {"edit_generation", QString::number(receipt.triggerTransaction->binding.editGeneration)},
            {"directory_revision", RevisionJson(receipt.triggerTransaction->binding.baseRevision)},
            {"transaction_active", true}, {"workspace_rebound", false},
            {"variable_count", static_cast<qint64>(receipt.triggerTransaction->variableCount)},
            {"trigger_count", static_cast<qint64>(receipt.triggerTransaction->triggerCount)},
            {"eca_count", static_cast<qint64>(receipt.triggerTransaction->ecaCount)},
            {"argument_count", static_cast<qint64>(receipt.triggerTransaction->argumentCount)}};
        if (receipt.triggerEdits) result["trigger_edit_receipt"] = QJsonObject{
            {"operation_count", static_cast<qint64>(receipt.triggerEdits->operationCount)},
            {"removed_eca_count", static_cast<qint64>(receipt.triggerEdits->removedEcaCount)},
            {"removed_argument_count", static_cast<qint64>(receipt.triggerEdits->removedArgumentCount)},
            {"enabled_change_count", static_cast<qint64>(receipt.triggerEdits->enabledChangeCount)},
            {"argument_change_count", static_cast<qint64>(receipt.triggerEdits->argumentChangeCount)},
            {"appended_root_count", static_cast<qint64>(receipt.triggerEdits->appendedRootCount)},
            {"text_change_count", static_cast<qint64>(receipt.triggerEdits->textChangeCount)},
            {"converted_gui_count", static_cast<qint64>(receipt.triggerEdits->convertedGuiCount)}};
        if (!saved.diagnostics.empty())
            result["diagnostics"] = DiagnosticsJson(saved.diagnostics).value("diagnostics");
        if (static_cast<std::size_t>(QJsonDocument(result).toJson(QJsonDocument::Compact).size()) >
            kMaxResponseBytes) return Failure("MCP_MAP_EDIT_JSON_BUDGET", "候选保存回执超过输出预算。");
        return result;
    };
    server.RegisterTool(std::move(tool));
    RegisterTriggerTextQueryTool(server);
}

} // namespace War3Map::Mcp
