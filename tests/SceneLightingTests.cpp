#include "War3MapCore/Render/DayNightLighting.h"
#include "War3MapCore/Render/SceneEnvironment.h"
#include "War3MapCore/MapData/MapShadow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Render;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Near(float actual, float expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-4f) throw std::runtime_error(message);
}

// 原创最小 MDX 字节：只含版本、动作与一个光源。
class Bytes {
public:
    void U32(std::uint32_t value) { Append(&value, 4); }
    void F32(float value) { Append(&value, 4); }
    void Tag(const char* tag) { Append(tag, 4); }
    void Name(const std::string& text, std::size_t width) {
        std::vector<char> field(width, '\0');
        std::memcpy(field.data(), text.data(), std::min(text.size(), width));
        Append(field.data(), width);
    }
    void Chunk(const char* tag, const Bytes& body) { Tag(tag); U32(static_cast<std::uint32_t>(body.data.size())); Append(body.data.data(), body.data.size()); }
    void Raw(const Bytes& other) { Append(other.data.data(), other.data.size()); }
    std::vector<std::byte> data;

private:
    void Append(const void* source, std::size_t count) {
        const auto* bytes = static_cast<const std::byte*>(source);
        data.insert(data.end(), bytes, bytes + count);
    }
};

std::vector<std::byte> DayNightMdx(std::array<float, 4> rotation) {
    Bytes file;
    file.Tag("MDLX");
    Bytes version; version.U32(800); file.Chunk("VERS", version);
    Bytes sequences;
    sequences.Name("Stand", 80); sequences.U32(0); sequences.U32(24000);
    for (int i = 0; i < 11; ++i) sequences.U32(0); // 其余 44 字节
    file.Chunk("SEQS", sequences);

    Bytes node;
    node.Name("Sun", 80); node.U32(0); node.U32(0xFFFFFFFFu); node.U32(0x200);
    node.Tag("KGRT"); node.U32(1); node.U32(0); node.U32(0xFFFFFFFFu);
    node.U32(0); for (const auto c : rotation) node.F32(c);
    Bytes light;
    light.U32(static_cast<std::uint32_t>(node.data.size() + 4)); light.Raw(node);
    light.U32(1); light.F32(0); light.F32(0);                     // 平行光、衰减
    light.F32(0.1f); light.F32(0.2f); light.F32(0.3f); light.F32(2.0f);   // 颜色 BGR、强度
    light.F32(0.0f); light.F32(0.5f); light.F32(1.0f); light.F32(0.25f);  // 环境光 BGR、强度
    // KLAC 线性：0 时红、24 时蓝（BGR 存储）。
    light.Tag("KLAC"); light.U32(2); light.U32(1); light.U32(0xFFFFFFFFu);
    light.U32(0); light.F32(0); light.F32(0); light.F32(1);
    light.U32(24000); light.F32(1); light.F32(0); light.F32(0);
    // KLBI Hermite：带切线，值 0.4 → 0.8。
    light.Tag("KLBI"); light.U32(2); light.U32(2); light.U32(0xFFFFFFFFu);
    light.U32(0); light.F32(0.4f); light.F32(9); light.F32(9);
    light.U32(24000); light.F32(0.8f); light.F32(9); light.F32(9);
    Bytes lights;
    lights.U32(static_cast<std::uint32_t>(light.data.size() + 4)); lights.Raw(light);
    file.Chunk("LITE", lights);
    return file.data;
}

void TestProfile() {
    const auto profile = Text::ParseProfile("// c\n[TerrainLights]\r\nA = x.mdl \n; skip\nL=\n[Other]\nA=y\n");
    Require(profile.at("terrainlights").at("a") == "x.mdl" && profile.at("terrainlights").at("l").empty()
        && profile.at("other").at("a") == "y" && profile.size() == 2, "profile 节/键小写、值去空白、跳过注释");
    constexpr std::string_view duplicate = "[ WATER ]\r\n DeepLevelPathing = 64\n; ignored\n[water]\nDeepLevelPathing=128 \n";
    std::vector<std::string> values;
    Text::VisitProfileFields(duplicate, [&](std::string_view section, std::string_view key, std::string_view value) {
        Require(section == "water" && key == "deeplevelpathing", "profile visitor uses the shared normalization rules");
        values.emplace_back(value);
    });
    Require(values == std::vector<std::string>{"64", "128"}, "profile visitor preserves ordered duplicate definitions");
    Require(Text::ParseProfile(duplicate).at("water").at("deeplevelpathing") == "128", "legacy profile consumers keep last-value semantics");
}

