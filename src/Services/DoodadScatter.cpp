#include "War3MapCore/Services/DoodadScatter.h"
#include "War3MapCore/Services/PlacementCreation.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace War3Map::Services {
namespace {
constexpr std::string_view kDomain = "Placement.Scatter";
constexpr std::uint32_t kMaxPerDab = 4096;
constexpr std::uint32_t kAttemptsPerItem = 8;

bool Finite(double value) { return std::isfinite(value); }

char Fold(char c) {
    if (c == '/') return '\\';
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// 只含 * 与 ? 的通配；回溯指针写法，最坏 O(n·m)。
bool Glob(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || Fold(pattern[p]) == Fold(text[t]))) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
} // namespace

std::uint64_t ScatterRandom::Next() noexcept {
    std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

double ScatterRandom::Uniform() noexcept { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }

std::uint64_t ScatterSpacingIndex::Key(std::int64_t cx, std::int64_t cy) const noexcept {
    return (static_cast<std::uint64_t>(cx) << 32) ^ (static_cast<std::uint64_t>(cy) & 0xffffffffull);
}

void ScatterSpacingIndex::Add(double x, double y) {
    if (!(spacing_ > 0)) return;
    cells_[Key(static_cast<std::int64_t>(std::floor(x / spacing_)), static_cast<std::int64_t>(std::floor(y / spacing_)))].push_back({x, y});
}

bool ScatterSpacingIndex::Free(double x, double y) const {
    if (!(spacing_ > 0)) return true;
    const auto cx = static_cast<std::int64_t>(std::floor(x / spacing_)), cy = static_cast<std::int64_t>(std::floor(y / spacing_));
    const double limit = spacing_ * spacing_;
    for (std::int64_t dx = -1; dx <= 1; ++dx) for (std::int64_t dy = -1; dy <= 1; ++dy) {
        const auto found = cells_.find(Key(cx + dx, cy + dy));
        if (found == cells_.end()) continue;
        for (const auto& point : found->second) {
            const double ex = point[0] - x, ey = point[1] - y;
            if (ex * ex + ey * ey < limit) return false;
        }
    }
    return true;
}

Status ValidateScatter(const ScatterBrushSettings& s, std::span<const ScatterPaletteType> palette) {
    const auto fail = [](std::string code, std::string message) { return Status::Failure(std::string(kDomain), std::move(code), std::move(message)); };
    if (!Finite(s.radius) || s.radius <= 0 || s.radius > 8192) return fail("SCATTER_RADIUS", "笔刷半径须在 0 到 8192 游戏单位之间。");
    if (s.densityMode == ScatterDensityMode::PerArea) {
        if (!Finite(s.perTenThousandSquareUnits) || s.perTenThousandSquareUnits <= 0 || s.perTenThousandSquareUnits > 10000)
            return fail("SCATTER_DENSITY", "密度须为正数，且不超过每 100×100 游戏单位 10000 个。");
    } else if (s.countPerDab == 0 || s.countPerDab > kMaxPerDab)
        return fail("SCATTER_DENSITY", "每笔个数须在 1 到 4096 之间。");
    if (!Finite(s.yawMinDegrees) || !Finite(s.yawMaxDegrees) || s.yawMinDegrees > s.yawMaxDegrees || s.yawMaxDegrees - s.yawMinDegrees > 360)
        return fail("SCATTER_YAW", "朝向范围无效：最小不能大于最大，跨度不超过 360°。");
    if (!Finite(s.scaleMin) || !Finite(s.scaleMax) || s.scaleMin <= 0 || s.scaleMin > s.scaleMax || s.scaleMax > 100)
        return fail("SCATTER_SCALE", "缩放范围须为正数、最小不大于最大、不超过 100。");
    if (!Finite(s.minSpacing) || s.minSpacing < 0 || s.minSpacing > 8192) return fail("SCATTER_SPACING", "最小间距须在 0 到 8192 游戏单位之间。");
    if (!Finite(s.slopeMinDegrees) || !Finite(s.slopeMaxDegrees) || s.slopeMinDegrees < 0 || s.slopeMaxDegrees > 90 || s.slopeMinDegrees > s.slopeMaxDegrees)
        return fail("SCATTER_SLOPE", "坡度范围须在 0°–90° 之间，最小不大于最大。");
    if (palette.empty()) return fail("SCATTER_PALETTE_EMPTY", "调色板里没有装饰物类型。");
    for (const auto& type : palette) {
        if (type.rawcode.size() != 4) return fail("SCATTER_PALETTE_TYPE", "调色板类型须是四字符物体代码：" + type.rawcode);
        if (!Finite(type.weight) || type.weight <= 0) return fail("SCATTER_PALETTE_WEIGHT", "调色板权重须为正数：" + type.rawcode);
        if (type.variations == 0) return fail("SCATTER_PALETTE_VARIATION", "变体数至少为 1：" + type.rawcode);
    }
    return Status::Ok();
}

Result<ScatterDabReport> ScatterDab(double centerX, double centerY, const ScatterBrushSettings& s,
    std::span<const ScatterPaletteType> palette, const ScatterSurfaceProbe& probe, ScatterSpacingIndex& spacing, ScatterRandom& random) {
    if (const auto valid = ValidateScatter(s, palette); !valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    if (!Finite(centerX) || !Finite(centerY)) return Result<ScatterDabReport>::Failure(std::string(kDomain), "SCATTER_CENTER", "笔刷中心不是有限坐标。");
    if (!probe) return Result<ScatterDabReport>::Failure(std::string(kDomain), "SCATTER_SURFACE", "没有可落点的表面。");
    ScatterDabReport report;
    std::uint32_t count = s.countPerDab;
    if (s.densityMode == ScatterDensityMode::PerArea) {
        // 期望个数取整数部分，小数部分按概率补一个：反复落笔的平均密度与设置一致。
        const double expected = s.perTenThousandSquareUnits * std::numbers::pi * s.radius * s.radius / 10000.0;
        const double whole = std::floor(expected);
        count = static_cast<std::uint32_t>(std::min<double>(kMaxPerDab, whole + (random.Uniform() < expected - whole ? 1 : 0)));
    }
    report.requested = count;
    double total = 0;
    for (const auto& type : palette) total += type.weight;
    const double cosMin = std::cos(s.slopeMinDegrees * std::numbers::pi / 180.0);
    const double cosMax = std::cos(s.slopeMaxDegrees * std::numbers::pi / 180.0);
    for (std::uint32_t item = 0; item < count; ++item) {
        bool placed = false;
        std::uint32_t lastReason = 0; // 1 无表面 2 坡度 3 间距
        for (std::uint32_t attempt = 0; attempt < kAttemptsPerItem && !placed; ++attempt) {
            const double r = s.radius * std::sqrt(random.Uniform()), angle = 2 * std::numbers::pi * random.Uniform();
            const double x = centerX + r * std::cos(angle), y = centerY + r * std::sin(angle);
            if (!spacing.Free(x, y)) { lastReason = 3; continue; }
            const auto surface = probe(x, y);
            if (!surface || !Finite(surface->position[2])) { lastReason = 1; continue; }
            // 坡度 = 法线与竖直方向的夹角；nz 越小坡越陡。
            const double nz = std::clamp(surface->normal[2], -1.0, 1.0);
            if (nz > cosMin + 1e-9 || nz < cosMax - 1e-9) { lastReason = 2; continue; }
            double pick = random.Uniform() * total;
            const ScatterPaletteType* type = &palette.back();
            for (const auto& candidate : palette) {
                if (pick < candidate.weight) { type = &candidate; break; }
                pick -= candidate.weight;
            }
            ScatterCandidate candidate;
            candidate.rawcode = type->rawcode;
            candidate.variation = s.randomVariation && type->variations > 1
                ? std::min(type->variations - 1, static_cast<std::uint32_t>(random.Uniform() * type->variations)) : 0;
            candidate.position = {x, y, surface->position[2]};
            const double yaw = s.yawMinDegrees + (s.yawMaxDegrees - s.yawMinDegrees) * random.Uniform();
            candidate.yawRadians = std::fmod(yaw, 360.0) * std::numbers::pi / 180.0;
            if (candidate.yawRadians < 0) candidate.yawRadians += 2 * std::numbers::pi;
            candidate.scale = s.scaleMin + (s.scaleMax - s.scaleMin) * random.Uniform();
            spacing.Add(x, y);
            report.placed.push_back(std::move(candidate));
            placed = true;
        }
        if (!placed) {
            if (lastReason == 1) ++report.noSurface;
            else if (lastReason == 2) ++report.slope;
            else ++report.spacing;
        }
    }
    return Result<ScatterDabReport>::Success(std::move(report));
}

std::vector<std::uint32_t> SelectScatterErase(double centerX, double centerY, double radius,
    std::span<const ScatterPaletteType> palette, std::span<const MapData::DoodadPlacement> doodads) {
    std::vector<std::uint32_t> selected;
    if (!Finite(centerX) || !Finite(centerY) || !(radius > 0)) return selected;
    const double limit = radius * radius;
    for (std::uint32_t i = 0; i < doodads.size(); ++i) {
        const auto& doodad = doodads[i];
        if (std::none_of(palette.begin(), palette.end(), [&](const auto& type) { return type.rawcode == doodad.rawcode; })) continue;
        const double dx = doodad.position[0] - centerX, dy = doodad.position[1] - centerY;
        if (dx * dx + dy * dy <= limit) selected.push_back(i);
    }
    return selected;
}

MapData::DoodadPlacement MakeScatterDoodad(const ScatterCandidate& candidate) {
    auto record = std::get<MapData::DoodadPlacement>(MakeDefaultPlacement(ObjectData::ObjectType::Doodad, candidate.rawcode, 0,
        {static_cast<float>(candidate.position[0]), static_cast<float>(candidate.position[1]), static_cast<float>(candidate.position[2])}));
    record.rotation = static_cast<float>(candidate.yawRadians);
    const auto scale = static_cast<float>(candidate.scale);
    record.scale = {scale, scale, scale};
    record.variation = static_cast<std::int32_t>(candidate.variation);
    record.flags = static_cast<std::uint8_t>(record.flags | MapData::kDoodadFlagFixedZ);
    return record;
}

bool ScatterSurfaceFilterMatches(std::string_view patterns, std::string_view rawcode, std::string_view modelPath) {
    bool any = false;
    std::size_t start = 0;
    while (start <= patterns.size()) {
        auto end = patterns.find_first_of("; \t\r\n", start);
        if (end == std::string_view::npos) end = patterns.size();
        const auto pattern = patterns.substr(start, end - start);
        if (!pattern.empty()) {
            any = true;
            if (Glob(pattern, rawcode) || Glob(pattern, modelPath)) return true;
        }
        start = end + 1;
    }
    return !any;
}

double ScatterDensityFromSquareMetres(double perHundredSquareMetres, double unitsPerMetre) {
    // 100 平方米 = 100·u² 平方游戏单位；换到每 10000 平方游戏单位。
    return unitsPerMetre > 0 ? perHundredSquareMetres * 10000.0 / (100.0 * unitsPerMetre * unitsPerMetre) : 0;
}

double ScatterDensityToSquareMetres(double perTenThousandSquareUnits, double unitsPerMetre) {
    return perTenThousandSquareUnits * 100.0 * unitsPerMetre * unitsPerMetre / 10000.0;
}

} // namespace War3Map::Services
