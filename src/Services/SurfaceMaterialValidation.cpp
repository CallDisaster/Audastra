#include "War3MapCore/Services/SurfaceMaterialValidation.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/TextureImage.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <utility>

namespace War3Map::Services {
namespace {
using Render::SurfaceMaterialImageFormat;
using State = SurfaceMaterialImageSource::State;

// 编辑器统计用的解码上限与 DecodeTexture 相同（4096×4096）；更大的图只看文件头。
constexpr std::size_t kMeasurePixels = 16 * 1024 * 1024;
// 法线图“平”的判据：R、G 的标准差都不到一个色阶。
constexpr float kFlatDeviation = 1.0f / 255.0f;
// 切线空间法线图的 R、G 平均值应在 0.5 附近。
constexpr float kTangentMeanTolerance = 0.15f;
// ORM 的 G（粗糙度）最大值低于此值时处处近乎镜面。
constexpr float kGlossyRoughness = 0.05f;
// 细节图平均值低于此值时“像素 ÷ 平均值”会失控。
constexpr float kDetailMeanLow = 0.02f;

Diagnostic Make(DiagnosticSeverity severity, std::string code, std::string message) {
    return {severity, std::string(kSurfaceMaterialSubsystem), std::move(code), std::move(message), std::nullopt};
}
std::string Percent(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", static_cast<double>(value));
    return buffer;
}
std::string Size(const Render::SurfaceMaterialImageHeader& header) {
    return std::to_string(header.width) + "×" + std::to_string(header.height);
}
bool NormalKey(std::string_view key) { return key == "normal" || key == "detailNormal"; }
bool Decodable(SurfaceMaterialImageFormat format) {
    return format == SurfaceMaterialImageFormat::Png || format == SurfaceMaterialImageFormat::Jpeg
        || format == SurfaceMaterialImageFormat::Tga;
}
std::string_view Label(std::string_view key) {
    const auto* found = Render::FindSurfaceMaterialKey(key);
    return found ? found->label : key;
}

void CheckStats(std::string_view key, const Render::SurfaceMaterialPixelStats& stats, std::vector<Diagnostic>& out) {
    const auto& r = stats.channels[0];
    const auto& g = stats.channels[1];
    const auto& b = stats.channels[2];
    if (NormalKey(key)) {
        if (r.deviation < kFlatDeviation && g.deviation < kFlatDeviation)
            out.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_NORMAL_FLAT", "法线图是平的，不会有效果。"));
        if (std::abs(r.mean - 0.5f) > kTangentMeanTolerance || std::abs(g.mean - 0.5f) > kTangentMeanTolerance)
            out.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_NORMAL_NOT_TANGENT",
                "R、G 平均值为 " + Percent(r.mean) + "、" + Percent(g.mean) + "，离 0.5 太远，多半不是切线空间法线图。"));
        return;
    }
    if (key == "orm") {
        if (r.maximum - r.minimum <= 2.0f / 255.0f && g.maximum - g.minimum <= 2.0f / 255.0f
            && b.maximum - b.minimum <= 2.0f / 255.0f)
            out.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_ORM_CONSTANT",
                "整张图是常数（遮蔽 " + Percent(r.mean) + "、粗糙度 " + Percent(g.mean) + "、金属 " + Percent(b.mean)
                    + "），用数值代替图片即可。"));
        if (g.maximum < kGlossyRoughness)
            out.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_ORM_GLOSSY", "粗糙度（G）处处接近 0，会很亮。"));
        if (Render::SurfaceMaterialOrmUncompressed(stats))
            out.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_ORM_METAL",
                "金属（B）大于 24 的像素有 " + std::to_string(stats.metalTexels) + " 个，达到千分之一："
                "游戏一侧不能压成 BC5，按未压缩保留，占四倍内存。"));
        return;
    }
    if (key == "detailOrm" && (r.mean < kDetailMeanLow || g.mean < kDetailMeanLow))
        out.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_DETAIL_MEAN_LOW",
            "平均遮蔽或平均粗糙度接近 0（" + Percent(r.mean) + "、" + Percent(g.mean) + "）；着色按“像素 ÷ 平均值”取比例，结果会失控。"));
    if (key == "detailAlbedo" && stats.brightnessMean < kDetailMeanLow)
        out.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_DETAIL_MEAN_LOW",
            "平均亮度接近 0（" + Percent(stats.brightnessMean) + "）；着色按“像素 ÷ 平均值”取比例，结果会失控。"));
}

void Prefix(std::vector<Diagnostic>& out, const std::vector<Diagnostic>& items, const std::string& prefix) {
    for (auto item : items) { item.message = prefix + item.message; out.push_back(std::move(item)); }
}
}