void TestDayNight() {
    const auto parsed = ParseMdx(DayNightMdx({0, 0, 0, 1}));
    if (!parsed.Succeeded()) throw std::runtime_error("光源模型解析失败：" + parsed.diagnostics.front().message);
    const auto& model = *parsed.value;
    Require(model.lights.size() == 1 && model.lights[0].type == 1 && model.lights[0].name == "Sun", "读取平行光");
    Near(model.lights[0].color[0], 0.3f, "静态颜色 BGR 转 RGB");
    Require(model.lights[0].colorKeys.size() == 2 && model.lights[0].ambientIntensityKeys.size() == 2, "读取颜色/强度轨迹");
    const auto noon = SampleDayNight(model, 12.0f);
    Require(noon.has_value(), "正午光照");
    // 直射光：红蓝各半 × 强度 2；环境光：静态颜色 (1, 0.5, 0) × 轨迹强度 0.6。
    Near(noon->diffuse[0], 1.0f, "直射光红"); Near(noon->diffuse[1], 0.0f, "直射光绿"); Near(noon->diffuse[2], 1.0f, "直射光蓝");
    Near(noon->ambient[0], 0.6f, "环境光红"); Near(noon->ambient[1], 0.3f, "环境光绿"); Near(noon->ambient[2], 0.0f, "环境光蓝");
    Near(noon->towardLight[2], 1.0f, "无旋转时光向朝上（光沿 −Z 射下）");
    Near(SampleDayNight(model, 30.0f)->diffuse[2], 2.0f, "时刻夹到 24 时");
    // 绕 X 轴 90°：光向 (0, −1, 0)。
    const float half = std::sqrt(0.5f);
    const auto tilted = SampleDayNight(*ParseMdx(DayNightMdx({half, 0, 0, half})).value, 0.0f);
    Near(tilted->towardLight[1], -1.0f, "节点旋转决定光向");
    MdxModel empty;
    Require(!SampleDayNight(empty, 12.0f), "没有平行光时为空");
}

void TestModelPath() {
    const auto profile = Text::ParseProfile("[TerrainLights]\nA=Environment\\DNC\\A\\T.mdl\n[UnitLights]\nA=Environment\\DNC\\A\\U.MDL\n");
    Require(DayNightModelPath(profile, 'A', 0, false) == "Environment\\DNC\\A\\T.mdx", "按主 tileset 取地形光并改 .mdx");
    Require(DayNightModelPath(profile, 'A', 0, true) == "Environment\\DNC\\A\\U.mdx", "单位光");
    Require(DayNightModelPath(profile, 'L', 0, false).find("DNCLordaeronTerrain.mdx") != std::string::npos, "缺条目用 Lordaeron");
    Require(DayNightModelPath(profile, 'L', 'A', false) == "Environment\\DNC\\A\\T.mdx", "光照环境代替主 tileset");
}

MapData::TerrainGridSnapshot Grid() {
    MapData::TerrainGridSnapshot grid;
    grid.header.width = 3; grid.header.height = 2;
    grid.header.offset = {-256.0f, 64.0f};
    return grid;
}

