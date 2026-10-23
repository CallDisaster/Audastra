#include "War3MapCore/MapData/TerrainSpecialDoodadPathing.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.SpecialDoodads";
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
unsigned Channel(const TerrainDoodadFootprint& image, unsigned x, unsigned y, unsigned channel) {
    if (channel == 3 && image.pixelBytes == 3) return 255;
    const auto row = image.topOrigin ? y : image.height - 1 - y;
    return std::to_integer<unsigned>(image.pixels[(std::size_t{row} * image.width + x) * image.pixelBytes + channel]);
}
int Elevation(const TerrainDoodadFootprint& image, unsigned x, unsigned y) {
    const auto alpha = Channel(image, x == 0 ? 0 : x * 4 - 1, image.height - 1 - (y == 0 ? 0 : y * 4 - 1), 3);
    if (alpha <= 10) return 0;
    if (alpha >= 120 && alpha <= 130) return 1;
    if (alpha >= 250) return 2;
    Fail("TERRAIN_SPECIAL_ALPHA", "特殊地形alpha不是原版的0、1或2层范围。");
}
void Validate(const TerrainGridSnapshot& grid, const SpecialDoodadPlacement& placement,
    const TerrainDoodadFootprint& image, std::uint8_t maximumLevel) {
    if (placement.variation != 0 || placement.position[0] < 0 || placement.position[1] < 0)
        Fail("TERRAIN_SPECIAL_LAYOUT", "特殊地形需要已知的零变体及非负地格坐标。");
    if (image.width < 4 || image.height < 4 || image.width > 256 || image.height > 256
        || image.width % 4 || image.height % 4 || (image.pixelBytes != 3 && image.pixelBytes != 4)
        || image.pixels.size() != std::size_t{image.width} * image.height * image.pixelBytes)
        Fail("TERRAIN_SPECIAL_LAYOUT", "特殊占地需要宽高为4倍数的4..256完整TGA。");
    const auto x = static_cast<std::uint32_t>(placement.position[0]), y = static_cast<std::uint32_t>(placement.position[1]);
    const auto w = image.width / 4, h = image.height / 4;
    if (x >= grid.header.width - 1 || y >= grid.header.height - 1
        || w > grid.header.width - 1 - x || h > grid.header.height - 1 - y)
        Fail("TERRAIN_SPECIAL_BOUNDS", "特殊地形矩形超出实际地图地格范围。");
    const int first = grid.vertices[std::size_t{y} * grid.header.width + x].CliffLevel();
    const int base = first - Elevation(image, 0, 0);
    for (unsigned dy = 0; dy <= h; ++dy) for (unsigned dx = 0; dx <= w; ++dx) {
        const auto level = Elevation(image, dx, dy);
        // 原版load资格以SW实际层级加alpha检查上限；编辑后完整一致性检查另以相对基准比较。
        if (first + level > maximumLevel)
            Fail("TERRAIN_SPECIAL_HEIGHT_LIMIT", "特殊地形alpha加起点层级超过实际配置上限。");
        if (grid.vertices[std::size_t{y + dy} * grid.header.width + x + dx].CliffLevel() != base + level)
            Fail("TERRAIN_SPECIAL_CLIFF_CHANGED", "特殊地形的顶点层级与贴图alpha不一致，原版会删除该模型，当前不能保留它保存。");
    }
}
} // namespace

Result<std::vector<std::uint8_t>> BuildTerrainSpecialDoodadPathing(const TerrainGridSnapshot& before,
    const TerrainGridSnapshot& after, std::span<const TerrainSpecialDoodadPathingInput> inputs,
    std::uint8_t maximumCliffLevel, std::size_t maximumWorkingBytes) {
    try {
        const auto& header = before.header;
        if (header.version != 11 || after.header.version != 11 || header.width < 2 || header.height < 2
            || header.width > 2049 || header.height > 2049 || header.width != after.header.width || header.height != after.header.height
            || header.offset != after.header.offset || before.vertices.size() != std::size_t{header.width} * header.height
            || before.vertices.size() != after.vertices.size() || maximumCliffLevel > 14)
            Fail("TERRAIN_SPECIAL_LAYOUT", "特殊通行需要尺寸相同的完整经典地形和实际0..14层上限。");
        if (inputs.empty()) return Result<std::vector<std::uint8_t>>::Success({});
        const auto width = std::size_t{header.width - 1} * 4, height = std::size_t{header.height - 1} * 4;
        const auto cells = width * height;
        if (cells > maximumWorkingBytes || inputs.size() > maximumWorkingBytes / sizeof(TerrainSpecialDoodadPathingInput))
            Fail("TERRAIN_SPECIAL_BUDGET", "特殊地形输出或记录数超过本次预算。");
        std::size_t pixelWork = 0;
        for (const auto& input : inputs) {
            if (!input.placement) Fail("TERRAIN_SPECIAL_LAYOUT", "特殊地形缺少实际摆放记录。");
            if (!input.footprint) continue;
            Validate(before, *input.placement, *input.footprint, maximumCliffLevel);
            Validate(after, *input.placement, *input.footprint, maximumCliffLevel);
            const auto pixels = std::size_t{input.footprint->width} * input.footprint->height;
            if (pixels > maximumWorkingBytes - pixelWork)
                Fail("TERRAIN_SPECIAL_BUDGET", "特殊地形累计像素工作超过本次预算。");
            pixelWork += pixels;
        }
        std::vector<std::uint8_t> result(cells, 0);
        for (const auto& input : inputs) {
            if (!input.footprint) continue;
            const auto& image = *input.footprint;
            const auto left = static_cast<std::size_t>(input.placement->position[0]) * 4;
            const auto bottom = static_cast<std::size_t>(input.placement->position[1]) * 4;
            for (unsigned y = 0; y < image.height; ++y) for (unsigned x = 0; x < image.width; ++x) {
                auto& target = result[(bottom + y) * width + left + x];
                if (target & 1) continue; // 原版选择第一条覆盖记录；全黑像素也阻止后续项覆盖。
                const auto row = image.height - 1 - y;
                target = static_cast<std::uint8_t>(1 | (Channel(image, x, row, 2) == 255 ? 0xc2 : 0)
                    | (Channel(image, x, row, 1) == 255 ? 0x04 : 0) | (Channel(image, x, row, 0) == 255 ? 0x08 : 0));
            }
        }
        return Result<std::vector<std::uint8_t>>::Success(std::move(result));
    } catch (Failure& failure) { return {{}, std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Result<std::vector<std::uint8_t>>::Failure(kSubsystem, "TERRAIN_SPECIAL_BUDGET", "特殊地形通行分配失败。"); }
    catch (const std::length_error&) { return Result<std::vector<std::uint8_t>>::Failure(kSubsystem, "TERRAIN_SPECIAL_BUDGET", "特殊地形通行超过容量。"); }
}

} // namespace War3Map::MapData
