#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "War3MapCore/MapData/TerrainRampEditing.h"
#include "TerrainHistoryCodec.h"

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
bool Rawcode(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
bool ValidStroke(const TerrainJournalStroke& stroke, std::size_t& points) {
    if ((stroke.intent.brush.kind != TerrainBrushKind::CornerRamp || stroke.intent.waterBrush
            || stroke.intent.bounds || stroke.intent.palette || stroke.intent.blockPaste || stroke.intent.resize || stroke.ordinaryDoodads || stroke.frozenDoodads
            || stroke.units || stroke.frozenUnits || stroke.startLocations || stroke.frozenStartLocations)
        && stroke.intent.cornerRampAlgorithm != TerrainCornerRampAlgorithm::DryOnly) return false;
    if ((stroke.intent.brush.kind != TerrainBrushKind::StraightRamp || stroke.intent.waterBrush
            || stroke.intent.bounds || stroke.intent.palette || stroke.intent.blockPaste || stroke.intent.resize || stroke.ordinaryDoodads || stroke.frozenDoodads
            || stroke.units || stroke.frozenUnits || stroke.startLocations || stroke.frozenStartLocations)
        && stroke.intent.straightRampAlgorithm != TerrainStraightRampAlgorithm::Independent) return false;
    if (stroke.frozenDoodads || stroke.frozenUnits || stroke.frozenStartLocations
        || unsigned(stroke.ordinaryDoodads.has_value()) + unsigned(stroke.units.has_value())
            + unsigned(stroke.startLocations.has_value()) > 1) return false;
    if (stroke.startLocations)
        return IsTerrainDoodadsJournalPlaceholder(stroke.intent) && stroke.rulesSha256.empty()
            && !stroke.cliffEnforceWaterLimits && ValidateTerrainStartLocationsJournalEdit(*stroke.startLocations).Succeeded();
    if (stroke.ordinaryDoodads)
        return IsTerrainDoodadsJournalPlaceholder(stroke.intent) && stroke.rulesSha256.empty()
            && !stroke.cliffEnforceWaterLimits && ValidateTerrainDoodadsJournalEdit(*stroke.ordinaryDoodads).Succeeded();
    if (stroke.units)
        return IsTerrainDoodadsJournalPlaceholder(stroke.intent) && stroke.rulesSha256.empty()
            && !stroke.cliffEnforceWaterLimits && ValidateTerrainUnitsJournalEdit(*stroke.units).Succeeded();
    const auto& intent = stroke.intent;
    const auto& brush = intent.brush;
    if (intent.resize)
        return Neutral(brush) && intent.points.empty() && !intent.followDoodads && !intent.waterBrush
            && !intent.bounds && !intent.palette && !intent.blockPaste && !stroke.cliffEnforceWaterLimits
            && Sha(stroke.rulesSha256) && ValidateTerrainResizeIntent(*intent.resize).Succeeded();
    if (intent.blockPaste)
        return Neutral(brush) && intent.points.empty() && !intent.waterBrush && !intent.bounds && !intent.palette
            && !stroke.cliffEnforceWaterLimits && Sha(stroke.rulesSha256)
            && ValidateTerrainBlockPasteIntent(*intent.blockPaste).Succeeded();
    if (stroke.intent.bounds) {
        const auto& bounds = *stroke.intent.bounds; const auto& r = bounds.playable;
        return Neutral(brush) && intent.points.empty() && !intent.followDoodads && !intent.waterBrush && !intent.palette
            && !stroke.cliffEnforceWaterLimits && Sha(stroke.rulesSha256) && bounds.algorithm == 1
            && r.west < r.east && r.south < r.north && r.east <= 2048 && r.north <= 2048;
    }
    if (intent.palette) {
        const auto& palette = *intent.palette;
        return Neutral(brush) && intent.points.empty() && !intent.followDoodads && !intent.waterBrush
            && !stroke.cliffEnforceWaterLimits && Sha(stroke.rulesSha256)
            && (palette.table == TerrainTextureTable::Ground || palette.table == TerrainTextureTable::Cliff)
            && palette.slot < (palette.table == TerrainTextureTable::Ground ? 16u : 15u)
            && Rawcode(palette.replacementRawcode)
            && ((palette.algorithm == 1 && Rawcode(palette.expectedRawcode)
                    && palette.expectedRawcode != palette.replacementRawcode)
                || (palette.algorithm == 2 && palette.table == TerrainTextureTable::Ground
                    && palette.slot > 0 && palette.expectedRawcode.empty())
                || (palette.algorithm == 3 && palette.table == TerrainTextureTable::Cliff
                    && palette.slot > 0 && palette.expectedRawcode.empty()));
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
    case TerrainBrushKind::PathingRestriction: case TerrainBrushKind::RemoveWater: break;
    case TerrainBrushKind::CornerRamp: case TerrainBrushKind::StraightRamp: break;
    default: return false;
    }
    if (!intent.waterBrush && brush.kind == TerrainBrushKind::CornerRamp)
        return intent.points.size() == 1 && IsCanonicalTerrainCornerRampBrush(brush)
            && (intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::DryOnly
                || intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain
                || intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners
                || intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners)
            && Sha(stroke.rulesSha256) && !stroke.cliffEnforceWaterLimits;
    if (!intent.waterBrush && brush.kind == TerrainBrushKind::StraightRamp)
        return intent.points.size() == 1 && IsCanonicalTerrainStraightRampBrush(brush)
            && (intent.straightRampAlgorithm == TerrainStraightRampAlgorithm::Independent
                || intent.straightRampAlgorithm == TerrainStraightRampAlgorithm::Connected)
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
template<class Journal> bool Valid(const Journal& journal) {
    if (!Sha(journal.sourceSha256) || journal.members.size() != TerrainJournalMemberNames.size()
        || journal.strokes.empty() || journal.strokes.size() > 128
        || journal.baselinePathingLayers.size() > TerrainJournalMaximumBytes - 128 * 1024) return false;
    for (std::size_t i = 0; i < journal.members.size(); ++i) {
        const auto& member = journal.members[i];
        if (member.name != TerrainJournalMemberNames[i] || (i < 2 && !member.present)
            || (member.present ? !Sha(member.sha256) : (!member.sha256.empty() || member.bytes != 0))) return false;
    }
    std::size_t points = 0, available = TerrainJournalMaximumBytes - 128 * 1024 - journal.baselinePathingLayers.size();
    for (const auto& stroke : journal.strokes) {
        if (!ValidStroke(stroke, points)) return false;
        if (stroke.ordinaryDoodads) {
            if (!journal.members[5].present) return false;
            if (stroke.ordinaryDoodads->inserted.size() > available) return false;
            available -= stroke.ordinaryDoodads->inserted.size();
        }
        if (stroke.units) {
            if (!journal.members[4].present || stroke.units->inserted.size() > available) return false;
            available -= stroke.units->inserted.size();
        }
        if (stroke.startLocations) {
            if (!journal.members[4].present || !journal.members[2].present
                || stroke.startLocations->units.inserted.size() > available) return false;
            available -= stroke.startLocations->units.inserted.size();
            if (stroke.startLocations->settings.inserted.size() > available) return false;
            available -= stroke.startLocations->settings.inserted.size();
        }
    }
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

Status detail::ValidateTerrainJournalParts(std::string_view sourceSha256,
    std::span<const TerrainJournalMember> members, std::span<const std::byte> baselinePathingLayers,
    std::span<const TerrainJournalStroke> strokes) {
    struct View {
        std::string_view sourceSha256;
        std::span<const TerrainJournalMember> members;
        std::span<const std::byte> baselinePathingLayers;
        std::span<const TerrainJournalStroke> strokes;
    };
    return Valid(View{sourceSha256, members, baselinePathingLayers, strokes})
        ? Status::Ok() : Status{Failed<TerrainStrokeJournal>().diagnostics};
}

Status ValidateTerrainStrokeJournal(const TerrainStrokeJournal& journal) {
    if (journal.history) return detail::ValidateTerrainHistoryJournal(journal);
    return Valid(journal) ? Status::Ok() : Status{Failed<TerrainStrokeJournal>().diagnostics};
}

Result<std::vector<std::byte>> EncodeTerrainStrokeJournal(const TerrainStrokeJournal& journal) {
    if (journal.history) return detail::EncodeTerrainHistoryJournal(journal);
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
        const bool wetCorners = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp
                && stroke.intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain; });
        const bool nativeCorners = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp
                && stroke.intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners; });
        const bool nativeWetCorners = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp
                && stroke.intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners; });
        const bool bounds = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.bounds.has_value(); });
        const bool palettes = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.palette.has_value(); });
        const bool doodads = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.ordinaryDoodads.has_value(); });
        const bool units = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.units.has_value(); });
        const bool starts = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.startLocations.has_value(); });
        const bool straightRamps = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::StraightRamp; });
        const bool connectedStraights = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::StraightRamp
                && stroke.intent.straightRampAlgorithm == TerrainStraightRampAlgorithm::Connected; });
        const bool blocks = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.blockPaste.has_value(); });
        const bool resizes = std::any_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.resize.has_value(); });
        // FTJE/H/I由保存站history外层占用；含水内角typed正文独立使用FTJJ。
        const unsigned version = nativeWetCorners ? 19 : resizes ? 16 : nativeCorners ? 15 : blocks ? 13 : connectedStraights ? 12 : straightRamps ? 11 : starts ? 10 : units ? 9 : wetCorners ? 8 : doodads ? 7 : palettes ? 6 : bounds ? 5
            : cornerRamps ? 4 : nativeWater ? 3 : nativeCliffs ? 2 : 1;
        wire.Number(0x004a5446u | ((version >= 10 ? std::uint32_t('A') + version - 10 : std::uint32_t('0') + version) << 24), 4);
        wire.Text(journal.sourceSha256);
        wire.Number(journal.members.size(), 1);
        for (const auto& member : journal.members) {
            wire.Text(member.name); wire.Number(member.present, 1); wire.Text(member.sha256); wire.Number(member.bytes, 8);
        }
        wire.Bytes(journal.baselinePathingLayers);
        wire.Number(journal.strokes.size(), 4);
        for (const auto& stroke : journal.strokes) {
            if (version >= 5) {
                wire.Number(stroke.intent.resize ? 7 : stroke.intent.blockPaste ? 6 : stroke.startLocations ? 5 : stroke.units ? 4 : stroke.ordinaryDoodads ? 3 : stroke.intent.palette ? 2 : stroke.intent.bounds ? 1 : 0, 1);
                if (stroke.intent.resize) {
                    const auto& resize = *stroke.intent.resize; const auto& r = resize.canvas;
                    wire.Number(resize.algorithm, 1);
                    for (const auto value : {r.west, r.south, r.east, r.north}) wire.Number(std::bit_cast<std::uint32_t>(value), 4);
                    wire.Text(stroke.rulesSha256); continue;
                }
                if (stroke.intent.blockPaste) {
                    const auto& block = *stroke.intent.blockPaste; const auto& r = block.sourceCells;
                    wire.Number(block.algorithm, 1); wire.Number(r.x0, 4); wire.Number(r.y0, 4);
                    wire.Number(r.x1, 4); wire.Number(r.y1, 4);
                    wire.Number(block.destinationVertex[0], 4); wire.Number(block.destinationVertex[1], 4);
                    wire.Text(block.expectedBlockSha256); wire.Number(stroke.intent.followDoodads, 1);
                    wire.Text(stroke.rulesSha256); continue;
                }
                if (stroke.startLocations) {
                    for (const auto* value : {static_cast<const TerrainPlacementJournalDelta*>(&stroke.startLocations->units), &stroke.startLocations->settings}) {
                        wire.Text(value->beforeSha256); wire.Text(value->afterSha256);
                        wire.Number(value->beforeBytes, 4); wire.Number(value->afterBytes, 4);
                        wire.Number(value->prefixBytes, 4); wire.Number(value->removedBytes, 4); wire.Bytes(value->inserted);
                    }
                    continue;
                }
                if (stroke.ordinaryDoodads || stroke.units) {
                    const TerrainPlacementJournalDelta& value = stroke.units
                        ? static_cast<const TerrainPlacementJournalDelta&>(*stroke.units)
                        : static_cast<const TerrainPlacementJournalDelta&>(*stroke.ordinaryDoodads);
                    wire.Text(value.beforeSha256); wire.Text(value.afterSha256);
                    wire.Number(value.beforeBytes, 4); wire.Number(value.afterBytes, 4);
                    wire.Number(value.prefixBytes, 4); wire.Number(value.removedBytes, 4); wire.Bytes(value.inserted);
                    continue;
                }
                if (stroke.intent.bounds) {
                    const auto& value = *stroke.intent.bounds;
                    wire.Number(value.algorithm, 1); wire.Number(value.playable.west, 4); wire.Number(value.playable.south, 4);
                    wire.Number(value.playable.east, 4); wire.Number(value.playable.north, 4); wire.Text(stroke.rulesSha256);
                    continue;
                }
                if (stroke.intent.palette) {
                    const auto& value = *stroke.intent.palette;
                    wire.Number(value.algorithm, 1); wire.Number(static_cast<unsigned>(value.table), 1);
                    wire.Number(value.slot, 4); wire.Text(value.expectedRawcode); wire.Text(value.replacementRawcode);
                    wire.Text(stroke.rulesSha256); continue;
                }
            }
            const auto& intent = stroke.intent; const auto& brush = intent.brush;
            wire.Number(static_cast<unsigned>(brush.kind), 1);
            if (version >= 8 && brush.kind == TerrainBrushKind::CornerRamp)
                wire.Number(static_cast<unsigned>(intent.cornerRampAlgorithm), 1);
            if (version >= 12 && brush.kind == TerrainBrushKind::StraightRamp)
                wire.Number(static_cast<unsigned>(intent.straightRampAlgorithm), 1);
            wire.Number(static_cast<unsigned>(brush.shape), 1);
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
                if (version >= 3) wire.Number(static_cast<unsigned>(water.algorithm), 1);
            }
            if (version >= 2) {
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
    if (bytes.size() >= 4 && bytes[0] == std::byte{'F'} && bytes[1] == std::byte{'T'}
        && bytes[2] == std::byte{'J'} && (bytes[3] == std::byte{'E'} || bytes[3] == std::byte{'H'} || bytes[3] == std::byte{'I'}))
        return detail::DecodeTerrainHistoryJournal(bytes);
    try {
        Reader reader{bytes}; TerrainStrokeJournal journal;
        const auto magic = reader.Number(4);
        if ((magic & 0x00ffffffu) != 0x004a5446u
            || ((magic >> 24) != 'A' && (magic >> 24) != 'B' && (magic >> 24) != 'C' && (magic >> 24) != 'D' && (magic >> 24) != 'F' && (magic >> 24) != 'G' && (magic >> 24) != 'J'
                && ((magic >> 24) < '1' || (magic >> 24) > '9')))
            return Failed<TerrainStrokeJournal>();
        const auto version = (magic >> 24) >= 'A' ? static_cast<unsigned>((magic >> 24) - 'A') + 10u
            : static_cast<unsigned>((magic >> 24) - '0');
        // 先验证magic，再集中派生wire能力；旧记录不会因新variant而扩域。
        const bool hasCliffOption = version >= 2, hasWaterAlgorithm = version >= 3, hasCorner = version >= 4;
        const bool hasVariants = version >= 5, hasPalette = version >= 6, hasDoodads = version >= 7;
        const bool hasCornerAlgorithm = version >= 8, hasUnits = version >= 9, hasStarts = version >= 10;
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
            if (hasVariants) {
                const auto kind = reader.Number(1);
                if (kind > (version >= 16 ? 7u : version >= 13 ? 6u : hasStarts ? 5u : hasUnits ? 4u : hasDoodads ? 3u : hasPalette ? 2u : 1u)) return Failed<TerrainStrokeJournal>();
                if (kind == 7) {
                    TerrainResizeIntent resize; resize.algorithm = static_cast<std::uint8_t>(reader.Number(1));
                    resize.canvas.west = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Number(4)));
                    resize.canvas.south = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Number(4)));
                    resize.canvas.east = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Number(4)));
                    resize.canvas.north = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Number(4)));
                    intent.resize = resize; stroke.rulesSha256 = reader.Text(64);
                    journal.strokes.push_back(std::move(stroke)); continue;
                }
                if (kind == 6) {
                    TerrainBlockPasteIntent block; block.algorithm = static_cast<std::uint8_t>(reader.Number(1));
                    block.sourceCells.x0 = static_cast<std::uint32_t>(reader.Number(4));
                    block.sourceCells.y0 = static_cast<std::uint32_t>(reader.Number(4));
                    block.sourceCells.x1 = static_cast<std::uint32_t>(reader.Number(4));
                    block.sourceCells.y1 = static_cast<std::uint32_t>(reader.Number(4));
                    block.destinationVertex[0] = static_cast<std::uint32_t>(reader.Number(4));
                    block.destinationVertex[1] = static_cast<std::uint32_t>(reader.Number(4));
                    block.expectedBlockSha256 = reader.Text(64); intent.followDoodads = reader.Bool();
                    intent.blockPaste = std::move(block); stroke.rulesSha256 = reader.Text(64);
                    journal.strokes.push_back(std::move(stroke)); continue;
                }
                if (kind == 5) {
                    TerrainStartLocationsJournalEdit edit;
                    for (auto* value : {static_cast<TerrainPlacementJournalDelta*>(&edit.units), &edit.settings}) {
                        value->beforeSha256 = reader.Text(64); value->afterSha256 = reader.Text(64);
                        value->beforeBytes = static_cast<std::uint32_t>(reader.Number(4)); value->afterBytes = static_cast<std::uint32_t>(reader.Number(4));
                        value->prefixBytes = static_cast<std::uint32_t>(reader.Number(4)); value->removedBytes = static_cast<std::uint32_t>(reader.Number(4));
                        const auto inserted = reader.Bytes(TerrainJournalMaximumBytes);
                        value->inserted.assign(inserted.begin(), inserted.end());
                    }
                    stroke.startLocations = std::move(edit);
                    journal.strokes.push_back(std::move(stroke)); continue;
                }
                if (kind == 3 || kind == 4) {
                    TerrainPlacementJournalDelta edit;
                    edit.beforeSha256 = reader.Text(64); edit.afterSha256 = reader.Text(64);
                    edit.beforeBytes = static_cast<std::uint32_t>(reader.Number(4)); edit.afterBytes = static_cast<std::uint32_t>(reader.Number(4));
                    edit.prefixBytes = static_cast<std::uint32_t>(reader.Number(4)); edit.removedBytes = static_cast<std::uint32_t>(reader.Number(4));
                    const auto inserted = reader.Bytes(TerrainJournalMaximumBytes);
                    edit.inserted.assign(inserted.begin(), inserted.end());
                    if (kind == 3) {
                        TerrainDoodadsJournalEdit value;
                        static_cast<TerrainPlacementJournalDelta&>(value) = std::move(edit);
                        stroke.ordinaryDoodads = std::move(value);
                    } else {
                        TerrainUnitsJournalEdit value;
                        static_cast<TerrainPlacementJournalDelta&>(value) = std::move(edit);
                        stroke.units = std::move(value);
                    }
                    journal.strokes.push_back(std::move(stroke)); continue;
                }
                if (kind == 1) {
                    TerrainBoundsEditIntent bounds; bounds.algorithm = static_cast<std::uint8_t>(reader.Number(1));
                    bounds.playable.west = static_cast<std::uint32_t>(reader.Number(4));
                    bounds.playable.south = static_cast<std::uint32_t>(reader.Number(4));
                    bounds.playable.east = static_cast<std::uint32_t>(reader.Number(4));
                    bounds.playable.north = static_cast<std::uint32_t>(reader.Number(4));
                    stroke.intent.bounds = bounds; stroke.rulesSha256 = reader.Text(64);
                    journal.strokes.push_back(std::move(stroke)); continue;
                }
                if (kind == 2) {
                    TerrainPaletteEditIntent palette; palette.algorithm = static_cast<std::uint8_t>(reader.Number(1));
                    palette.table = static_cast<TerrainTextureTable>(reader.Number(1));
                    palette.slot = static_cast<std::uint32_t>(reader.Number(4));
                    palette.expectedRawcode = reader.Text(4); palette.replacementRawcode = reader.Text(4);
                    intent.palette = std::move(palette); stroke.rulesSha256 = reader.Text(64);
                    journal.strokes.push_back(std::move(stroke)); continue;
                }
            }
            brush.kind = static_cast<TerrainBrushKind>(reader.Number(1));
            if (brush.kind == TerrainBrushKind::StraightRamp && version < 11)
                return Failed<TerrainStrokeJournal>();
            if (brush.kind == TerrainBrushKind::StraightRamp)
                intent.straightRampAlgorithm = version >= 12
                    ? static_cast<TerrainStraightRampAlgorithm>(reader.Number(1)) : TerrainStraightRampAlgorithm::Independent;
            if (brush.kind == TerrainBrushKind::CornerRamp) {
                if (!hasCorner)
                    return Failed<TerrainStrokeJournal>();
                // 旧记录不含版本字节，永久保留原干地接受域。
                intent.cornerRampAlgorithm = hasCornerAlgorithm
                    ? static_cast<TerrainCornerRampAlgorithm>(reader.Number(1)) : TerrainCornerRampAlgorithm::DryOnly;
                if (intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners && version < 15)
                    return Failed<TerrainStrokeJournal>();
                if (intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners && version < 19)
                    return Failed<TerrainStrokeJournal>();
            }
            brush.shape = static_cast<TerrainBrushShape>(reader.Number(1));
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
                water.algorithm = hasWaterAlgorithm ? static_cast<TerrainWaterAlgorithm>(reader.Number(1))
                    : TerrainWaterAlgorithm::LegacyFlat;
                intent.waterBrush = water;
            }
            if (hasCliffOption && reader.Bool()) stroke.cliffEnforceWaterLimits = reader.Bool();
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
            || (version == 2 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
                [](const auto& stroke) { return stroke.cliffEnforceWaterLimits.has_value(); })))
            return Failed<TerrainStrokeJournal>();
        if (version == 3 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.waterBrush
                && stroke.intent.waterBrush->algorithm == TerrainWaterAlgorithm::NativeRamps; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 4 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 5 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.bounds.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        if (version == 6 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.palette.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        if (version == 7 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.ordinaryDoodads.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        if (version == 8 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp
                && stroke.intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::WetTerrain; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 9 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.units.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        if (version == 10 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.startLocations.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        if (version == 11 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::StraightRamp; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 12 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::StraightRamp
                && stroke.intent.straightRampAlgorithm == TerrainStraightRampAlgorithm::Connected; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 13 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.blockPaste.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        if (version == 15 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp
                && stroke.intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeCorners; }))
            return Failed<TerrainStrokeJournal>();
        if (version == 16 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.resize.has_value(); }))
            return Failed<TerrainStrokeJournal>();
        if (version == 19 && std::none_of(journal.strokes.begin(), journal.strokes.end(),
            [](const auto& stroke) { return stroke.intent.brush.kind == TerrainBrushKind::CornerRamp
                && stroke.intent.cornerRampAlgorithm == TerrainCornerRampAlgorithm::NativeWetCorners; }))
            return Failed<TerrainStrokeJournal>();
        return Result<TerrainStrokeJournal>::Success(std::move(journal));
    } catch (const std::bad_alloc&) { return Failed<TerrainStrokeJournal>(); }
      catch (const std::length_error&) { return Failed<TerrainStrokeJournal>(); }
      catch (const std::invalid_argument&) { return Failed<TerrainStrokeJournal>(); }
}

} // namespace War3Map::MapData