void TestEnvironment() {
    MapData::MapEnvironmentSettings settings;
    settings.fogStyle = 0; settings.fogStart = 1000; settings.fogEnd = 5000;
    settings.fogColor = {10, 20, 30, 255}; // BGRA
    const SceneLight sun{{0, 0, 2}, {1, 1, 1}, {0.2f, 0.2f, 0.2f}};
    std::vector<std::byte> shadow(8 * 4);
    shadow[0] = std::byte{1}; shadow[31] = std::byte{255};
    auto environment = BuildSceneEnvironment(&sun, &settings, true, Grid(), std::span<const std::byte>(shadow));
    const auto& v = environment.values;
    Require(v.fogMode == WAR3_RENDER_FOG_LINEAR && v.fogStart == 1000 && v.fogEnd == 5000 && environment.notes.empty(), "线性雾");
    Near(v.fogColor[0], 30.0f / 255.0f, "雾色 BGRA 取 RGB");
    Require(v.shadowWidth == 8 && v.shadowHeight == 4 && v.shadowCellSize == 32.0f && v.shadowOriginX == -256.0f
        && v.shadowOriginY == 64.0f && environment.shadow[0] == 255 && environment.shadow[1] == 0 && environment.shadow[31] == 255,
        "阴影图每格 4×4、原点为地形西南角、非 0 即阴影");
    const auto described = DescribeSceneEnvironment(environment);
    Require(described.shadow == environment.shadow.data() && described.structSize == sizeof(War3RenderEnvironmentV2), "描述借用阴影像素");

    // 未勾选“使用地形迷雾”时，即使雾参数有效也不显示（与生成脚本不调用 SetTerrainFogEx 一致）。
    Require(BuildSceneEnvironment(&sun, &settings, false, Grid(), std::nullopt).values.fogMode == WAR3_RENDER_FOG_NONE,
        "未启用地形迷雾不显示雾");
    settings.fogStyle = 2;
    environment = BuildSceneEnvironment(nullptr, &settings, true, Grid(), std::nullopt);
    Require(environment.values.fogMode == WAR3_RENDER_FOG_LINEAR && environment.notes.size() == 1, "指数雾按线性近似并说明");
    ApplySceneFog(environment, &settings, false);
    Require(environment.values.fogMode == WAR3_RENDER_FOG_NONE && environment.notes.empty(), "关闭地形迷雾时移除雾与雾说明");
    settings.fogStyle = 0;
    ApplySceneFog(environment, &settings, true);
    Require(environment.values.fogMode == WAR3_RENDER_FOG_LINEAR && environment.values.fogEnd == 5000 && environment.notes.empty(),
        "重新启用时只重算雾");
    settings.fogStyle = 2;
    Require(environment.values.shadowWidth == 0 && DescribeSceneEnvironment(environment).shadow == nullptr, "无阴影文件");
    Near(environment.values.ambientColor[0], TerrainLighting{}.ambient[0], "无 DNC 时中性光与地表默认一致");
    settings.fogStyle = 3;
    Require(BuildSceneEnvironment(nullptr, &settings, true, Grid(), std::nullopt).values.fogMode == WAR3_RENDER_FOG_NONE, "未知雾类型不显示");
    settings.fogStyle = 0; settings.fogEnd = 500;
    Require(BuildSceneEnvironment(nullptr, &settings, true, Grid(), std::nullopt).values.fogMode == WAR3_RENDER_FOG_NONE, "起止无效不显示");
    shadow.pop_back();
    environment = BuildSceneEnvironment(nullptr, &settings, false, Grid(), std::span<const std::byte>(shadow));
    Require(environment.values.fogMode == WAR3_RENDER_FOG_NONE && environment.shadow.empty() && environment.notes.size() == 1,
        "阴影图大小不符时忽略并说明");

    Near(TerrainLightingFrom(&sun).diffuse[0], 1.0f, "地形光转地表光照");
    const SceneLight below{{0, 0, -1}, {1, 1, 1}, {0, 0, 0}};
    Near(TerrainLightingFrom(&below).ambient[0], TerrainLighting{}.ambient[0], "光向朝下时保留默认光照");
}

