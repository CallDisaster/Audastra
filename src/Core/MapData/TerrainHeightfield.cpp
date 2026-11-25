#include "War3MapCore/MapData/TerrainHeightfield.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../Serialization/StrictJson.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr std::string_view kDomain = "Terrain.Heightfield";
constexpr std::string_view kPayloadTag = "Audastra.TerrainHeightfield.v1";
constexpr std::size_t kClampSamples = 32;

struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(std::string(kDomain), code, std::move(message)).diagnostics};
}
void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
template<class T, class Function> Result<T> Guard(Function function) {
    try { return Result<T>::Success(function()); }
    catch (Failure& failed) { return {{}, std::move(failed.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Result<T>::Failure(std::string(kDomain), "TERRAIN_HEIGHTFIELD_MEMORY", "高度场导入内存不足，地图未修改。");
    } catch (const std::length_error&) {
        return Result<T>::Failure(std::string(kDomain), "TERRAIN_HEIGHTFIELD_MEMORY", "高度场导入容量超限，地图未修改。");
    }
}
Status GuardStatus(const auto& function) {
    try { function(); return Status::Ok(); }
    catch (Failure& failed) { return {std::move(failed.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Status::Failure(std::string(kDomain), "TERRAIN_HEIGHTFIELD_MEMORY", "高度场导入内存不足，地图未修改。");
    }
}

// 整数值的 JSON 数字（允许写成 -16384.0）；超出 int64 或带小数时返回空。
std::optional<std::int64_t> Integral(const nlohmann::json& value) {
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (!value.is_number_float()) return std::nullopt;
    const auto number = value.get<double>();
    if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > 9.0e15) return std::nullopt;
    return static_cast<std::int64_t>(number);
}

std::string PayloadSha256(std::uint8_t algorithm, std::uint8_t cliffLevel, std::uint32_t width, std::uint32_t height,
    std::span<const std::uint16_t> groundRaw) {
    std::vector<std::byte> wire;
    wire.reserve(kPayloadTag.size() + 10 + groundRaw.size() * 2);
    for (const auto c : kPayloadTag) wire.push_back(std::byte(c));
    const auto put = [&](std::uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) wire.push_back(std::byte((value >> (8 * i)) & 255));
    };
    put(algorithm, 1); put(cliffLevel, 1); put(width, 4); put(height, 4);
    for (const auto value : groundRaw) put(value, 2);
    return Take(Sha256(wire));
}

bool LowerSha(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void CheckLimits(const TerrainWaterBrushLimits& limits) {
    const TerrainWaterBrushLimits hard;
    if (!limits.maximumCandidateBytes || limits.maximumCandidateBytes > hard.maximumCandidateBytes
        || !limits.maximumWork || limits.maximumWork > hard.maximumWork)
        Fail("TERRAIN_HEIGHTFIELD_LIMITS", "高度场导入预算只能收紧硬上限，不能为零。");
}
} // namespace

Result<TerrainHeightfieldSource> ParseTerrainHeightfieldManifest(std::span<const std::byte> json) {
    return Guard<TerrainHeightfieldSource>([&] {
        Serialization::StrictJsonLimits limits;
        limits.maxBytes = TerrainHeightfieldManifestMaximumBytes; limits.maxDepth = 4; limits.maxNodes = 64;
        limits.maxStringBytes = 512;
        auto parsed = Serialization::ParseStrictJson(json, limits);
        if (!parsed.Succeeded() || !parsed.value) throw Failure{std::move(parsed.diagnostics)};
        const auto& root = *parsed.value;
        if (!root.is_object()) Fail("TERRAIN_HEIGHTFIELD_MANIFEST", "高度场清单必须是 JSON 对象。");
        for (const auto& [key, value] : root.items()) {
            (void)value;
            if (key != "format" && key != "cellSize" && key != "columns" && key != "rows" && key != "originWorld"
                && key != "fillHeight" && key != "data")
                Fail("TERRAIN_HEIGHTFIELD_MANIFEST", "高度场清单含未知字段：" + key);
        }
        const auto format = root.find("format");
        if (format == root.end() || !format->is_string() || format->get<std::string>() != TerrainHeightfieldFormatV1)
            Fail("TERRAIN_HEIGHTFIELD_FORMAT", "高度场清单的 format 必须是 " + std::string(TerrainHeightfieldFormatV1) + "。");
        const auto number = [&](const char* key) -> const nlohmann::json& {
            const auto found = root.find(key);
            if (found == root.end()) Fail("TERRAIN_HEIGHTFIELD_MANIFEST", std::string("高度场清单缺少 ") + key + "。");
            return *found;
        };
        const auto cell = Integral(number("cellSize"));
        if (!cell || *cell != 128) Fail("TERRAIN_HEIGHTFIELD_CELL_SIZE", "高度场 cellSize 只接受 128。");
        TerrainHeightfieldSource source;
        const auto columns = Integral(number("columns")), rows = Integral(number("rows"));
        if (!columns || !rows || *columns < 2 || *rows < 2 || *columns > TerrainHeightfieldMaximumSide
            || *rows > TerrainHeightfieldMaximumSide)
            Fail("TERRAIN_HEIGHTFIELD_SIZE", "高度场 columns/rows 须为 2.." + std::to_string(TerrainHeightfieldMaximumSide) + " 的整数。");
        source.columns = std::uint32_t(*columns); source.rows = std::uint32_t(*rows);
        const auto& origin = number("originWorld");
        if (!origin.is_array() || origin.size() != 2)
            Fail("TERRAIN_HEIGHTFIELD_ORIGIN", "高度场 originWorld 必须是两个数的数组。");
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const auto value = Integral(origin[axis]);
            if (!value || *value % 128 != 0 || *value < -1048576 || *value > 1048576)
                Fail("TERRAIN_HEIGHTFIELD_ORIGIN", "高度场 originWorld 须为 128 的整数倍。");
            source.originWorld[axis] = std::int32_t(*value);
        }
        const auto& fill = number("fillHeight");
        if (!fill.is_number() || !std::isfinite(fill.get<double>()) || std::abs(fill.get<double>()) > 1.0e6)
            Fail("TERRAIN_HEIGHTFIELD_FILL", "高度场 fillHeight 必须是有限数值。");
        source.fillHeight = fill.get<float>();
        source.dataPath = "heightfield.f32";
        if (const auto data = root.find("data"); data != root.end()) {
            if (!data->is_string() || !IO::IsSafeManifestRelativePath(data->get<std::string>()))
                Fail("TERRAIN_HEIGHTFIELD_DATA_PATH", "高度场 data 必须是清单目录内的相对路径，不能是绝对路径或含“..”。");
            source.dataPath = data->get<std::string>();
        }
        return source;
    });
}

Status LoadTerrainHeightfieldSamples(TerrainHeightfieldSource& source, std::span<const std::byte> f32) {
    return GuardStatus([&] {
        if (source.columns < 2 || source.rows < 2 || source.columns > TerrainHeightfieldMaximumSide
            || source.rows > TerrainHeightfieldMaximumSide)
            Fail("TERRAIN_HEIGHTFIELD_SIZE", "高度场尺寸无效。");
        const auto count = std::size_t(source.columns) * source.rows;
        if (f32.size() != count * 4)
            Fail("TERRAIN_HEIGHTFIELD_DATA_SIZE", "高度场数据应为 " + std::to_string(count * 4) + " 字节（"
                + std::to_string(source.columns) + "×" + std::to_string(source.rows) + " 个 float32），实际 "
                + std::to_string(f32.size()) + " 字节。");
        std::vector<float> heights(count);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t bits = 0;
            for (unsigned b = 0; b < 4; ++b) bits |= std::uint32_t(std::to_integer<unsigned>(f32[i * 4 + b])) << (8 * b);
            const auto value = std::bit_cast<float>(bits);
            if (!std::isfinite(value))
                Fail("TERRAIN_HEIGHTFIELD_DATA_VALUE", "高度场第 " + std::to_string(i / source.columns) + " 行第 "
                    + std::to_string(i % source.columns) + " 列不是有限数值。");
            heights[i] = value;
        }
        source.heights = std::move(heights);
    });
}

