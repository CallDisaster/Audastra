#include "War3MapCore/MapData/PathingRestrictionEditing.h"
#include "War3MapCore/MapData/MapPathing.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace War3Map::MapData {
namespace {
constexpr std::size_t kWpmHeader = 16;
constexpr std::size_t kDraftHeader = 4 + 64 + 64;
constexpr std::array kDraftMagic{std::byte{'A'}, std::byte{'P'}, std::byte{'R'}, std::byte{1}};

Status CheckPair(const TerrainHeader& terrain, std::span<const std::byte> original,
    std::span<const std::byte> current) {
    const auto parsed = ParseMapPathing(original, &terrain);
    if (!parsed.Succeeded()) return {parsed.diagnostics};
    if (current.size() != original.size() || !std::equal(original.begin(), original.begin() + kWpmHeader, current.begin()))
        return Status::Failure("Pathing.Edit", "PATHING_DRAFT_SHAPE", "通行草稿与源 WPM 的头或长度不符。");
    for (std::size_t i = kWpmHeader; i < original.size(); ++i) {
        const auto before = std::to_integer<std::uint8_t>(original[i]);
        const auto after = std::to_integer<std::uint8_t>(current[i]);
        if (((before ^ after) & ~(kPathingRestrictionBits | 0x20)) != 0
            || (before & kPathingRestrictionBits & ~after) != 0)
            return Status::Failure("Pathing.Edit", "PATHING_DRAFT_SOURCE_BITS", "通行草稿清除了源限制或改动了未支持位。");
    }
    return Status::Ok();
}

Result<std::vector<std::byte>> Header(const TerrainGridSnapshot& terrain, std::span<const std::byte> original) {
    auto sourceTerrain = SerializeMapTerrain(terrain);
    if (!sourceTerrain.Succeeded() || !sourceTerrain.value) return {{}, std::move(sourceTerrain.diagnostics)};
    const auto terrainSha = Sha256(*sourceTerrain.value), pathingSha = Sha256(original);
    if (!terrainSha.Succeeded()) return {{}, terrainSha.diagnostics};
    if (!pathingSha.Succeeded()) return {{}, pathingSha.diagnostics};
    std::vector<std::byte> bytes(kDraftMagic.begin(), kDraftMagic.end());
    for (const auto* sha : {&*terrainSha.value, &*pathingSha.value})
        for (const auto value : *sha) bytes.push_back(static_cast<std::byte>(value));
    return Result<std::vector<std::byte>>::Success(std::move(bytes));
}

std::string HexByte(unsigned value) {
    constexpr char digits[] = "0123456789abcdef";
    return {digits[(value >> 4) & 15], digits[value & 15]};
}

Result<std::vector<std::byte>> DraftMismatch(const TerrainHeader& terrain,
    std::span<const std::byte> expected, std::span<const std::byte> draft) {
    // 唯一编码器给出完整预期，不另写一套 APR 解析或掩码规则；摘要不截断检查。
    const auto common = std::min(expected.size(), draft.size());
    std::size_t first = common, mismatchedBytes = std::max(expected.size(), draft.size()) - common;
    std::size_t mismatchedCells = expected.size() > std::max(common, kDraftHeader)
        ? expected.size() - std::max(common, kDraftHeader) : 0;
    for (std::size_t offset = 0; offset < common; ++offset) {
        if (expected[offset] == draft[offset]) continue;
        if (first == common) first = offset;
        ++mismatchedBytes;
        if (offset >= kDraftHeader) ++mismatchedCells;
    }
    std::string message = "通行草稿声明与源 W3E/WPM 或候选附加位不一致。";
    if (mismatchedBytes != 0) {
        message += " 首个差异：APR 偏移=" + std::to_string(first);
        if (first >= kDraftHeader && first < expected.size()) {
            const auto cell = first - kDraftHeader;
            const auto width = (terrain.width - 1) * 4;
            message += "；格(x=" + std::to_string(cell % width) + ", y=" + std::to_string(cell / width) + ")";
        }
        if (first < common) {
            const auto difference = std::to_integer<unsigned>(expected[first] ^ draft[first]);
            unsigned firstBit = 1;
            while ((difference & firstBit) == 0) firstBit <<= 1;
            message += "；首个差异位=0x" + HexByte(firstBit) + "；差异位掩码=0x" + HexByte(difference);
        } else {
            message += first < expected.size() ? "；缺少声明字节" : "；多余声明字节";
        }
    }
    message += "；不匹配格数=" + std::to_string(mismatchedCells)
        + "；不匹配字节数=" + std::to_string(mismatchedBytes) + "。";
    auto result = Result<std::vector<std::byte>>::Failure("Pathing.Edit", "PATHING_DRAFT_IDENTITY", std::move(message));
    if (mismatchedBytes != 0)
        result.diagnostics.front().origin = OriginSpan{"overlay/editor/pathing-restrictions.bin", first,
            first < draft.size() ? 1u : 0u, {}};
    return result;
}
} // namespace