SurfaceMaterialImageInspection InspectSurfaceMaterialImage(std::string_view key, std::string_view path,
    std::span<const std::byte> bytes) {
    SurfaceMaterialImageInspection out;
    out.bytes = bytes.size();
    if (auto hash = Sha256(bytes); hash.Succeeded() && hash.value) out.sha256 = std::move(*hash.value);
    out.header = Render::ReadSurfaceMaterialImageHeader(path, bytes);
    const auto& header = out.header;
    auto& d = out.diagnostics;
    const std::string format(Render::SurfaceMaterialImageFormatName(header.format));
    if (header.format == SurfaceMaterialImageFormat::Unknown) {
        d.push_back(Make(DiagnosticSeverity::Error, "SURFACE_MATERIAL_IMAGE_FORMAT",
            "认不出图片格式；游戏一侧读 PNG、JPEG、BMP 和 24/32 位 TGA。"));
        return out;
    }
    if (!header.gameReadable) {
        d.push_back(Make(DiagnosticSeverity::Error, "SURFACE_MATERIAL_IMAGE_FORMAT",
            header.format == SurfaceMaterialImageFormat::Tga
                ? std::string("TGA 须是 24 或 32 位、无色表的真彩色，游戏一侧才读得了。")
                : format + " 游戏一侧读不了；用 PNG、JPEG、BMP 或 24/32 位 TGA。"));
        return out;
    }
    if (header.width && header.height) {
        const auto side = std::max(header.width, header.height);
        if (side > Render::kSurfaceMaterialImageMaxSide)
            d.push_back(Make(DiagnosticSeverity::Error, "SURFACE_MATERIAL_IMAGE_TOO_LARGE",
                Size(header) + "：任一边超过 8192，游戏一侧读不了。"));
        else if (side > Render::kSurfaceMaterialImageLargeSide)
            d.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_IMAGE_LARGE",
                Size(header) + "：超过 2048，占内存（游戏一侧每帧最多上传 16 MiB 图片）；编辑器预览缩到 2048。"));
        if (!std::has_single_bit(header.width) || !std::has_single_bit(header.height))
            d.push_back(Make(DiagnosticSeverity::Info, "SURFACE_MATERIAL_IMAGE_NOT_POWER_OF_TWO", Size(header) + "：不是 2 的幂。"));
    }
    if (header.format == SurfaceMaterialImageFormat::Jpeg && key != "detailAlbedo")
        d.push_back(Make(DiagnosticSeverity::Info, "SURFACE_MATERIAL_IMAGE_JPEG",
            "法线和 ORM 建议用 PNG：JPEG 的块状失真在这类图上看得出来。"));
    const auto pixels = static_cast<std::uint64_t>(header.width) * header.height;
    if (!Decodable(header.format) || pixels > kMeasurePixels) {
        d.push_back(Make(DiagnosticSeverity::Info, "SURFACE_MATERIAL_IMAGE_NOT_MEASURED",
            Decodable(header.format) ? std::string("超过编辑器统计上限（4096×4096），没有通道统计与预览。")
                                     : format + " 编辑器不解，没有通道统计与预览。"));
        return out;
    }
    auto decoded = Image::DecodeTexture(path, bytes, kMeasurePixels);
    if (!decoded.Succeeded() || !decoded.value || !decoded.value->Valid()) {
        std::string reason = decoded.diagnostics.empty() ? std::string("解码失败") : decoded.diagnostics.front().message;
        d.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_IMAGE_DECODE", "编辑器解不开这张图：" + reason));
        return out;
    }
    const auto& image = *decoded.value;
    out.header.width = image.width;
    out.header.height = image.height;
    out.stats = Render::MeasureSurfaceMaterialPixels(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(image.rgba.data()), image.rgba.size()));
    CheckStats(key, *out.stats, d);
    return out;
}

std::shared_ptr<const SurfaceMaterialImageInspection> SurfaceMaterialImageCache::Inspect(std::string_view key,
    std::string_view path, std::span<const std::byte> bytes) {
    auto hash = Sha256(bytes);
    // TGA 只能按扩展名认，路径扩展名也进缓存键。
    const auto dot = path.find_last_of('.');
    std::string extension = dot == std::string_view::npos ? std::string{} : std::string(path.substr(dot));
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const auto id = (hash.value ? *hash.value : std::string{}) + "|" + std::string(key) + "|" + extension;
    if (hash.value) {
        std::lock_guard lock(mutex_);
        if (const auto found = entries_.find(id); found != entries_.end()) {
            order_.splice(order_.begin(), order_, found->second.order);
            return found->second.value;
        }
    }
    auto value = std::make_shared<const SurfaceMaterialImageInspection>(InspectSurfaceMaterialImage(key, path, bytes));
    if (!hash.value) return value;
    std::lock_guard lock(mutex_);
    if (const auto found = entries_.find(id); found != entries_.end()) return found->second.value;
    order_.push_front(id);
    entries_.emplace(id, Entry{value, order_.begin()});
    while (entries_.size() > capacity_) { entries_.erase(order_.back()); order_.pop_back(); }
    return value;
}

void SurfaceMaterialImageCache::Clear() {
    std::lock_guard lock(mutex_);
    entries_.clear(); order_.clear();
}

