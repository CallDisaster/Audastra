#include "MapEditSaveValidation.h"
#include "War3MapCore/MapData/PathingRestrictionEditing.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace War3Map::Services::MapEditValidation {

constexpr std::size_t kMaximumStrokes = 128;
constexpr std::size_t kMaximumPointsPerStroke = 4096;
constexpr std::size_t kMaximumTotalPoints = 4096;

bool ValidSha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

Status ValidateFieldOperations(std::span<const MapEditFieldOperation> operations) {
    if (operations.empty() || operations.size() > MapData::kMaxMapFieldEdits) {
        return Status::Failure("MapEdit.Save", "SAVE_FIELD_COUNT",
            "同一候选只接受 1 到 32 项字段修改。");
    }
    for (const auto& operation : operations) {
        if (!ValidSha(operation.expectedMemberSha256) || operation.edit.expectedRowKey.empty()
            || operation.edit.expectedRowKey.size() > 16 * 1024 || operation.edit.field.empty()
            || operation.edit.field.size() > 64) {
            return Status::Failure("MapEdit.Save", "SAVE_FIELD_REQUEST_INVALID",
                "字段修改需要小写成员 SHA、非空行身份和字段标识。");
        }
        const auto sameKind = [&operation](const MapEditFieldOperation& other) {
            return other.edit.kind == operation.edit.kind;
        };
        if (std::any_of(operations.begin(), operations.end(), [&](const auto& other) {
                return sameKind(other) && other.expectedMemberSha256 != operation.expectedMemberSha256;
            })) {
            return Status::Failure("MapEdit.Save", "SAVE_FIELD_REQUEST_INVALID",
                "同一类别的字段修改必须绑定同一成员 SHA。");
        }
    }
    return Status::Ok();
}

Status ValidatePlacementOperation(const MapEditPlacementOperation& operation) {
    const auto validSource = [](const MapEditPlacementSource& source) {
        return (source.file == MapData::PlacementFile::Units || source.file == MapData::PlacementFile::Doodads)
            && !source.expectedRowKey.empty() && source.expectedRowKey.size() <= 16 * 1024
            && ValidSha(source.expectedMemberSha256);
    };
    if (const auto* copy = std::get_if<MapEditPlacementCopy>(&operation)) {
        if (copy->sources.empty() || copy->sources.size() > 4096
            || std::any_of(copy->sources.begin(), copy->sources.end(), [&](const auto& source) {
                return !validSource(source);
            }) || std::any_of(copy->offset.begin(), copy->offset.end(), [](float value) {
                return !std::isfinite(value) || std::abs(value) > 1'000'000.0f;
            })) {
            return Status::Failure("MapEdit.Save", "SAVE_PLACEMENT_REQUEST_INVALID",
                "组复制需要 1..4096 个绑定行和有限的世界坐标偏移。");
        }
        return Status::Ok();
    }
    if (const auto* rotation = std::get_if<MapEditPlacementRotation>(&operation)) {
        if (!validSource(rotation->source) || !std::isfinite(rotation->facingRadians)
            || rotation->facingRadians < 0
            || rotation->facingRadians >= static_cast<float>(2 * std::numbers::pi_v<double>)) {
            return Status::Failure("MapEdit.Save", "SAVE_PLACEMENT_REQUEST_INVALID",
                "朝向修改需要绑定行和 [0, 360) 度的有限角度。");
        }
        return Status::Ok();
    }
    if (const auto* created = std::get_if<MapEditPlacementCreate>(&operation)) {
        const auto count = std::visit([](const auto& records) { return records.size(); }, created->records);
        if (!ValidSha(created->expectedMemberSha256) || !count || count > 32)
            return Status::Failure("MapEdit.Save", "SAVE_PLACEMENT_REQUEST_INVALID", "新增摆放须为 1..32 条同类记录并绑定原成员 SHA。");
        const auto* units = std::get_if<std::vector<MapData::UnitPlacement>>(&created->records);
        const auto starts = units && std::any_of(units->begin(), units->end(), [](const auto& record) { return record.rawcode == "sloc"; });
        if (starts) return Status::Failure("MapEdit.Save", "SAVE_PLACEMENT_START_UNSUPPORTED", "新增起始位置须走玩家设置联动命令。");
        return Status::Ok();
    }
    if (const auto* removed = std::get_if<MapEditPlacementDelete>(&operation)) {
        if (removed->sources.empty() || removed->sources.size() > 32
            || std::any_of(removed->sources.begin(), removed->sources.end(), [&](const auto& source) { return !validSource(source); }))
            return Status::Failure("MapEdit.Save", "SAVE_PLACEMENT_REQUEST_INVALID", "删除摆放须为 1..32 个绑定来源行。");
        return Status::Ok();
    }
    return Status::Failure("MapEdit.Save", "SAVE_PLACEMENT_REQUEST_INVALID", "未提供摆放编辑操作。");
}