Result<TerrainHeightfieldSource> ReadTerrainHeightfield(const std::filesystem::path& manifest) {
    auto json = IO::ReadFileBytes(manifest, TerrainHeightfieldManifestMaximumBytes);
    if (!json.Succeeded() || !json.value) return {{}, std::move(json.diagnostics)};
    auto source = ParseTerrainHeightfieldManifest(*json.value);
    if (!source.Succeeded() || !source.value) return source;
    const auto dataPath = manifest.parent_path() / IO::PathFromUtf8(source.value->dataPath);
    const auto maximum = std::uint64_t(source.value->columns) * source.value->rows * 4;
    auto data = IO::ReadFileBytes(dataPath, maximum + 1);
    if (!data.Succeeded() || !data.value) return {{}, std::move(data.diagnostics)};
    const auto loaded = LoadTerrainHeightfieldSamples(*source.value, *data.value);
    if (!loaded.Succeeded()) return {{}, loaded.diagnostics};
    return source;
}

Result<TerrainHeightfieldConversion> ConvertTerrainHeightfield(const TerrainHeader& header,
    const TerrainHeightfieldSource& source, const TerrainHeightfieldOptions& options) {
    return Guard<TerrainHeightfieldConversion>([&] {
        if (header.width < 2 || header.height < 2 || header.width > TerrainHeightfieldMaximumSide
            || header.height > TerrainHeightfieldMaximumSide)
            Fail("TERRAIN_HEIGHTFIELD_MAP_LAYOUT", "地图地形网格尺寸无效。");
        if (source.heights.size() != std::size_t(source.columns) * source.rows || source.columns < 2 || source.rows < 2)
            Fail("TERRAIN_HEIGHTFIELD_SIZE", "高度场数据尚未完整装入。");
        if (options.cliffLevel > 14) Fail("TERRAIN_HEIGHTFIELD_CLIFF_LEVEL", "悬崖层须为 0..14。");
        if (!std::isfinite(options.sink) || std::abs(options.sink) > 65536.0f)
            Fail("TERRAIN_HEIGHTFIELD_SINK", "下沉量必须是有限数值。");
        std::array<std::int64_t, 2> shift{};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const double offset = header.offset[axis];
            const double delta = double(source.originWorld[axis]) - offset;
            if (!std::isfinite(offset) || std::fmod(delta, 128.0) != 0)
                Fail("TERRAIN_HEIGHTFIELD_ALIGNMENT", "高度场原点 (" + std::to_string(source.originWorld[0]) + ", "
                    + std::to_string(source.originWorld[1]) + ") 不在地图地形网格上（地图西南角 "
                    + std::to_string(header.offset[0]) + ", " + std::to_string(header.offset[1])
                    + "，格距 128）；不做重采样。");
            shift[axis] = std::int64_t(delta / 128.0);
        }
        TerrainHeightfieldConversion output;
        auto& intent = output.intent; auto& report = output.report;
        intent.cliffLevel = options.cliffLevel; intent.width = header.width; intent.height = header.height;
        intent.groundRaw.resize(std::size_t(header.width) * header.height);
        const double base = 128.0 * (double(options.cliffLevel) - 2.0);
        report.storableMinimum = float((0.0 - 8192.0) / 4.0 + base);
        report.storableMaximum = float((16383.0 - 8192.0) / 4.0 + base);
        report.minimumHeight = std::numeric_limits<float>::infinity();
        report.maximumHeight = -std::numeric_limits<float>::infinity();
        bool covered = false, clamped = false;
        for (std::uint32_t y = 0; y < header.height; ++y) for (std::uint32_t x = 0; x < header.width; ++x) {
            const auto column = std::int64_t(x) - shift[0], row = std::int64_t(y) - shift[1];
            const bool inside = column >= 0 && row >= 0 && column < source.columns && row < source.rows;
            const double input = inside ? double(source.heights[std::size_t(row) * source.columns + std::size_t(column)])
                : double(source.fillHeight);
            const double height = input - double(options.sink);
            report.minimumHeight = std::min(report.minimumHeight, float(height));
            report.maximumHeight = std::max(report.maximumHeight, float(height));
            if (inside) {
                ++report.importedVertices;
                if (!covered) report.coveredBounds = {x, y, x, y};
                else {
                    auto& b = report.coveredBounds;
                    b[0] = std::min(b[0], x); b[1] = std::min(b[1], y); b[2] = std::max(b[2], x); b[3] = std::max(b[3], y);
                }
                covered = true;
            } else ++report.filledVertices;
            const double scaled = std::nearbyint((height - base) * 4.0) + 8192.0;
            std::uint16_t raw = 0;
            if (scaled < 0.0 || scaled > 16383.0) {
                (scaled < 0.0 ? report.clampedLow : report.clampedHigh) += 1;
                raw = scaled < 0.0 ? 0 : 16383;
                if (!clamped) report.clampedBounds = {x, y, x, y};
                else {
                    auto& b = report.clampedBounds;
                    b[0] = std::min(b[0], x); b[1] = std::min(b[1], y); b[2] = std::max(b[2], x); b[3] = std::max(b[3], y);
                }
                clamped = true;
                if (report.clampedSamples.size() < kClampSamples)
                    report.clampedSamples.push_back({float(double(header.offset[0]) + 128.0 * x),
                        float(double(header.offset[1]) + 128.0 * y), float(height)});
            } else raw = std::uint16_t(scaled);
            intent.groundRaw[std::size_t(y) * header.width + x] = raw;
        }
        report.outsideVertices = std::uint64_t(source.columns) * source.rows - report.importedVertices;
        intent.payloadSha256 = PayloadSha256(intent.algorithm, intent.cliffLevel, intent.width, intent.height, intent.groundRaw);
        return output;
    });
}

