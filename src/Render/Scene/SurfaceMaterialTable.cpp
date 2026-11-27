#include "War3MapCore/Render/SurfaceMaterialTable.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <utility>

namespace War3Map::Render {
namespace {
std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
bool Rawcode(std::string_view text) {
    if (text.size() != 4) return false;
    for (const char c : text) if (c <= ' ' || c > '~') return false;
    return true;
}
std::optional<float> Number(std::string_view text, float low, float high) {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    float value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)
        || value < low || value > high) return std::nullopt;
    return value;
}
struct Section { std::string rawcode; ModelSurfaceMaterial material; };
bool HasImages(const ModelSurfaceMaterial& m) {
    return !m.normalPath.empty() || !m.ormPath.empty() || !m.detailNormalPath.empty() || !m.detailOrmPath.empty()
        || !m.detailAlbedoPath.empty();
}
}

std::shared_ptr<const ModelSurfaceMaterial> SurfaceMaterialTable::Find(std::string_view rawcode) const {
    const auto found = byRawcode.find(rawcode);
    return found == byRawcode.end() ? nullptr : found->second;
}

SurfaceMaterialTable ParseSurfaceMaterialTable(std::string_view text) {
    SurfaceMaterialTable table;
    std::vector<Section> sections;
    Section* current = nullptr;
    const auto warn = [&](std::size_t line, std::string message) {
        if (table.warnings.size() >= 64) return;
        table.warnings.push_back({DiagnosticSeverity::Warning, "Render.SurfaceMaterials", "SURFACE_MATERIAL_LINE_INVALID",
            std::string(kSurfaceMaterialTablePath) + " 第 " + std::to_string(line) + " 行：" + std::move(message), std::nullopt});
    };
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::size_t lineNumber = 0;
    while (!text.empty()) {
        ++lineNumber;
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (const auto comment = line.find_first_of(";#"); comment != std::string_view::npos) line = line.substr(0, comment);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            current = nullptr;
            const auto close = line.find(']');
            const auto code = close == std::string_view::npos ? std::string_view{} : Trim(line.substr(1, close - 1));
            if (!Rawcode(code)) { warn(lineNumber, "节名不是四字符物体类型。"); continue; }
            for (auto& section : sections) if (section.rawcode == code) current = &section;
            if (!current) {
                if (sections.size() >= kSurfaceMaterialTableMaxTypes) { warn(lineNumber, "物体类型超过 256 种，本节忽略。"); continue; }
                current = &sections.emplace_back(Section{std::string(code), {}});
            }
            continue;
        }
        const auto equals = line.find('=');
        if (!current || equals == std::string_view::npos) { warn(lineNumber, "不在任何节内，或缺少“=”。"); continue; }
        const auto key = Trim(line.substr(0, equals)), value = Trim(line.substr(equals + 1));
        auto& m = current->material;
        const auto number = [&](float& target, float low, float high) {
            if (const auto parsed = Number(value, low, high)) target = *parsed;
            else warn(lineNumber, std::string(key) + " 的值无效或超出范围，保留原值。");
        };
        if (key == "shader") {
            if (value == "surface" || value == "water")
                m.shader = value == "water" ? ModelSurfaceShader::Water : ModelSurfaceShader::Surface;
            else warn(lineNumber, "shader 只能是 surface 或 water，保留原值。");
        }
        else if (key == "normal") m.normalPath = std::string(value);
        else if (key == "orm") m.ormPath = std::string(value);
        else if (key == "detailNormal") m.detailNormalPath = std::string(value);
        else if (key == "detailOrm") m.detailOrmPath = std::string(value);
        else if (key == "detailAlbedo") m.detailAlbedoPath = std::string(value);
        else if (key == "normalY") {
            if (value == "gl" || value == "dx") m.normalYSign = value == "gl" ? -1.0f : 1.0f;
            else warn(lineNumber, "normalY 只能是 gl 或 dx，保留原值。");
        }
        else if (key == "normalScale") number(m.normalScale, 0.0f, 8.0f);
        else if (key == "roughness") number(m.roughness, 0.0f, 4.0f);
        else if (key == "metallic") number(m.metallic, 0.0f, 1.0f);
        else if (key == "occlusion") number(m.occlusion, 0.0f, 1.0f);
        else if (key == "specular") number(m.specular, 0.0f, 4.0f);
        else if (key == "detailScale") number(m.detailScale, 8.0f, 65536.0f);
        else if (key == "detailStrength") number(m.detailStrength, 0.0f, 4.0f);
        else warn(lineNumber, "未知键 " + std::string(key) + "。");
    }
    for (auto& section : sections) {
        if (section.material.shader == ModelSurfaceShader::Surface && !HasImages(section.material)) {
            if (table.warnings.size() < 64)
                table.warnings.push_back({DiagnosticSeverity::Warning, "Render.SurfaceMaterials", "SURFACE_MATERIAL_NO_IMAGES",
                    std::string(kSurfaceMaterialTablePath) + " 的 [" + section.rawcode + "] 没有任何图片，已忽略。", std::nullopt});
            continue;
        }
        table.byRawcode.emplace(section.rawcode, std::make_shared<const ModelSurfaceMaterial>(std::move(section.material)));
    }
    return table;
}
}
