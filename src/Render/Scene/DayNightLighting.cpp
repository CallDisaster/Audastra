#include "War3MapCore/Render/DayNightLighting.h"

#include <algorithm>
#include <cmath>

namespace War3Map::Render {
namespace {

float Lerp(float a, float b, float t) { return a + (b - a) * t; }
std::array<float, 3> Lerp(const std::array<float, 3>& a, const std::array<float, 3>& b, float t) {
    return {Lerp(a[0], b[0], t), Lerp(a[1], b[1], t), Lerp(a[2], b[2], t)};
}

// 按帧线性插值；区间外取两端按键，没有按键时取静态值。
template <class Key, class Value, class Get>
Value SampleKeys(const std::vector<Key>& keys, float frame, Value fallback, Get get) {
    if (keys.empty()) return fallback;
    if (frame <= static_cast<float>(keys.front().frame)) return get(keys.front());
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (frame > static_cast<float>(keys[i].frame)) continue;
        const auto span = static_cast<float>(keys[i].frame - keys[i - 1].frame);
        const auto t = span > 0 ? (frame - static_cast<float>(keys[i - 1].frame)) / span : 1.0f;
        return Lerp(get(keys[i - 1]), get(keys[i]), t);
    }
    return get(keys.back());
}

// 单位四元数旋转向量。
std::array<float, 3> Rotate(std::array<float, 4> q, const std::array<float, 3>& v) {
    const auto length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 0)) return v;
    for (auto& c : q) c /= length;
    const auto [x, y, z, w] = q;
    const std::array<float, 3> t{2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])};
    return {v[0] + w * t[0] + (y * t[2] - z * t[1]), v[1] + w * t[1] + (z * t[0] - x * t[2]),
        v[2] + w * t[2] + (x * t[1] - y * t[0])};
}

std::array<float, 3> Scale(const std::array<float, 3>& color, float intensity) {
    return {color[0] * intensity, color[1] * intensity, color[2] * intensity};
}

} // namespace

std::optional<SceneLight> SampleDayNight(const MdxModel& model, float hours) {
    const auto light = std::find_if(model.lights.begin(), model.lights.end(), [](const MdxLight& l) { return l.type == 1; });
    const auto stand = std::find_if(model.sequences.begin(), model.sequences.end(),
        [](const MdxSequence& s) { return Text::LowerAscii(s.name).starts_with("stand"); });
    if (light == model.lights.end() || stand == model.sequences.end() || stand->end <= stand->start) return std::nullopt;
    const auto day = std::clamp(std::isfinite(hours) ? hours : kDefaultDayNightHours, 0.0f, 24.0f) / 24.0f;
    const auto frame = static_cast<float>(stand->start) + static_cast<float>(stand->end - stand->start) * day;
    const auto color = [](const MdxColorKey& key) { return key.rgb; };
    const auto scalar = [](const MdxScalarKey& key) { return key.value; };
    SceneLight output;
    output.diffuse = Scale(SampleKeys(light->colorKeys, frame, light->color, color),
        SampleKeys(light->intensityKeys, frame, light->intensity, scalar));
    output.ambient = Scale(SampleKeys(light->ambientColorKeys, frame, light->ambientColor, color),
        SampleKeys(light->ambientIntensityKeys, frame, light->ambientIntensity, scalar));
    output.towardLight = Rotate(light->rotation, {0, 0, 1});
    return output;
}

std::string DayNightModelPath(const Text::ProfileSections& worldEditData, char mainTileset,
    std::uint8_t lightEnvironment, bool unit) {
    const char tileset = lightEnvironment != 0 ? static_cast<char>(lightEnvironment) : mainTileset;
    std::string path = unit ? "Environment\\DNC\\DNCLordaeron\\DNCLordaeronUnit\\DNCLordaeronUnit.mdl"
                            : "Environment\\DNC\\DNCLordaeron\\DNCLordaeronTerrain\\DNCLordaeronTerrain.mdl";
    if (const auto section = worldEditData.find(unit ? "unitlights" : "terrainlights"); section != worldEditData.end())
        if (const auto entry = section->second.find(Text::LowerAscii(std::string(1, tileset)));
            entry != section->second.end() && !entry->second.empty())
            path = entry->second;
    const auto lower = Text::LowerAscii(path);
    if (lower.ends_with(".mdl")) path.replace(path.size() - 4, 4, ".mdx");
    return path;
}

} // namespace War3Map::Render
