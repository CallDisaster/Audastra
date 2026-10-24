#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Shape(TerrainBrushShape shape) { return shape == TerrainBrushShape::Circle || shape == TerrainBrushShape::Square; }
bool Neutral(const TerrainBrush& brush) {
    const TerrainBrush other;
    return brush.kind == other.kind && brush.shape == other.shape && brush.radius == other.radius
        && brush.groundTexture == other.groundTexture && brush.cliffTexture == other.cliffTexture
        && brush.levelDelta == other.levelDelta && brush.value == other.value && brush.strength == other.strength
        && brush.seed == other.seed && !brush.anchor && !brush.size && !brush.variation
        && brush.randomizeVariation == other.randomizeVariation && brush.waterHeight == other.waterHeight
        && brush.pathingRestrictions == other.pathingRestrictions;
}
bool ValidStroke(const TerrainJournalStroke& stroke, std::size_t& points) {
    const auto& intent = stroke.intent;
    const auto& brush = intent.brush;
    if (stroke.intent.bounds) {
        const auto& bounds = *stroke.intent.bounds; const auto& r = bounds.playable;
        return Neutral(brush) && intent.points.empty() && !intent.followDoodads && !intent.waterBrush
            && !stroke.cliffEnforceWaterLimits && Sha(stroke.rulesSha256) && bounds.algorithm == 1
            && r.west < r.east && r.south < r.north && r.east <= 2048 && r.north <= 2048;
    }
    if (intent.points.empty() || intent.points.size() > 4096 - points || brush.anchor) return false;
    points += intent.points.size();
    for (const auto& point : intent.points) if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    if (!Shape(brush.shape) || brush.radius > 16 || brush.groundTexture.size() > 4
        || (!brush.groundTexture.empty() && brush.groundTexture.size() != 4)
        || !std::all_of(brush.groundTexture.begin(), brush.groundTexture.end(), [](unsigned char c) { return c >= 32 && c <= 126; })
        || brush.cliffTexture >= 15 || brush.levelDelta < -1 || brush.levelDelta > 1
        || !std::isfinite(brush.strength) || brush.strength < 0 || brush.strength > 1024
        || !std::isfinite(brush.waterHeight) || brush.waterHeight < -2048 || brush.waterHeight > 2047.75f
        || (brush.size && (*brush.size < 1 || *brush.size > 8)) || (brush.variation && *brush.variation > 17)
        || !brush.pathingRestrictions || (brush.pathingRestrictions & ~0x0e) != 0) return false;
    switch (brush.kind) {
    case TerrainBrushKind::Texture: case TerrainBrushKind::Blight: case TerrainBrushKind::Raise:
    case TerrainBrushKind::Lower: case TerrainBrushKind::Plateau: case TerrainBrushKind::Smooth:
    case TerrainBrushKind::CliffLevel: case TerrainBrushKind::Noise: case TerrainBrushKind::CliffTexture:
    case TerrainBrushKind::WaterHeight: case TerrainBrushKind::Ramp: case TerrainBrushKind::Boundary:
    case TerrainBrushKind::PathingRestriction: break;
    case TerrainBrushKind::CornerRamp: break;
    default: return false;
    }
    if (!intent.waterBrush && brush.kind == TerrainBrushKind::CornerRamp)
        return intent.points.size() == 1 && IsCanonicalTerrainCornerRampBrush(brush)
            && Sha(stroke.rulesSha256) && !stroke.cliffEnforceWaterLimits;
    if (!intent.waterBrush) return (stroke.rulesSha256.empty() && !stroke.cliffEnforceWaterLimits)
        || (brush.kind == TerrainBrushKind::CliffLevel && stroke.cliffEnforceWaterLimits.has_value() && Sha(stroke.rulesSha256));
    if (stroke.cliffEnforceWaterLimits) return false;
    if (!Neutral(brush)) return false;
    const auto& water = *intent.waterBrush;
    return Sha(stroke.rulesSha256) && Shape(water.shape)
        && (water.kind == TerrainWaterBrushKind::Shallow || water.kind == TerrainWaterBrushKind::Deep)
        && water.size >= 1 && water.size <= 8 && water.cliffTexture < 15
        && (water.algorithm == TerrainWaterAlgorithm::LegacyFlat || water.algorithm == TerrainWaterAlgorithm::NativeRamps);
}
bool Valid(const TerrainStrokeJournal& journal) {
    if (!Sha(journal.sourceSha256) || journal.members.size() != TerrainJournalMemberNames.size()
        || journal.strokes.empty() || journal.strokes.size() > 128
        || journal.baselinePathingLayers.size() > TerrainJournalMaximumBytes - 128 * 1024) return false;
    for (std::size_t i = 0; i < journal.members.size(); ++i) {
        const auto& member = journal.members[i];
        if (member.name != TerrainJournalMemberNames[i] || (i < 2 && !member.present)
            || (member.present ? !Sha(member.sha256) : (!member.sha256.empty() || member.bytes != 0))) return false;
    }
    std::size_t points = 0;
    for (const auto& stroke : journal.strokes) if (!ValidStroke(stroke, points)) return false;
    return true;
}
struct Wire {
    std::vector<std::byte> data;
    void Number(std::uint64_t value, unsigned bytes) {
        if (bytes > TerrainJournalMaximumBytes - data.size()) throw std::length_error("journal");
        for (unsigned i = 0; i < bytes; ++i) data.push_back(std::byte((value >> (i * 8)) & 255));
    }
    void Bytes(std::span<const std::byte> bytes) {
        Number(bytes.size(), 4);
        if (bytes.size() > TerrainJournalMaximumBytes - data.size()) throw std::length_error("journal");
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    void Text(std::string_view value) { Bytes(std::as_bytes(std::span(value.data(), value.size()))); }
};
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t cursor = 0;
    std::uint64_t Number(unsigned count) {
        if (count > bytes.size() - cursor) throw std::invalid_argument("journal");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[cursor++])) << (i * 8);
        return value;
    }
    bool Bool() { const auto value = Number(1); if (value > 1) throw std::invalid_argument("journal"); return value != 0; }
    std::span<const std::byte> Bytes(std::size_t maximum) {
        const auto count = Number(4);
        if (count > maximum || count > bytes.size() - cursor) throw std::invalid_argument("journal");
        const auto result = bytes.subspan(cursor, static_cast<std::size_t>(count)); cursor += count; return result;
    }
    std::string Text(std::size_t maximum) {
        const auto value = Bytes(maximum); return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    }
};
template<class T> Result<T> Failed() {
    return Result<T>::Failure("Terrain.Journal", "TERRAIN_JOURNAL_INVALID", "地形前向记录版本、结构、来源身份或累计预算无效，未恢复任何命令。");
}
} // namespace

