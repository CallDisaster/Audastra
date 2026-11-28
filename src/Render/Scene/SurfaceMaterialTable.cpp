#include "War3MapCore/Render/SurfaceMaterialTable.h"

#include <charconv>
#include <cmath>
#include <optional>
#include <utility>

namespace War3Map::Render {
namespace {
constexpr std::string_view kSpace = " \t\r\n";

std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}
struct Section { std::string rawcode; ModelSurfaceMaterial material; };
bool HasImages(const ModelSurfaceMaterial& m) {
    return !m.normalPath.empty() || !m.ormPath.empty() || !m.detailNormalPath.empty() || !m.detailOrmPath.empty()
        || !m.detailAlbedoPath.empty();
}
std::string* ImagePath(ModelSurfaceMaterial& m, std::string_view key) {
    if (key == "normal") return &m.normalPath;
    if (key == "orm") return &m.ormPath;
    if (key == "detailNormal") return &m.detailNormalPath;
    if (key == "detailOrm") return &m.detailOrmPath;
    if (key == "detailAlbedo") return &m.detailAlbedoPath;
    return nullptr;
}
float* NumberField(ModelSurfaceMaterial& m, std::string_view key) {
    if (key == "normalScale") return &m.normalScale;
    if (key == "roughness") return &m.roughness;
    if (key == "metallic") return &m.metallic;
    if (key == "occlusion") return &m.occlusion;
    if (key == "specular") return &m.specular;
    if (key == "detailScale") return &m.detailScale;
    if (key == "detailStrength") return &m.detailStrength;
    return nullptr;
}
}

const SurfaceMaterialKey* FindSurfaceMaterialKey(std::string_view name) noexcept {
    for (const auto& key : kSurfaceMaterialKeys) if (key.name == name) return &key;
    return nullptr;
}

std::optional<float> ParseSurfaceMaterialNumber(std::string_view text, const SurfaceMaterialKey& key) {
    if (key.kind != SurfaceMaterialKeyKind::Number) return std::nullopt;
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    float value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)
        || value < key.minimum || value > key.maximum) return std::nullopt;
    return value;
}

bool SurfaceMaterialRawcode(std::string_view text) noexcept {
    if (text.size() != 4) return false;
    for (const char c : text) if (c <= ' ' || c > '~') return false;
    return true;
}

SurfaceMaterialLine ReadSurfaceMaterialLine(std::string_view line) noexcept {
    SurfaceMaterialLine out;
    const auto content = line.substr(0, line.find_first_of(";#"));
    const auto trimmed = Trim(content);
    if (trimmed.empty()) return out;
    if (trimmed.front() == '[') {
        const auto close = trimmed.find(']');
        const auto code = close == std::string_view::npos ? std::string_view{} : Trim(trimmed.substr(1, close - 1));
        out.kind = SurfaceMaterialRawcode(code) ? SurfaceMaterialLine::Kind::Section : SurfaceMaterialLine::Kind::BadSection;
        out.rawcode = code;
        return out;
    }
    const auto equals = content.find('=');
    if (equals == std::string_view::npos) { out.kind = SurfaceMaterialLine::Kind::Bad; return out; }
    out.kind = SurfaceMaterialLine::Kind::KeyValue;
    out.key = Trim(content.substr(0, equals));
    const auto first = content.find_first_not_of(kSpace, equals + 1);
    if (first == std::string_view::npos) {
        out.valueBegin = out.valueEnd = equals + 1;
    } else {
        out.valueBegin = first;
        out.valueEnd = content.find_last_not_of(kSpace) + 1;
    }
    out.value = content.substr(out.valueBegin, out.valueEnd - out.valueBegin);
    return out;
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
        const auto read = ReadSurfaceMaterialLine(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        using Kind = SurfaceMaterialLine::Kind;
        if (read.kind == Kind::Blank) continue;
        if (read.kind == Kind::Section || read.kind == Kind::BadSection) {
            current = nullptr;
            if (read.kind == Kind::BadSection) { warn(lineNumber, "节名不是四字符物体类型。"); continue; }
            for (auto& section : sections) if (section.rawcode == read.rawcode) current = &section;
            if (!current) {
                if (sections.size() >= kSurfaceMaterialTableMaxTypes) { warn(lineNumber, "物体类型超过 256 种，本节忽略。"); continue; }
                current = &sections.emplace_back(Section{std::string(read.rawcode), {}});
            }
            continue;
        }
        if (!current || read.kind == Kind::Bad) { warn(lineNumber, "不在任何节内，或缺少“=”。"); continue; }
        const auto* key = FindSurfaceMaterialKey(read.key);
        if (!key) { warn(lineNumber, "未知键 " + std::string(read.key) + "。"); continue; }
        auto& m = current->material;
        if (key->kind == SurfaceMaterialKeyKind::Image) { *ImagePath(m, key->name) = std::string(read.value); continue; }
        if (key->kind == SurfaceMaterialKeyKind::Number) {
            if (const auto parsed = ParseSurfaceMaterialNumber(read.value, *key)) *NumberField(m, key->name) = *parsed;
            else warn(lineNumber, std::string(key->name) + " 的值无效或超出范围，保留原值。");
            continue;
        }
        if (read.value != key->choices[0] && read.value != key->choices[1]) {
            warn(lineNumber, std::string(key->name) + " 只能是 " + std::string(key->choices[0]) + " 或 "
                + std::string(key->choices[1]) + "，保留原值。");
            continue;
        }
        if (key->name == "shader") m.shader = read.value == "water" ? ModelSurfaceShader::Water : ModelSurfaceShader::Surface;
        else m.normalYSign = read.value == "gl" ? -1.0f : 1.0f;
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