std::optional<TerrainBrushFootprint> GetPathingRestrictionFootprint(
    const TerrainHeader& header, const TerrainBrush& brush, const TerrainStrokePoint& point) {
    if (header.width < 2 || header.height < 2 || header.width > 2049 || header.height > 2049
        || !std::isfinite(header.offset[0]) || !std::isfinite(header.offset[1])
        || !std::isfinite(point.x) || !std::isfinite(point.y)
        || (brush.size && (*brush.size < 1 || *brush.size > 8)) || (!brush.size && brush.radius > 16)) return {};
    const auto width = (header.width - 1) * 4, height = (header.height - 1) * 4;
    const double u = std::floor((point.x - header.offset[0]) / kPathingCellSize);
    const double v = std::floor((point.y - header.offset[1]) / kPathingCellSize);
    if (u < 0 || v < 0 || u >= width || v >= height) return {};
    const auto x = static_cast<std::uint32_t>(u), y = static_cast<std::uint32_t>(v);
    const auto radius = brush.size ? *brush.size - 1 : brush.radius;
    return TerrainBrushFootprint{header.offset[0] + (u + 0.5) * kPathingCellSize,
        header.offset[1] + (v + 0.5) * kPathingCellSize, (radius + 0.5) * kPathingCellSize,
        x > radius ? x - radius : 0, y > radius ? y - radius : 0,
        std::min(width, x + radius + 1), std::min(height, y + radius + 1)};
}

Result<PathingRestrictionStroke> ApplyPathingRestrictionStroke(
    const TerrainHeader& terrain, std::span<const std::byte> original, std::span<const std::byte> current,
    const TerrainBrush& brush, std::span<const TerrainStrokePoint> points) {
    if (const auto checked = CheckPair(terrain, original, current); !checked.Succeeded()) return {{}, checked.diagnostics};
    std::vector<std::byte> intent(original.size() - kWpmHeader);
    for (std::size_t i = 0; i < intent.size(); ++i)
        intent[i] = (current[kWpmHeader + i] & ~original[kWpmHeader + i]) & std::byte{kPathingRestrictionBits};
    auto result = ApplyPathingRestrictionStroke(terrain, original, current, intent, brush, points);
    if (result.Succeeded() && result.value && result.value->bytes.empty()) result.value->bytes.assign(current.begin(), current.end());
    return result;
}

