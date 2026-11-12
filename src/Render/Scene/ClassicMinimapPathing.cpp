#include "War3MapCore/Render/ClassicMinimapPathing.h"

namespace War3Map::Render {
namespace {

constexpr const char* kSubsystem = "Minimap.Pathing";
constexpr std::uint8_t kAlpha = 64;

// 原版普通模式的状态到颜色（1.27a WorldEdit 调色表），索引为 不可走4|不可飞2|不可建1。
// 2026-11-12 原版受控导出逐格核对：浅水=蓝、深水与悬崖边=品红、边界=白。
constexpr std::array<std::array<std::uint8_t, 3>, 8> kStateColors{{
    {0, 0, 0}, {0, 0, 255}, {0, 255, 0}, {0, 255, 255},
    {255, 0, 0}, {255, 0, 255}, {255, 255, 0}, {255, 255, 255},
}};

Status Reject(const char* code, const char* message) {
    return Status::Failure(kSubsystem, code, message);
}

} // namespace

std::optional<std::array<std::uint8_t, 4>> ClassicMinimapPathingColor(std::uint8_t cell) noexcept {
    const unsigned state = ((cell & 0x02u) ? 4u : 0u) | ((cell & 0x04u) ? 2u : 0u) | ((cell & 0x08u) ? 1u : 0u);
    if (!state) return std::nullopt;
    const auto& color = kStateColors[state];
    return std::array<std::uint8_t, 4>{color[0], color[1], color[2], kAlpha};
}

Status CompositeClassicMinimapPathing(Image::DecodedImage& canvas, const TerrainMinimapPostprocessFacts& facts,
    const std::array<std::uint32_t, 4>& rectangle, const MapData::PathingGridSnapshot& pathing) {
    if (canvas.width != 256 || canvas.height != 256 || canvas.rgba.size() != 256u * 256u * 4u)
        return Reject("MINIMAP_PATHING_CANVAS", "通行叠加只接受 256x256 RGBA 画布。");
    if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1]
        || facts.sourceWidth != rectangle[2] - rectangle[0] || facts.sourceHeight != rectangle[3] - rectangle[1])
        return Reject("MINIMAP_PATHING_RECTANGLE", "通行叠加范围与底图不一致。");
    if (pathing.width % 4 || pathing.height % 4 || rectangle[2] > pathing.width / 4 || rectangle[3] > pathing.height / 4
        || pathing.cells.size() != static_cast<std::size_t>(pathing.width) * pathing.height)
        return Reject("MINIMAP_PATHING_GRID", "通行图尺寸与地形格范围不一致。");
    for (std::uint32_t y = 0; y < 256; ++y) {
        for (std::uint32_t x = 0; x < 256; ++x) {
            const auto source = TerrainMinimapCanvasSource(facts, x, y);
            if (!source) continue;
            // 原版逐地形格取其西南角通行格（格坐标 4*tile）。
            const auto cellX = 4 * (rectangle[0] + (*source)[0]), cellY = 4 * (rectangle[1] + (*source)[1]);
            const auto color = ClassicMinimapPathingColor(pathing.cells[static_cast<std::size_t>(cellY) * pathing.width + cellX]);
            if (!color) continue;
            const float alpha = static_cast<float>(static_cast<double>((*color)[3]) / 255.0);
            const float keep = static_cast<float>(1.0 - alpha);
            const auto at = (static_cast<std::size_t>(y) * 256 + x) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                const float base = static_cast<float>(std::to_integer<unsigned>(canvas.rgba[at + c]));
                const float overlay = static_cast<float>((*color)[c]);
                canvas.rgba[at + c] = static_cast<std::byte>(static_cast<int>(base * keep + overlay * alpha));
            }
        }
    }
    return Status::Ok();
}

} // namespace War3Map::Render