Status ValidateTerrainHeightfieldIntent(const TerrainHeightfieldIntent& intent) {
    return GuardStatus([&] {
        if (intent.algorithm != 1 || intent.cliffLevel > 14 || intent.width < 2 || intent.height < 2
            || intent.width > TerrainHeightfieldMaximumSide || intent.height > TerrainHeightfieldMaximumSide
            || intent.groundRaw.size() != std::size_t(intent.width) * intent.height || !LowerSha(intent.payloadSha256)
            || std::any_of(intent.groundRaw.begin(), intent.groundRaw.end(), [](std::uint16_t raw) { return raw > 0x3fff; }))
            Fail("TERRAIN_HEIGHTFIELD_INTENT", "高度场载荷需要算法 1、0..14 悬崖层、完整网格的 0..16383 地面值及小写 SHA-256。");
        if (PayloadSha256(intent.algorithm, intent.cliffLevel, intent.width, intent.height, intent.groundRaw) != intent.payloadSha256)
            Fail("TERRAIN_HEIGHTFIELD_INTENT_DIGEST", "高度场载荷与其摘要不一致。");
    });
}

Result<TerrainHeightfieldStrokeCandidate> PrepareTerrainHeightfieldStroke(const TerrainGridSnapshot& source,
    const TerrainWaterLoadedRules& rules, const TerrainHeightfieldIntent& intent,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes, const TerrainWaterBrushLimits& limits) {
    return Guard<TerrainHeightfieldStrokeCandidate>([&] {
        CheckLimits(limits); Check(ValidateTerrainHeightfieldIntent(intent));
        const auto& header = source.header;
        if (header.version != 11 || header.width != intent.width || header.height != intent.height
            || source.vertices.size() != std::size_t(header.width) * header.height
            || (!source.tiles.empty() && source.tiles.size() != std::size_t(header.width - 1) * (header.height - 1)))
            Fail("TERRAIN_HEIGHTFIELD_LAYOUT", "高度场载荷与当前经典 W3E v11 网格尺寸不一致。");
        if (rules.maximumCliffLevel > 14 || intent.cliffLevel > rules.maximumCliffLevel)
            Fail("TERRAIN_HEIGHTFIELD_CLIFF_LEVEL", "悬崖层 " + std::to_string(intent.cliffLevel) + " 超过当前地形规则的最高层 "
                + std::to_string(rules.maximumCliffLevel) + "。");
        std::size_t work = 0;
        const auto spend = [&](std::size_t count) {
            if (count > limits.maximumWork - work) Fail("TERRAIN_HEIGHTFIELD_WORK_BUDGET", "高度场导入超过工作量预算，地图未修改。");
            work += count;
        };
        for (const auto& shape : specialShapes) {
            spend(1 + shape.relativeCliffLevels.size());
            if (shape.hasFootprint && std::any_of(shape.relativeCliffLevels.begin(), shape.relativeCliffLevels.end(),
                    [](std::int8_t level) { return level != 0; }))
                Fail("TERRAIN_HEIGHTFIELD_SPECIAL", "地图上有带高差的特殊地形模型（" + shape.rawcode
                    + "），统一悬崖层会使它失配；请先移除后再导入。");
        }
        std::size_t bytes = sizeof(TerrainHeightfieldStrokeCandidate);
        const auto add = [&](std::size_t count, std::size_t element) {
            if (element && count > (limits.maximumCandidateBytes - bytes) / element)
                Fail("TERRAIN_HEIGHTFIELD_MEMORY_BUDGET", "高度场候选超过字节预算，地图未修改。");
            bytes += count * element;
        };
        add(source.vertices.size(), sizeof(TerrainVertex)); add(source.tiles.size(), sizeof(TerrainTile));
        add(header.groundTilesets.size() + header.cliffTilesets.size(), sizeof(std::string) + 16);
        spend(source.vertices.size() * 4 + source.tiles.size());
        TerrainHeightfieldStrokeCandidate candidate; candidate.terrain = source;
        auto& changed = candidate.result.changed;
        for (std::uint32_t y = 0; y < header.height; ++y) for (std::uint32_t x = 0; x < header.width; ++x) {
            const auto index = std::size_t(y) * header.width + x;
            auto& vertex = candidate.terrain.vertices[index];
            if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw || vertex.UnknownWaterFlags())
                Fail("TERRAIN_HEIGHTFIELD_DOMAIN", "地形顶点 (" + std::to_string(x) + ", " + std::to_string(y)
                    + ") 含未知字段，不能整图导入高度。");
            const auto ground = intent.groundRaw[index];
            const auto cliff = std::uint8_t((vertex.cliffAndLayerRaw & 0xf0) | intent.cliffLevel);
            const auto texture = std::uint8_t(vertex.textureAndFlagsRaw & ~0x10u);
            if (vertex.groundHeightRaw == ground && vertex.cliffAndLayerRaw == cliff && vertex.textureAndFlagsRaw == texture) continue;
            vertex.groundHeightRaw = ground; vertex.cliffAndLayerRaw = cliff; vertex.textureAndFlagsRaw = texture;
            if (changed.Empty()) changed = {x, y, x + 1, y + 1, false};
            else {
                changed.x0 = std::min(changed.x0, x); changed.y0 = std::min(changed.y0, y);
                changed.x1 = std::max(changed.x1, x + 1); changed.y1 = std::max(changed.y1, y + 1);
            }
            ++candidate.result.changedVertices;
        }
        if (!changed.Empty()) {
            spend(std::size_t(changed.x1 - changed.x0) * (changed.y1 - changed.y0) * 2);
            RefreshTerrainDerived(candidate.terrain, changed.x0, changed.y0, changed.x1, changed.y1);
            candidate.result.requiresPathingRebuild = candidate.result.requiresObjectValidityRefresh = true;
        }
        candidate.result.processed = {0, 0, header.width, header.height, false};
        candidate.result.work = work; candidate.peakBytes = bytes;
        return candidate;
    });
}

} // namespace War3Map::MapData