Status ValidateTerrainStrokeJournal(const TerrainStrokeJournal& journal) {
    return Valid(journal) ? Status::Ok() : Status{Failed<TerrainStrokeJournal>().diagnostics};
}

Result<std::vector<std::byte>> EncodeTerrainStrokeJournal(const TerrainStrokeJournal& journal) {
    if (!Valid(journal)) return Failed<std::vector<std::byte>>();
    try {
        Wire wire;
        const bool nativeCliffs = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.cliffEnforceWaterLimits.has_value(); });
        const bool nativeWater = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.waterBrush
                && stroke.intent.waterBrush->algorithm == TerrainWaterAlgorithm::NativeRamps; });
        const bool cornerRamps = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp; });
        const bool bounds = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.bounds.has_value(); });
        wire.Number(bounds ? 0x354a5446 : cornerRamps ? 0x344a5446 : nativeWater ? 0x334a5446 : nativeCliffs ? 0x324a5446 : 0x314a5446, 4);
        wire.Text(journal.sourceSha256);
        wire.Number(journal.members.size(), 1);
        for (const auto& member : journal.members) {
            wire.Text(member.name); wire.Number(member.present, 1); wire.Text(member.sha256); wire.Number(member.bytes, 8);
        }
        wire.Bytes(journal.baselinePathingLayers);
        wire.Number(journal.strokes.size(), 4);
        for (const auto& stroke : journal.strokes) {
            if (bounds) {
                wire.Number(stroke.intent.bounds.has_value(), 1);
                if (stroke.intent.bounds) {
                    const auto& value = *stroke.intent.bounds;
                    wire.Number(value.algorithm, 1); wire.Number(value.playable.west, 4); wire.Number(value.playable.south, 4);
                    wire.Number(value.playable.east, 4); wire.Number(value.playable.north, 4); wire.Text(stroke.rulesSha256);
                    continue;
                }
            }
            const auto& intent = stroke.intent; const auto& brush = intent.brush;
            wire.Number(static_cast<unsigned>(brush.kind), 1); wire.Number(static_cast<unsigned>(brush.shape), 1);
            wire.Number(brush.radius, 4); wire.Text(brush.groundTexture); wire.Number(brush.cliffTexture, 1);
            wire.Number(static_cast<std::uint32_t>(brush.levelDelta), 4); wire.Number(brush.value, 1);
            wire.Number(std::bit_cast<std::uint32_t>(brush.strength), 4); wire.Number(brush.seed, 4);
            wire.Number(brush.size.has_value(), 1); if (brush.size) wire.Number(*brush.size, 4);
            wire.Number(brush.variation.has_value(), 1); if (brush.variation) wire.Number(*brush.variation, 1);
            wire.Number(brush.randomizeVariation, 1); wire.Number(std::bit_cast<std::uint32_t>(brush.waterHeight), 4);
            wire.Number(brush.pathingRestrictions, 1); wire.Number(intent.followDoodads, 1);
            wire.Number(intent.waterBrush.has_value(), 1);
            if (intent.waterBrush) {
                const auto& water = *intent.waterBrush;
                wire.Number(static_cast<unsigned>(water.kind), 1); wire.Number(static_cast<unsigned>(water.shape), 1);
                wire.Number(water.size, 4); wire.Number(water.cliffTexture, 1); wire.Number(water.randomSeed, 4);
                wire.Number(water.enforceWaterLimits, 1);
                if (nativeWater || cornerRamps || bounds) wire.Number(static_cast<unsigned>(water.algorithm), 1);
            }
            if (nativeCliffs || nativeWater || cornerRamps || bounds) {
                wire.Number(stroke.cliffEnforceWaterLimits.has_value(), 1);
                if (stroke.cliffEnforceWaterLimits) wire.Number(*stroke.cliffEnforceWaterLimits, 1);
            }
            wire.Text(stroke.rulesSha256); wire.Number(intent.points.size(), 4);
            for (const auto& point : intent.points) {
                wire.Number(std::bit_cast<std::uint64_t>(point.x), 8); wire.Number(std::bit_cast<std::uint64_t>(point.y), 8);
            }
        }
        return Result<std::vector<std::byte>>::Success(std::move(wire.data));
    } catch (const std::bad_alloc&) { return Failed<std::vector<std::byte>>(); }
      catch (const std::length_error&) { return Failed<std::vector<std::byte>>(); }
}

