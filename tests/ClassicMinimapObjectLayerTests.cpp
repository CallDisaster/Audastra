#include "War3MapCore/Render/ClassicMinimapObjectLayer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace War3Map;
using namespace War3Map::Render;
using Bytes = std::vector<std::byte>;

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string error = "unexpected failure";
        for (const auto& diagnostic : result.diagnostics) error += " | " + diagnostic.code;
        throw std::runtime_error(error);
    }
    return std::move(*result.value);
}
Bytes Text(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data()); return {begin, begin + text.size()};
}
Bytes Slk(std::initializer_list<std::string_view> rows) {
    std::string result = "ID;PWXL;N;E\n"; std::size_t y = 1;
    for (const auto row : rows) {
        std::size_t x = 1, start = 0;
        while (start <= row.size()) {
            const auto end = row.find('|', start);
            const auto value = row.substr(start, end == std::string_view::npos ? row.size() - start : end - start);
            result += "C;X" + std::to_string(x++) + ";Y" + std::to_string(y) + ";K\"" + std::string(value) + "\"\n";
            if (end == std::string_view::npos) break; start = end + 1;
        }
        ++y;
    }
    return Text(result + "E\n");
}
ObjectData::StandardObjectCatalog Catalog(bool mask = false, bool badColor = false) {
    ObjectData::CatalogLoadOptions options;
    options.sources = {{ObjectData::ObjectType::Doodad, {"fixture.slk"}, {}}}; options.stringFiles.clear();
    const auto bytes = Slk({"doodID|showInMM|useMMColor|MMRed|MMGreen|MMBlue|file|pathTex|numVar",
        mask ? "TEST|1|1|10|20|30||Fixture/Mask.tga|1" : badColor ? "TEST|1|1|999|20|30|||1" : "TEST|1|1|10|20|30|||1",
        "HIDE|0|1|40|50|60|||1", "AUTO|1|0|0|0|0|||1"});
    return Take(ObjectData::StandardObjectCatalog::Load([&](const std::string&) { return Result<Bytes>::Success(bytes); }, options));
}
Resources::ResourceResolver Resources(std::optional<Bytes> mask = std::nullopt) {
    Resources::ResourceResolver resolver; Resources::ResourceSource source;
    source.id = "fixture"; source.layer = Resources::ResourceLayer::MapImport; source.priority = 1;
    source.probe = [mask](std::string_view name) { return Result<Resources::ResourceProbe>::Success(mask && name == "FIXTURE\\MASK.TGA"
        ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(name)} : Resources::ResourceProbe{}); };
    source.read = [mask](std::string_view) { return mask ? Result<Bytes>::Success(*mask)
        : Result<Bytes>::Failure("fixture", "RESOURCE_MISSING", "fixture absent"); };
    Require(resolver.AddSource(std::move(source)).Succeeded(), "register fixture"); return resolver;
}
Bytes AsymmetricMask() {
    Bytes bytes(18 + 4 * 2 * 3);
    bytes[2] = std::byte{2}; bytes[12] = std::byte{4}; bytes[14] = std::byte{2};
    bytes[16] = std::byte{24}; bytes[17] = std::byte{0x20}; // top-down 真彩色，首像素非空。
    bytes[18] = std::byte{255}; return bytes;
}
MapData::DoodadPlacement Placement(std::string rawcode = "TEST") {
    MapData::DoodadPlacement placement; placement.rawcode = std::move(rawcode); placement.position = {192, 192, 0};
    placement.scale = {1, 1, 1}; return placement;
}
ClassicMinimapObjectLayerInput Input(const ObjectData::StandardObjectCatalog& catalog,
    const Resources::ResourceResolver& resources, const std::vector<MapData::DoodadPlacement>& placements) {
    ClassicMinimapObjectLayerInput input; input.placements = placements; input.catalog = &catalog;
    input.resources = &resources; input.sourceRectangle = {0, 0, 8, 8};
    input.invalidModelPath = "Fixture\\ConfiguredInvalidModel.mdx"; return input;
}

void FixedFieldsAndOrder() {
    auto catalog = Catalog(); auto resources = Resources();
    std::vector<MapData::DoodadPlacement> placements{Placement(), Placement("HIDE"), Placement("AUTO")};
    auto output = Take(BuildClassicMinimapObjectLayer(Input(catalog, resources, placements)));
    Require(output.pixels.size() == 2 && output.omittedPlacements == 1, "showInMM必须过滤且0x08绘图不产生单位");
    Require(output.pixels[0].x == 1 && output.pixels[0].y == 1, "无pathing按world/map origin定位单格");
    Require(output.pixels[0].color == std::array<std::uint8_t, 4>{10, 20, 30, 255}, "MM颜色不能交换红蓝");
    Require(output.pixels[1].color == std::array<std::uint8_t, 4>{0, 64, 0, 255}, "无模型颜色沿用原版固定深绿色");
    Require(output.pixels[1].placementIndex == 2, "覆盖顺序必须保留DOO遍历次序");
}
void FootprintQuarterTurns() {
    auto catalog = Catalog(true); auto resources = Resources(AsymmetricMask());
    const std::array<std::array<std::uint32_t, 2>, 4> expected{{{5, 4}, {7, 5}, {6, 7}, {4, 6}}};
    for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
        std::vector<MapData::DoodadPlacement> placements{Placement()};
        placements.front().rotation = static_cast<float>(quadrant * std::numbers::pi / 2.0);
        auto input = Input(catalog, resources, placements); input.highResolution = true; input.sourceRectangle = {0, 0, 32, 32};
        const auto output = Take(BuildClassicMinimapObjectLayer(input));
        Require(output.pixels.size() == 1, "非空pathing只覆盖一个源像素");
        Require(output.pixels.front().x == expected[quadrant][0] && output.pixels.front().y == expected[quadrant][1], "原版quarter turn footprint锚点或旋转错误");
    }
}
void RejectBrokenAndOverBudget() {
    auto catalog = Catalog(true); auto malformed = Resources(Bytes{std::byte{0}});
    std::vector<MapData::DoodadPlacement> placements{Placement()};
    Require(!BuildClassicMinimapObjectLayer(Input(catalog, malformed, placements)).Succeeded(), "损坏pathing不能冒充单格缺失");
    auto resources = Resources(AsymmetricMask()); auto input = Input(catalog, resources, placements);
    ClassicMinimapObjectLayerLimits limits; limits.maxPixelsPerFootprint = 1;
    Require(!BuildClassicMinimapObjectLayer(input, limits).Succeeded(), "超预算不能返回成功并漏画");
    auto badColor = Catalog(false, true); auto none = Resources();
    Require(!BuildClassicMinimapObjectLayer(Input(badColor, none, placements)).Succeeded(), "无效MM字段不得省略对象后成功");
    placements.front().rawcode = "NOPE";
    Require(!BuildClassicMinimapObjectLayer(Input(catalog, resources, placements)).Succeeded(), "未知对象不等于空对象层");
    auto noConfigCatalog = Catalog();
    const std::vector<MapData::DoodadPlacement> noConfigPlacements{Placement("AUTO")};
    auto withoutConfig = Input(noConfigCatalog, resources, noConfigPlacements);
    withoutConfig.invalidModelPath.reset();
    Require(!BuildClassicMinimapObjectLayer(withoutConfig).Succeeded(), "缺少InvalidModel配置不得猜测模型路径");
}
}

int main() {
    try { FixedFieldsAndOrder(); FootprintQuarterTurns(); RejectBrokenAndOverBudget(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