void TestShadowInputRetainsCodecFactsAndExistingPixels() {
    auto grid = Grid(); grid.header.height = 9; // 8×32字节，独立覆盖所有256种合法原强度。
    std::vector<std::byte> source;
    for (unsigned value = 0; value < 256; ++value) source.push_back(static_cast<std::byte>(value));
    const auto original = source;
    auto environment = BuildSceneEnvironment(nullptr, nullptr, false, grid, std::span<const std::byte>(source));
    Require(source == original && environment.shadowInput.state == SceneShadowInputState::Parsed
        && environment.shadowInput.sourceBytes == 256 && environment.shadowInput.diagnostics.empty(),
        "全部原强度合法且raw逐字不变，Parsed只记录本次codec结果");
    Require(environment.shadow.size() == 256 && environment.shadow.front() == 0 && environment.notes.empty(),
        "合法正文保留既有预览尺寸和无额外告警");
    for (std::size_t index = 1; index < environment.shadow.size(); ++index)
        Require(environment.shadow[index] == 255, "预览仍非零到255，不能把raw非二值误拒或改像素政策");
    Require(environment.values.shadowWidth == 8 && environment.values.shadowHeight == 32
        && environment.values.shadowOriginX == -256 && environment.values.shadowOriginY == 64
        && environment.values.shadowCellSize == 32 && environment.values.shadowDarkness == kStaticShadowDarkness,
        "codec接入不改变旧空间、暗度或渲染ABI");
    auto zeros = source; std::fill(zeros.begin(), zeros.end(), std::byte{0});
    const auto zero = BuildSceneEnvironment(nullptr, nullptr, false, grid, std::span<const std::byte>(zeros));
    Require(zero.shadowInput.state == SceneShadowInputState::Parsed && zero.shadow.size() == 256
        && DescribeSceneEnvironment(zero).shadow != nullptr && std::all_of(zero.shadow.begin(), zero.shadow.end(), [](auto value) { return value == 0; }),
        "真实全零正文是已解析预览，不能按无暗像素误判为未提供或缺失");
    const auto absent = BuildSceneEnvironment(nullptr, nullptr, false, grid, std::nullopt);
    Require(absent.shadowInput.state == SceneShadowInputState::NotProvided && !absent.shadowInput.sourceBytes
        && absent.shadowInput.diagnostics.empty() && absent.shadow.empty() && absent.notes.empty(),
        "nullopt只证明未提供，不捏造成员Missing或codec失败");
    const auto sameDiagnostics = [](const std::vector<Diagnostic>& actual, const std::vector<Diagnostic>& expected) {
        Require(actual.size() == expected.size(), "codec诊断不能丢失或重复");
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const auto& a = actual[i]; const auto& b = expected[i];
            Require(a.severity == b.severity && a.subsystem == b.subsystem && a.code == b.code && a.message == b.message
                && a.origin.has_value() == b.origin.has_value(), "codec原诊断不得压码或改写语义");
            if (a.origin) Require(a.origin->source == b.origin->source && a.origin->offset == b.origin->offset
                && a.origin->length == b.origin->length && a.origin->fingerprint == b.origin->fingerprint, "codec origin须完整保留");
        }
    };
    const auto rejected = [&](const MapData::TerrainGridSnapshot& rejectedGrid, std::vector<std::byte> raw,
        std::uint32_t columns, std::uint32_t rows, SceneShadowInputState state, const char* code) {
        const auto codec = MapData::ParseMapShadow(raw, columns, rows);
        Require(!codec.Succeeded() && codec.diagnostics.front().code == code, "独立codec对照必须命中预期拒绝");
        const auto before = raw;
        auto result = BuildSceneEnvironment(nullptr, nullptr, false, rejectedGrid, std::span<const std::byte>(raw));
        Require(raw == before && result.shadowInput.state == state && result.shadowInput.sourceBytes == raw.size()
            && result.shadow.empty() && !DescribeSceneEnvironment(result).shadow && result.notes.size() == 1
            && result.notes.front().code == "SCENE_SHADOW_SIZE", "坏正文保留一条兼容说明，不返回部分阴影或改源字节");
        sameDiagnostics(result.shadowInput.diagnostics, codec.diagnostics);
        ApplySceneFog(result, nullptr, false);
        sameDiagnostics(result.shadowInput.diagnostics, codec.diagnostics);
        Require(result.notes.size() == 1 && result.notes.front().code == "SCENE_SHADOW_SIZE", "重算雾不能清除或复制原阴影诊断");
    };
    rejected(grid, {}, 8, 32, SceneShadowInputState::PresentEmpty, "SHADOW_TRUNCATED");
    source.pop_back(); rejected(grid, source, 8, 32, SceneShadowInputState::Rejected, "SHADOW_TRUNCATED");
    source = original; source.push_back(std::byte{0}); rejected(grid, source, 8, 32, SceneShadowInputState::Rejected, "SHADOW_TRAILING_DATA");
    auto dimensions = grid; dimensions.header.width = 1;
    rejected(dimensions, {std::byte{1}}, 0, 32, SceneShadowInputState::Rejected, "SHADOW_DIMENSIONS");
    dimensions.header.width = 2050;
    rejected(dimensions, {std::byte{1}}, 8196, 32, SceneShadowInputState::Rejected, "SHADOW_GRID_LIMIT");
}

} // namespace

int main() {
    try {
        TestProfile();
        TestDayNight();
        TestModelPath();
        TestEnvironment();
        TestShadowInputRetainsCodecFactsAndExistingPixels();
        std::cout << "scene lighting: profile, MDX lights, day/night sampling, DNC paths, fog and static shadow passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