Result<TerrainStrokeJournal> DecodeTerrainStrokeJournal(std::span<const std::byte> bytes) {
    if (bytes.size() > TerrainJournalMaximumBytes) return Failed<TerrainStrokeJournal>();
    try {
        Reader reader{bytes}; TerrainStrokeJournal journal;
        const auto version = reader.Number(4);
        if (version != 0x314a5446 && version != 0x324a5446 && version != 0x334a5446 && version != 0x344a5446 && version != 0x354a5446)
            return Failed<TerrainStrokeJournal>();
        journal.sourceSha256 = reader.Text(64);
        if (reader.Number(1) != TerrainJournalMemberNames.size()) return Failed<TerrainStrokeJournal>();
        for (std::size_t i = 0; i < TerrainJournalMemberNames.size(); ++i) {
            TerrainJournalMember member; member.name = reader.Text(32); member.present = reader.Bool();
            member.sha256 = reader.Text(64); member.bytes = reader.Number(8); journal.members.push_back(std::move(member));
        }
        const auto layers = reader.Bytes(TerrainJournalMaximumBytes - 128 * 1024);
        journal.baselinePathingLayers.assign(layers.begin(), layers.end());
        const auto count = reader.Number(4);
        if (!count || count > 128) return Failed<TerrainStrokeJournal>();
        std::size_t points = 0;
        for (std::size_t i = 0; i < count; ++i) {
            TerrainJournalStroke stroke; auto& intent = stroke.intent; auto& brush = intent.brush;
            if (version == 0x354a5446) {
                const auto kind = reader.Number(1);
                if (kind > 1) return Failed<TerrainStrokeJournal>();
                if (kind == 1) {
                    TerrainBoundsEditIntent bounds; bounds.algorithm = static_cast<std::uint8_t>(reader.Number(1));
                    bounds.playable.west = static_cast<std::uint32_t>(reader.Number(4));
                    bounds.playable.south = static_cast<std::uint32_t>(reader.Number(4));
                    bounds.playable.east = static_cast<std::uint32_t>(reader.Number(4));
                    bounds.playable.north = static_cast<std::uint32_t>(reader.Number(4));
                    stroke.intent.bounds = bounds; stroke.rulesSha256 = reader.Text(64);
                    journal.strokes.push_back(std::move(stroke)); continue;
                }
            }
            brush.kind = static_cast<TerrainBrushKind>(reader.Number(1)); brush.shape = static_cast<TerrainBrushShape>(reader.Number(1));
            if (brush.kind == TerrainBrushKind::CornerRamp && version != 0x344a5446 && version != 0x354a5446) return Failed<TerrainStrokeJournal>();
            brush.radius = static_cast<std::uint32_t>(reader.Number(4)); brush.groundTexture = reader.Text(4);
            brush.cliffTexture = static_cast<std::uint8_t>(reader.Number(1));
            brush.levelDelta = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Number(4))); brush.value = reader.Bool();
            brush.strength = std::bit_cast<float>(static_cast<std::uint32_t>(reader.Number(4))); brush.seed = static_cast<std::uint32_t>(reader.Number(4));
            if (reader.Bool()) brush.size = static_cast<std::uint32_t>(reader.Number(4));
            if (reader.Bool()) brush.variation = static_cast<std::uint8_t>(reader.Number(1));
            brush.randomizeVariation = reader.Bool(); brush.waterHeight = std::bit_cast<float>(static_cast<std::uint32_t>(reader.Number(4)));
            brush.pathingRestrictions = static_cast<std::uint8_t>(reader.Number(1)); intent.followDoodads = reader.Bool();
            if (reader.Bool()) {
                TerrainWaterStrokeOptions water; water.kind = static_cast<TerrainWaterBrushKind>(reader.Number(1));
                water.shape = static_cast<TerrainBrushShape>(reader.Number(1)); water.size = static_cast<std::uint32_t>(reader.Number(4));
                water.cliffTexture = static_cast<std::uint8_t>(reader.Number(1)); water.randomSeed = static_cast<std::uint32_t>(reader.Number(4));
                water.enforceWaterLimits = reader.Bool();
                // FTJ1/2的水语义固定为旧flat算法，资源摘要不能授予新算法版本。
                water.algorithm = (version == 0x334a5446 || version == 0x344a5446 || version == 0x354a5446) ? static_cast<TerrainWaterAlgorithm>(reader.Number(1))
                    : TerrainWaterAlgorithm::LegacyFlat;
                intent.waterBrush = water;
            }
            if (version != 0x314a5446 && reader.Bool()) stroke.cliffEnforceWaterLimits = reader.Bool();
            stroke.rulesSha256 = reader.Text(64);
            const auto pointCount = reader.Number(4);
            if (!pointCount || pointCount > 4096 - points || pointCount > (reader.bytes.size() - reader.cursor) / 16)
                return Failed<TerrainStrokeJournal>();
            points += pointCount; intent.points.reserve(static_cast<std::size_t>(pointCount));
            for (std::size_t p = 0; p < pointCount; ++p) {
                const auto x = std::bit_cast<double>(reader.Number(8)); const auto y = std::bit_cast<double>(reader.Number(8));
                intent.points.push_back({x, y});
            }
            journal.strokes.push_back(std::move(stroke));
        }
        if (reader.cursor != bytes.size() || !Valid(journal)
            || (version == 0x324a5446 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
                [](const auto& stroke) { return stroke.cliffEnforceWaterLimits.has_value(); })))
            return Failed<TerrainStrokeJournal>();
        if (version == 0x334a5446 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.waterBrush
                && stroke.intent.waterBrush->algorithm == TerrainWaterAlgorithm::NativeRamps; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 0x344a5446 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 0x354a5446 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.bounds.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        return Result<TerrainStrokeJournal>::Success(std::move(journal));
    } catch (const std::bad_alloc&) { return Failed<TerrainStrokeJournal>(); }
      catch (const std::length_error&) { return Failed<TerrainStrokeJournal>(); }
      catch (const std::invalid_argument&) { return Failed<TerrainStrokeJournal>(); }
}

} // namespace War3Map::MapData