Status ValidateStrokes(std::span<const MapEditTerrainStroke> strokes) {
    if (strokes.empty() || strokes.size() > kMaximumStrokes) {
        return Status::Failure("MapEdit.Save", "SAVE_STROKE_COUNT",
            "同一候选只接受 1 到 128 笔有序地形笔划。");
    }
    std::size_t pointCount = 0;
    for (const auto& stroke : strokes) {
        if (stroke.bounds) {
            const auto& brush = stroke.brush; const MapData::TerrainBrush neutral;
            const auto& r = stroke.bounds->playable;
            if (brush.kind != neutral.kind || brush.shape != neutral.shape || brush.radius != neutral.radius
                || brush.groundTexture != neutral.groundTexture || brush.cliffTexture != neutral.cliffTexture
                || brush.levelDelta != neutral.levelDelta || brush.value != neutral.value
                || brush.strength != neutral.strength || brush.seed != neutral.seed || brush.anchor
                || brush.size || brush.variation || brush.randomizeVariation != neutral.randomizeVariation
                || brush.waterHeight != neutral.waterHeight || brush.pathingRestrictions != neutral.pathingRestrictions
                || !stroke.points.empty() || stroke.pointSpace != MapEditPointSpace::World
                || stroke.followDoodads || stroke.waterBrush || stroke.bounds->algorithm != 1
                || r.west >= r.east || r.south >= r.north || r.east > 2048 || r.north > 2048)
                return Status::Failure("MapEdit.Save", "SAVE_STROKE_BOUNDS_INVALID",
                    "可玩区只接受独立的整数地格半开矩形；不搬移对象、不改变地图尺寸、不接受客户端规则。");
            continue;
        }
        if (stroke.waterBrush) {
            const auto& brush = stroke.brush;
            const MapData::TerrainBrush neutral;
            if (brush.kind != neutral.kind || brush.shape != neutral.shape || brush.radius != neutral.radius
                || brush.groundTexture != neutral.groundTexture || brush.cliffTexture != neutral.cliffTexture
                || brush.levelDelta != neutral.levelDelta || brush.value != neutral.value
                || brush.strength != neutral.strength || brush.seed != neutral.seed || brush.anchor
                || brush.size || brush.variation || brush.randomizeVariation != neutral.randomizeVariation
                || brush.waterHeight != neutral.waterHeight || brush.pathingRestrictions != neutral.pathingRestrictions)
                return Status::Failure("MapEdit.Save", "SAVE_STROKE_BRUSH_CONFLICT", "浅/深水笔划不能同时指定普通地形笔刷参数。");
            const auto& water = *stroke.waterBrush;
            if ((water.kind != MapData::TerrainWaterBrushKind::Shallow && water.kind != MapData::TerrainWaterBrushKind::Deep)
                || (water.shape != MapData::TerrainBrushShape::Circle && water.shape != MapData::TerrainBrushShape::Square)
                || water.size < 1 || water.size > 8 || water.cliffTexture >= 15
                || (water.algorithm != MapData::TerrainWaterAlgorithm::LegacyFlat
                    && water.algorithm != MapData::TerrainWaterAlgorithm::NativeRamps))
                return Status::Failure("MapEdit.Save", "SAVE_STROKE_WATER_BOUNDS", "浅/深水笔刷种类、形状、经典尺寸或悬崖槽位非法。");
        } else {
            switch (stroke.brush.kind) {
            case MapData::TerrainBrushKind::Texture:
            case MapData::TerrainBrushKind::Blight:
            case MapData::TerrainBrushKind::Raise:
            case MapData::TerrainBrushKind::Lower:
            case MapData::TerrainBrushKind::Plateau:
            case MapData::TerrainBrushKind::Smooth:
            case MapData::TerrainBrushKind::CliffLevel:
            // 仅接纳意图；换材质的净差仍须真实CliffTypes行、旧变体及完整native WPM重建门。
            case MapData::TerrainBrushKind::CliffTexture:
            case MapData::TerrainBrushKind::WaterHeight:
            case MapData::TerrainBrushKind::Ramp:
            case MapData::TerrainBrushKind::CornerRamp:
            case MapData::TerrainBrushKind::PathingRestriction:
                break;
            default:
                return Status::Failure("MapEdit.Save", "SAVE_STROKE_KIND", "地形笔刷种类未获识别。");
            }
            if (stroke.brush.kind == MapData::TerrainBrushKind::CornerRamp
                && (!MapData::IsCanonicalTerrainCornerRampBrush(stroke.brush) || stroke.points.size() != 1))
                return Status::Failure("MapEdit.Save", "SAVE_STROKE_CORNER_RAMP", "拐角坡道只接受单个canonical点及创建/移除选项。");
            if (stroke.brush.kind == MapData::TerrainBrushKind::PathingRestriction
                && (!stroke.brush.pathingRestrictions || (stroke.brush.pathingRestrictions & ~MapData::kPathingRestrictionBits) != 0))
                return Status::Failure("MapEdit.Save", "PATHING_RESTRICTION_FLAGS", "通行笔划只允许不可走、不可飞和不可建限制。");
            if (stroke.brush.shape != MapData::TerrainBrushShape::Circle
                && stroke.brush.shape != MapData::TerrainBrushShape::Square) {
                return Status::Failure("MapEdit.Save", "SAVE_STROKE_SHAPE", "地形笔刷形状未获识别。");
            }
            if (stroke.brush.radius > 16 || !std::isfinite(stroke.brush.strength)
                || stroke.brush.strength < 0 || stroke.brush.strength > 1024
                || stroke.brush.groundTexture.size() > 4
                || stroke.brush.levelDelta < -1 || stroke.brush.levelDelta > 1) {
                return Status::Failure("MapEdit.Save", "SAVE_STROKE_BRUSH_BOUNDS", "地形笔刷参数超过有界请求范围。");
            }
        }
        if (stroke.pointSpace != MapEditPointSpace::World
            && stroke.pointSpace != MapEditPointSpace::TerrainVertex) {
            return Status::Failure("MapEdit.Save", "SAVE_STROKE_POINT_SPACE", "地形笔划坐标系未获识别。");
        }
        if (stroke.points.empty() || stroke.points.size() > kMaximumPointsPerStroke
            || stroke.points.size() > kMaximumTotalPoints - pointCount) {
            return Status::Failure("MapEdit.Save", "SAVE_STROKE_POINTS", "笔划点总数必须在 1 到 4096 之间。");
        }
        pointCount += stroke.points.size();
        if (std::any_of(stroke.points.begin(), stroke.points.end(), [](const auto& point) {
            return !std::isfinite(point.x) || !std::isfinite(point.y);
        })) {
            return Status::Failure("MapEdit.Save", "SAVE_STROKE_POINTS", "笔划点坐标必须是有限数。");
        }
    }
    return Status::Ok();
}