Result<PathingRestrictionStroke> ApplyPathingRestrictionStroke(
    const TerrainHeader& terrain, std::span<const std::byte> original, std::span<const std::byte> current,
    std::span<const std::byte> intent, const TerrainBrush& brush, std::span<const TerrainStrokePoint> points) {
    using Output = Result<PathingRestrictionStroke>;
    if (brush.kind != TerrainBrushKind::PathingRestriction || brush.pathingRestrictions == 0
        || (brush.pathingRestrictions & ~kPathingRestrictionBits) != 0)
        return Output::Failure("Pathing.Edit", "PATHING_RESTRICTION_FLAGS", "只允许追加或撤回不可走、不可飞、不可建限制。");
    if (brush.shape != TerrainBrushShape::Circle && brush.shape != TerrainBrushShape::Square)
        return Output::Failure("Pathing.Edit", "TERRAIN_BRUSH_SHAPE", "通行笔刷只支持圆形或方形。");
    if (points.empty() || points.size() > 65536)
        return Output::Failure("Pathing.Edit", "TERRAIN_BRUSH_POINTS", "通行笔划需要 1 到 65536 个落点。");
    if (const auto checked = CheckPair(terrain, original, current); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (intent.size() != original.size() - kWpmHeader)
        return Output::Failure("Pathing.Edit", "PATHING_DRAFT_SHAPE", "限制意图层与源 WPM 格数不符。");
    for (std::size_t i = 0; i < intent.size(); ++i) {
        const auto mask = std::to_integer<std::uint8_t>(intent[i]);
        if ((mask & ~kPathingRestrictionBits) != 0
            || ((original[kWpmHeader + i] | intent[i]) & std::byte{kPathingRestrictionBits})
                != (current[kWpmHeader + i] & std::byte{kPathingRestrictionBits}))
            return Output::Failure("Pathing.Edit", "PATHING_DRAFT_IDENTITY", "限制意图层不能解释当前 WPM，未应用笔划。");
    }
    std::vector<TerrainBrushFootprint> footprints;
    footprints.reserve(points.size());
    for (const auto& point : points) {
        const auto footprint = GetPathingRestrictionFootprint(terrain, brush, point);
        if (!footprint) return Output::Failure("Pathing.Edit", "PATHING_BRUSH_COORDINATE", "通行笔刷尺寸或坐标无效，整笔未修改。");
        footprints.push_back(*footprint);
    }
    PathingRestrictionStroke output;
    output.intent.assign(intent.begin(), intent.end());
    const auto width = (terrain.width - 1) * 4;
    const double radius = brush.size ? *brush.size - 1 : brush.radius;
    for (const auto& footprint : footprints) {
        const double cx = (footprint.centerX - terrain.offset[0]) / kPathingCellSize - 0.5;
        const double cy = (footprint.centerY - terrain.offset[1]) / kPathingCellSize - 0.5;
        for (auto y = footprint.y0; y < footprint.y1; ++y) for (auto x = footprint.x0; x < footprint.x1; ++x) {
            const double dx = x - cx, dy = y - cy;
            if (brush.shape == TerrainBrushShape::Circle && dx * dx + dy * dy > radius * radius) continue;
            const auto index = kWpmHeader + static_cast<std::size_t>(y) * width + x;
            const auto before = std::to_integer<std::uint8_t>(output.bytes.empty() ? current[index] : output.bytes[index]);
            const auto cell = index - kWpmHeader;
            const auto oldIntent = std::to_integer<std::uint8_t>(output.intent[cell]);
            const auto nextIntent = static_cast<std::uint8_t>(brush.value ? oldIntent | brush.pathingRestrictions
                : oldIntent & ~brush.pathingRestrictions);
            if (nextIntent == oldIntent) continue;
            output.intent[cell] = static_cast<std::byte>(nextIntent);
            const auto after = static_cast<std::uint8_t>((before & ~kPathingRestrictionBits)
                | (std::to_integer<std::uint8_t>(original[index]) & kPathingRestrictionBits) | nextIntent);
            if (after != before) {
                if (output.bytes.empty()) output.bytes.assign(current.begin(), current.end());
                output.bytes[index] = static_cast<std::byte>(after);
            }
            auto& changed = output.changed;
            const auto vx = x / 4, vy = y / 4;
            if (changed.Empty()) changed = {vx, vy, vx + 2, vy + 2};
            else { changed.x0 = std::min(changed.x0, vx); changed.y0 = std::min(changed.y0, vy);
                changed.x1 = std::max(changed.x1, vx + 2); changed.y1 = std::max(changed.y1, vy + 2); }
        }
    }
    return Output::Success(std::move(output));
}

Result<std::vector<std::byte>> EncodePathingRestrictionDraft(
    const TerrainGridSnapshot& terrain, std::span<const std::byte> original, std::span<const std::byte> current) {
    if (const auto checked = CheckPair(terrain.header, original, current); !checked.Succeeded()) return {{}, checked.diagnostics};
    bool added = false;
    for (std::size_t i = kWpmHeader; i < original.size(); ++i)
        added |= ((std::to_integer<std::uint8_t>(current[i]) & ~std::to_integer<std::uint8_t>(original[i])) & kPathingRestrictionBits) != 0;
    if (!added) return Result<std::vector<std::byte>>::Success({});
    auto bytes = Header(terrain, original);
    if (!bytes.Succeeded() || !bytes.value) return bytes;
    bytes.value->reserve(kDraftHeader + original.size() - kWpmHeader);
    for (std::size_t i = kWpmHeader; i < original.size(); ++i)
        bytes.value->push_back(static_cast<std::byte>((std::to_integer<std::uint8_t>(current[i])
            & ~std::to_integer<std::uint8_t>(original[i])) & kPathingRestrictionBits));
    return bytes;
}

Result<std::vector<std::byte>> StripPathingRestrictionDraft(
    const TerrainGridSnapshot& terrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> draft, std::vector<std::byte>* decodedIntent) {
    using Output = Result<std::vector<std::byte>>;
    auto decoded = DecodePathingRestrictionDraft(terrain, original, current, draft);
    if (!decoded.Succeeded()) return decoded;
    std::vector<std::byte> stripped(current.begin(), current.end());
    for (std::size_t i = kWpmHeader; i < stripped.size(); ++i)
        stripped[i] = (stripped[i] & ~std::byte{kPathingRestrictionBits}) | (original[i] & std::byte{kPathingRestrictionBits});
    if (decodedIntent) decodedIntent->swap(*decoded.value);
    return Output::Success(std::move(stripped));
}

Result<std::vector<std::byte>> EncodePathingRestrictionDraft(
    const TerrainGridSnapshot& terrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> intent) {
    using Output = Result<std::vector<std::byte>>;
    if (const auto checked = CheckPair(terrain.header, original, current); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (intent.size() != original.size() - kWpmHeader)
        return Output::Failure("Pathing.Edit", "PATHING_DRAFT_SHAPE", "限制意图层与源 WPM 格数不符。");
    for (std::size_t i = 0; i < intent.size(); ++i) {
        if ((intent[i] & ~std::byte{kPathingRestrictionBits}) != std::byte{0}
            || ((original[kWpmHeader + i] | intent[i]) & std::byte{kPathingRestrictionBits})
                != (current[kWpmHeader + i] & std::byte{kPathingRestrictionBits}))
            return Output::Failure("Pathing.Edit", "PATHING_DRAFT_IDENTITY", "限制意图层包含未知位或与合成 WPM 不一致。");
    }
    if (std::none_of(intent.begin(), intent.end(), [](std::byte value) { return value != std::byte{0}; })) return Output::Success({});
    auto bytes = Header(terrain, original);
    if (!bytes.Succeeded()) return bytes;
    (*bytes.value)[3] = std::byte{2};
    bytes.value->insert(bytes.value->end(), intent.begin(), intent.end());
    return bytes;
}

Result<std::vector<std::byte>> DecodePathingRestrictionDraft(
    const TerrainGridSnapshot& terrain, std::span<const std::byte> original,
    std::span<const std::byte> current, std::span<const std::byte> draft) {
    using Output = Result<std::vector<std::byte>>;
    if (const auto checked = CheckPair(terrain.header, original, current); !checked.Succeeded()) return {{}, checked.diagnostics};
    if (draft.size() >= 4 && draft[3] == std::byte{2}) {
        auto expected = Header(terrain, original);
        if (!expected.Succeeded()) return expected;
        (*expected.value)[3] = std::byte{2};
        expected.value->resize(kDraftHeader + original.size() - kWpmHeader);
        // 仅来源已含的位可与 A 重叠；来源没有的限制必须由完整声明逐格解释。
        for (std::size_t i = kWpmHeader; i < original.size(); ++i) {
            const auto at = kDraftHeader + i - kWpmHeader;
            const auto declared = at < draft.size() ? draft[at] : std::byte{0};
            (*expected.value)[at] = ((declared & original[i]) | (current[i] & ~original[i])) & std::byte{kPathingRestrictionBits};
        }
        const bool nonempty = std::any_of(expected.value->begin() + kDraftHeader, expected.value->end(), [](std::byte value) { return value != std::byte{0}; });
        if (!nonempty || !std::equal(draft.begin(), draft.end(), expected.value->begin(), expected.value->end()))
            return DraftMismatch(terrain.header, *expected.value, draft);
        return Output::Success(std::vector<std::byte>(expected.value->begin() + kDraftHeader, expected.value->end()));
    }
    auto encoded = EncodePathingRestrictionDraft(terrain, original, current);
    if (!encoded.Succeeded()) return encoded;
    if (!encoded.value || encoded.value->empty() || !std::equal(draft.begin(), draft.end(), encoded.value->begin(), encoded.value->end()))
        return DraftMismatch(terrain.header, encoded.value ? std::span<const std::byte>(*encoded.value) : std::span<const std::byte>{}, draft);
    return Output::Success(std::vector<std::byte>(encoded.value->begin() + kDraftHeader, encoded.value->end()));
}

} // namespace War3Map::MapData