std::size_t SurfaceMaterialValidation::Count(DiagnosticSeverity severity) const {
    std::size_t count = 0;
    for (const auto& item : table) count += item.severity == severity;
    for (const auto& type : types) for (const auto& item : type.diagnostics) count += item.severity == severity;
    return count;
}

SurfaceMaterialValidation ValidateSurfaceMaterials(const SurfaceMaterialTableDocument& document,
    std::string_view rawcode, const SurfaceMaterialValidationSources& sources) {
    SurfaceMaterialValidation out;
    out.table = document.Table().warnings;
    const auto sections = document.Sections();
    for (const auto& section : sections)
        if (section.headers > 1)
            out.table.push_back(Make(DiagnosticSeverity::Info, "SURFACE_MATERIAL_SECTION_MERGED",
                "[" + section.rawcode + "] 有 " + std::to_string(section.headers) + " 个同名节，读表时合并；改动写在最后出现该键的那一行。"));
    for (const auto& section : sections) {
        if (!rawcode.empty() && section.rawcode != rawcode) continue;
        auto& type = out.types.emplace_back();
        type.rawcode = section.rawcode;
        const auto shader = static_cast<std::size_t>(Render::FindSurfaceMaterialKey("shader") - Render::kSurfaceMaterialKeys.data());
        const bool water = section.values[shader] && *section.values[shader] == "water";
        const std::string prefix = "[" + section.rawcode + "] ";
        if (sources.typeInfo) {
            type.type = sources.typeInfo(section.rawcode);
            const auto& info = *type.type;
            if (info.category == SurfaceMaterialTypeCategory::Unknown)
                type.diagnostics.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_TYPE_UNKNOWN",
                    prefix + "不是地图里的装饰物或可破坏物类型。"));
            else if (info.category == SurfaceMaterialTypeCategory::Ambiguous)
                type.diagnostics.push_back(Make(DiagnosticSeverity::Info, "SURFACE_MATERIAL_TYPE_AMBIGUOUS",
                    prefix + "装饰物和可破坏物都有这个类型；WarVK 按摆放匹配，两类摆放都会用到。"));
            if (info.placements == 0)
                type.diagnostics.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_TYPE_UNPLACED",
                    prefix + "地图里没有这一类型的摆放，WarVK 不会用到。"));
            if (water && info.blendLayer && !*info.blendLayer)
                type.diagnostics.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_WATER_NOT_BLEND",
                    prefix + "水面材质的模型没有 Blend 层，游戏里保持原样显示。"));
        }
        if (!sources.readImage) continue;
        for (std::size_t i = 0; i < Render::kSurfaceMaterialKeys.size(); ++i) {
            const auto& key = Render::kSurfaceMaterialKeys[i];
            if (key.kind != Render::SurfaceMaterialKeyKind::Image || !section.values[i] || section.values[i]->empty()) continue;
            auto& report = type.images.emplace_back();
            report.key = std::string(key.name);
            report.path = *section.values[i];
            const auto where = prefix + std::string(Label(key.name)) + " " + report.path + "：";
            // 游戏一侧按扩展名决定拷不拷：不在之列的图在游戏里不生效，内容再对也没用。
            if (!Render::SurfaceMaterialImageExtensionAccepted(report.path)) {
                type.diagnostics.push_back(Make(DiagnosticSeverity::Error, "SURFACE_MATERIAL_IMAGE_EXTENSION",
                    where + "游戏一侧只拷扩展名为 .tga .png .jpg .jpeg .bmp 的图，这张在游戏里不生效。"));
                continue;
            }
            // 游戏一侧按表里的原始字节到地图里找文件；1.27a 里非 ASCII 路径能否找到没有验证过。
            if (std::any_of(report.path.begin(), report.path.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; }))
                type.diagnostics.push_back(Make(DiagnosticSeverity::Warning, "SURFACE_MATERIAL_IMAGE_PATH_NOT_ASCII",
                    where + "路径含非 ASCII 字符，游戏里能否找到这张图没有验证过；建议只用字母、数字和 _ - . \\。"));
            auto source = sources.readImage(report.path);
            report.state = source.state;
            if (source.state == State::Missing) {
                type.diagnostics.push_back(Make(DiagnosticSeverity::Error, "SURFACE_MATERIAL_IMAGE_MISSING",
                    where + "不在地图里，也不在待导入文件里。"));
                continue;
            }
            if (source.state == State::Failed || !source.bytes) {
                const auto reason = source.problems.empty() ? std::string("读取失败。") : source.problems.front().message;
                type.diagnostics.push_back(Make(DiagnosticSeverity::Error, "SURFACE_MATERIAL_IMAGE_UNREADABLE", where + reason));
                continue;
            }
            const std::span<const std::byte> bytes(source.bytes->data(), source.bytes->size());
            report.inspection = sources.cache ? sources.cache->Inspect(key.name, report.path, bytes)
                : std::make_shared<const SurfaceMaterialImageInspection>(InspectSurfaceMaterialImage(key.name, report.path, bytes));
            Prefix(type.diagnostics, report.inspection->diagnostics, where);
        }
    }
    return out;
}
}