Result<std::vector<MapData::TerrainStrokePoint>> WorldPoints(
    const MapEditTerrainStroke& stroke, const MapData::TerrainGridSnapshot& terrain) {
    std::vector<MapData::TerrainStrokePoint> points;
    points.reserve(stroke.points.size());
    const double minX = terrain.header.offset[0], minY = terrain.header.offset[1];
    const double maxX = minX + (static_cast<double>(terrain.header.width) - 1.0) * 128.0;
    const double maxY = minY + (static_cast<double>(terrain.header.height) - 1.0) * 128.0;
    for (const auto& point : stroke.points) {
        if (stroke.pointSpace == MapEditPointSpace::TerrainVertex) {
            if (point.x < 0 || point.y < 0 || point.x >= terrain.header.width
                || point.y >= terrain.header.height || std::trunc(point.x) != point.x
                || std::trunc(point.y) != point.y) {
                return Result<std::vector<MapData::TerrainStrokePoint>>::Failure(
                    "MapEdit.Save", "SAVE_VERTEX_BOUNDS", "笔划点必须是源 W3E 范围内的整数顶点坐标。");
            }
            points.push_back({minX + point.x * 128.0, minY + point.y * 128.0});
        } else {
            if (point.x < minX || point.x > maxX || point.y < minY || point.y > maxY) {
                return Result<std::vector<MapData::TerrainStrokePoint>>::Failure(
                    "MapEdit.Save", "SAVE_STROKE_POINT_BOUNDS", "世界坐标笔划点超出源 W3E 地形范围。");
            }
            points.push_back(point);
        }
    }
    return Result<std::vector<MapData::TerrainStrokePoint>>::Success(std::move(points));
}


} // namespace War3Map::Services::MapEditValidation
