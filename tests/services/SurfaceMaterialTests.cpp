#include "War3MapCore/Render/SurfaceMaterialImage.h"
#include "War3MapCore/Services/SurfaceMaterialTableDocument.h"
#include "War3MapCore/Services/SurfaceMaterialValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using Render::SurfaceMaterialImageFormat;

void Require(bool condition, const std::string& text) { if (!condition) throw std::runtime_error(text); }
template<class T> bool HasCode(const std::vector<T>& items, std::string_view code) {
    return std::any_of(items.begin(), items.end(), [&](const auto& item) { return item.code == code; });
}
bool HasCode(const Status& status, std::string_view code) { return HasCode(status.diagnostics, code); }
std::size_t KeyIndex(std::string_view name) {
    return static_cast<std::size_t>(Render::FindSurfaceMaterialKey(name) - Render::kSurfaceMaterialKeys.data());
}

// 原创合成图：无色表 32 位 TGA，左上原点；像素按 RGBA 给出。
std::vector<std::byte> Tga(std::uint16_t width, std::uint16_t height, const std::vector<std::array<std::uint8_t, 4>>& pixels,
    std::uint8_t depth = 32) {
    std::vector<std::byte> out(18, std::byte{0});
    out[2] = std::byte{2};
    out[12] = std::byte(width & 0xFF); out[13] = std::byte(width >> 8);
    out[14] = std::byte(height & 0xFF); out[15] = std::byte(height >> 8);
    out[16] = std::byte{depth};
    out[17] = std::byte(depth == 32 ? 0x28 : 0x20);
    for (const auto& p : pixels) {
        out.push_back(std::byte{p[2]}); out.push_back(std::byte{p[1]}); out.push_back(std::byte{p[0]});
        if (depth == 32) out.push_back(std::byte{p[3]});
    }
    return out;
}
std::vector<std::byte> Filled(std::uint16_t width, std::uint16_t height, std::array<std::uint8_t, 4> pixel) {
    return Tga(width, height, std::vector<std::array<std::uint8_t, 4>>(static_cast<std::size_t>(width) * height, pixel));
}
std::vector<std::byte> Bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    for (const int value : values) out.push_back(std::byte(value));
    return out;
}
std::span<const std::byte> View(const std::vector<std::byte>& bytes) { return {bytes.data(), bytes.size()}; }

void DocumentRoundTripsAndMatchesParser() {
    const std::string text =
        "\xEF\xBB\xBF; 合成材质表\r\n"
        "[D001]\r\n"
        "normal = Synthetic\\rock_n.png ; 尾注释\r\n"
        "ortho = 1\n"
        "roughness = 9\n"
        "\n"
        "[XX]\r\n"
        "novalue\r\n"
        "[D001]\r\n"
        "orm=Synthetic\\rock_orm.png\r\n"
        "[W001]\r\n"
        "shader = water";
    const auto document = SurfaceMaterialTableDocument::Parse(text);
    Require(document.Text() == text, "原文逐字节保留");
    const auto expected = Render::ParseSurfaceMaterialTable(text);
    const auto table = document.Table();
    Require(table.byRawcode.size() == expected.byRawcode.size() && table.warnings.size() == expected.warnings.size(),
        "读表结果与解析器相同");
    const auto sections = document.Sections();
    Require(sections.size() == 2 && sections[0].rawcode == "D001" && sections[1].rawcode == "W001", "已接受类型按出现顺序");
    const auto& rock = sections[0];
    Require(rock.headers == 2 && rock.line == 2, "同名节计数与首行");
    Require(rock.values[KeyIndex("normal")] == std::optional<std::string>("Synthetic\\rock_n.png"), "图片值");
    Require(rock.values[KeyIndex("orm")] == std::optional<std::string>("Synthetic\\rock_orm.png"), "合并节的图片值");
    Require(!rock.values[KeyIndex("roughness")] && rock.invalid[KeyIndex("roughness")], "坏数值不生效并标记");
    Require(rock.unknownKeys == std::vector<std::string>{"ortho"}, "未知键");
    Require(sections[1].values[KeyIndex("shader")] == std::optional<std::string>("water"), "水面");
    Require(SurfaceMaterialTableDocument::Parse("").Text().empty() && SurfaceMaterialTableDocument::Parse("").Sections().empty(), "空表");
}

void SetEditsInPlace() {
    auto document = SurfaceMaterialTableDocument::Parse(
        "[D001]\r\n"
        "  normal = a.png ; 注释\r\n"
        "foo = 1\r\n"
        "; 段注释\r\n"
        "[D001]\r\n"
        "normal=b.png\r\n");
    Require(document.Set("D001", "normal", "c.png").Succeeded(), "改已有键");
    Require(document.Text() ==
        "[D001]\r\n  normal = a.png ; 注释\r\nfoo = 1\r\n; 段注释\r\n[D001]\r\nnormal=c.png\r\n", "只改最后一次出现的那一行");
    auto first = SurfaceMaterialTableDocument::Parse("[D001]\r\n  normal = a.png ; 注释\r\nfoo = 1\r\n");
    Require(first.Set("D001", "normal", "d.png").Succeeded(), "改带注释的行");
    Require(first.Text() == "[D001]\r\n  normal = d.png ; 注释\r\nfoo = 1\r\n", "行内注释与空白保留");
    Require(first.Set("D001", "roughness", "0.50").Succeeded(), "加新键");
    Require(first.Text() == "[D001]\r\n  normal = d.png ; 注释\r\n  roughness = 0.5\r\nfoo = 1\r\n",
        "新键在最后一个已知键之后，沿用缩进与“=”两侧空白，数值规整");
    Require(first.Set("D001", "specular", "+2").Succeeded() && first.Section("D001")->values[KeyIndex("specular")] == "2",
        "去掉加号");
    const auto before = first.Text();
    Require(HasCode(first.Set("D001", "roughness", "5"), "SURFACE_MATERIAL_VALUE_INVALID") && first.Text() == before, "超范围拒绝");
    Require(HasCode(first.Set("D001", "roughness", "abc"), "SURFACE_MATERIAL_VALUE_INVALID"), "非数拒绝");
    Require(HasCode(first.Set("D001", "normal", "x;y.png"), "SURFACE_MATERIAL_VALUE_INVALID"), "路径含注释符拒绝");
    Require(HasCode(first.Set("D001", "normal", ""), "SURFACE_MATERIAL_VALUE_INVALID"), "空路径拒绝");
    Require(HasCode(first.Set("D001", "normal", " a.png"), "SURFACE_MATERIAL_VALUE_INVALID"), "首尾空白拒绝");
    Require(HasCode(first.Set("D001", "normalY", "up"), "SURFACE_MATERIAL_VALUE_INVALID"), "选项拒绝");
    Require(HasCode(first.Set("D001", "ortho", "1"), "SURFACE_MATERIAL_KEY_UNKNOWN"), "未知键拒绝");
    Require(HasCode(first.Set("D0", "normal", "a.png"), "SURFACE_MATERIAL_RAWCODE_INVALID"), "坏类型拒绝");
    Require(first.Text() == before, "拒绝时原文不变");

    auto header = SurfaceMaterialTableDocument::Parse("[D001]\nfoo=1\n");
    Require(header.Set("D001", "normalY", "dx").Succeeded() && header.Text() == "[D001]\nnormalY=dx\nfoo=1\n",
        "没有已知键时紧跟节头，换行沿用 LF");
    auto append = SurfaceMaterialTableDocument::Parse("[D001]\nnormal=a.png");
    Require(append.Set("D002", "orm", "b.png").Succeeded() && append.Text() == "[D001]\nnormal=a.png\n\n[D002]\norm=b.png\n",
        "新类型加在表尾，空一行分隔");
    auto noFinal = SurfaceMaterialTableDocument::Parse("[D001]\nnormal=a.png");
    Require(noFinal.Set("D001", "normal", "b.png").Succeeded() && noFinal.Text() == "[D001]\nnormal=b.png", "末尾没有换行时保持");
    auto empty = SurfaceMaterialTableDocument::Parse("");
    Require(empty.Set("D001", "normal", "a.png").Succeeded() && empty.Text() == "[D001]\r\nnormal=a.png\r\n", "新表用 CRLF、无 BOM");
    auto bom = SurfaceMaterialTableDocument::Parse("\xEF\xBB\xBF[D001]\r\n");
    Require(bom.Set("D001", "metallic", "0").Succeeded() && bom.Text() == "\xEF\xBB\xBF[D001]\r\nmetallic=0\r\n", "BOM 保留");
    auto emptyValue = SurfaceMaterialTableDocument::Parse("[D001]\nnormal =  \n");
    Require(emptyValue.Set("D001", "normal", "a.png").Succeeded() && emptyValue.Text() == "[D001]\nnormal =a.png  \n",
        "空值插在“=”之后");
}

void UnsetAndRemove() {
    auto document = SurfaceMaterialTableDocument::Parse(
        "[D001]\nnormal=a.png ; 旧图\norm=o.png\n[D001]\nnormal=b.png\n\n; 第二种\n[D002]\nnormal=c.png");
    Require(document.Unset("D001", "normal").Succeeded(), "删键");
    Require(document.Text() == "[D001]\n; 旧图\norm=o.png\n[D001]\n\n; 第二种\n[D002]\nnormal=c.png", "所有同名节的该键都删，注释留作一行");
    Require(document.Unset("D009", "normal").Succeeded() && HasCode(document.Unset("D001", "ortho"), "SURFACE_MATERIAL_KEY_UNKNOWN"),
        "没写过的类型不变，未知键拒绝");
    Require(document.RemoveSection("D001").Succeeded(), "删节");
    Require(document.Text() == "; 第二种\n[D002]\nnormal=c.png", "删去两处同名节，紧挨下一节头的注释保留");
    Require(HasCode(document.RemoveSection("D001"), "SURFACE_MATERIAL_SECTION_MISSING"), "删不存在的节报错");
    Require(document.RemoveSection("D002").Succeeded() && document.Text() == "; 第二种", "删最后一节，原文末尾没有换行");
    auto last = SurfaceMaterialTableDocument::Parse("[D001]\nnormal=a.png\norm=b.png");
    Require(last.Unset("D001", "orm").Succeeded() && last.Text() == "[D001]\nnormal=a.png", "删最后一行后仍不加末尾换行");
}

void TableFull() {
    std::string text;
    for (int i = 0; i < 257; ++i) {
        char code[5]; std::snprintf(code, sizeof(code), "T%03d", i);
        text += "[" + std::string(code) + "]\nnormal=a.png\n";
    }
    auto document = SurfaceMaterialTableDocument::Parse(text);
    Require(document.Sections().size() == 256, "第 257 种被忽略");
    Require(HasCode(document.Set("T256", "orm", "b.png"), "SURFACE_MATERIAL_TABLE_FULL"), "第 257 种不能编辑");
    Require(HasCode(document.Set("Z000", "orm", "b.png"), "SURFACE_MATERIAL_TABLE_FULL"), "满表不能加新类型");
    Require(document.Set("T000", "orm", "b.png").Succeeded(), "已有类型照常编辑");
}

void HeadersAndPixels() {
    using Render::ReadSurfaceMaterialImageHeader;
    const auto png = Bytes({0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0x04, 0, 0, 0, 0x02, 0});
    auto header = ReadSurfaceMaterialImageHeader("a.png", View(png));
    Require(header.format == SurfaceMaterialImageFormat::Png && header.width == 1024 && header.height == 512 && header.gameReadable, "PNG 头");
    const auto jpeg = Bytes({0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 0, 0, 0xFF, 0xC0, 0, 11, 8, 0, 64, 0, 128, 3});
    header = ReadSurfaceMaterialImageHeader("a.jpg", View(jpeg));
    Require(header.format == SurfaceMaterialImageFormat::Jpeg && header.width == 128 && header.height == 64, "JPEG 帧头");
    const auto bmp = Bytes({'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 40, 0, 0, 0, 0x20, 0, 0, 0, 0xF0, 0xFF, 0xFF, 0xFF});
    header = ReadSurfaceMaterialImageHeader("a.bmp", View(bmp));
    Require(header.format == SurfaceMaterialImageFormat::Bmp && header.width == 32 && header.height == 16, "BMP 头（自上而下）");
    const auto gif = Bytes({'G', 'I', 'F', '8', '9', 'a', 8, 0, 4, 0});
    header = ReadSurfaceMaterialImageHeader("a.gif", View(gif));
    Require(header.format == SurfaceMaterialImageFormat::Gif && header.width == 8 && header.height == 4 && !header.gameReadable,
        "GIF 头；游戏一侧不拷 GIF");
    const auto tiff = Bytes({'I', 'I', 42, 0, 8, 0, 0, 0, 2, 0, 0, 1, 3, 0, 1, 0, 0, 0, 64, 0, 0, 0, 1, 1, 4, 0, 1, 0, 0, 0, 32, 0, 0, 0});
    header = ReadSurfaceMaterialImageHeader("a.tif", View(tiff));
    Require(header.format == SurfaceMaterialImageFormat::Tiff && header.width == 64 && header.height == 32 && !header.gameReadable,
        "TIFF 头；游戏一侧不拷 TIFF");
    for (const auto* path : {"a.png", "A.PNG", "x\\b.Jpg", "c.jpeg", "d.BMP", "e.tga"})
        Require(Render::SurfaceMaterialImageExtensionAccepted(path), std::string("游戏一侧拷 ") + path);
    for (const auto* path : {"a.gif", "a.tif", "a.tiff", "a.dds", "a.blp", "png", "a.png\\b", "a."})
        Require(!Render::SurfaceMaterialImageExtensionAccepted(path), std::string("游戏一侧不拷 ") + path);
    header = ReadSurfaceMaterialImageHeader("a.tga", View(Filled(3, 5, {1, 2, 3, 4})));
    Require(header.format == SurfaceMaterialImageFormat::Tga && header.width == 3 && header.height == 5 && header.gameReadable, "TGA 头");
    header = ReadSurfaceMaterialImageHeader("a.TGA", View(Tga(1, 1, {{0, 0, 0, 0}}, 16)));
    Require(header.format == SurfaceMaterialImageFormat::Tga && !header.gameReadable, "16 位 TGA 读不了");
    Require(ReadSurfaceMaterialImageHeader("a.png", View(Bytes({'B', 'L', 'P', '1'}))).format == SurfaceMaterialImageFormat::Blp, "BLP");
    Require(ReadSurfaceMaterialImageHeader("a.dds", View(Bytes({'D', 'D', 'S', ' '}))).format == SurfaceMaterialImageFormat::Dds, "DDS");
    Require(ReadSurfaceMaterialImageHeader("a.png", View(Bytes({1, 2, 3}))).format == SurfaceMaterialImageFormat::Unknown, "认不出");

    const std::vector<std::uint8_t> rgba{0, 100, 25, 255, 255, 100, 24, 255};
    const auto stats = Render::MeasureSurfaceMaterialPixels(rgba);
    Require(stats.pixels == 2 && stats.metalTexels == 1, "像素与金属计数");
    Require(stats.channels[0].minimum == 0.0f && stats.channels[0].maximum == 1.0f && std::abs(stats.channels[0].mean - 0.5f) < 1e-6f
        && std::abs(stats.channels[0].deviation - 0.5f) < 1e-6f && stats.channels[1].deviation == 0.0f, "通道统计");
    Require(std::abs(stats.brightnessMean - (0.2126f * 0.5f + 0.7152f * 100.0f / 255.0f + 0.0722f * 24.5f / 255.0f)) < 1e-5f, "Rec.709 亮度");
    Require(Render::SurfaceMaterialDetailMean(stats, 0.0f) == Render::kSurfaceMaterialDetailMeanFloor
        && Render::SurfaceMaterialDetailMean(Render::SurfaceMaterialPixelStats{}, 0.3f) == 1.0f, "细节均值下限与空图");
}

void InspectImages() {
    const auto flat = InspectSurfaceMaterialImage("normal", "n.tga", View(Filled(4, 4, {128, 128, 255, 255})));
    Require(flat.stats && HasCode(flat.diagnostics, "SURFACE_MATERIAL_NORMAL_FLAT")
        && !HasCode(flat.diagnostics, "SURFACE_MATERIAL_NORMAL_NOT_TANGENT") && flat.sha256.size() == 64, "平法线图");
    std::vector<std::array<std::uint8_t, 4>> tilted;
    for (int i = 0; i < 16; ++i) tilted.push_back({static_cast<std::uint8_t>(220 + i), static_cast<std::uint8_t>(120 + i), 255, 255});
    const auto object = InspectSurfaceMaterialImage("detailNormal", "n.tga", View(Tga(4, 4, tilted)));
    Require(HasCode(object.diagnostics, "SURFACE_MATERIAL_NORMAL_NOT_TANGENT") && !HasCode(object.diagnostics, "SURFACE_MATERIAL_NORMAL_FLAT"),
        "R 平均值离 0.5 太远");
    const auto constant = InspectSurfaceMaterialImage("orm", "o.tga", View(Filled(4, 4, {255, 5, 0, 255})));
    Require(HasCode(constant.diagnostics, "SURFACE_MATERIAL_ORM_CONSTANT") && HasCode(constant.diagnostics, "SURFACE_MATERIAL_ORM_GLOSSY")
        && !HasCode(constant.diagnostics, "SURFACE_MATERIAL_ORM_METAL"), "常数 ORM、近镜面");
    // 金属：B > 24 的像素达到千分之一（1000 像素中 1 个）时提醒；B = 24 或 2000 像素中 1 个都不提醒。
    const auto metal = [](std::uint16_t width, std::uint8_t blue, int count) {
        std::vector<std::array<std::uint8_t, 4>> pixels(width, {255, static_cast<std::uint8_t>(128), 0, 255});
        for (int i = 0; i < count; ++i) pixels[static_cast<std::size_t>(i)] = {255, 128, blue, 255};
        return HasCode(InspectSurfaceMaterialImage("orm", "o.tga", View(Tga(width, 1, pixels))).diagnostics, "SURFACE_MATERIAL_ORM_METAL");
    };
    Require(metal(1000, 25, 1) && !metal(1000, 24, 1) && !metal(2000, 25, 1) && metal(2000, 25, 2), "金属阈值与宿主相同");
    const auto dark = InspectSurfaceMaterialImage("detailAlbedo", "d.tga", View(Filled(2, 2, {0, 0, 0, 255})));
    Require(HasCode(dark.diagnostics, "SURFACE_MATERIAL_DETAIL_MEAN_LOW"), "细节颜色平均亮度近 0");
    const auto darkOrm = InspectSurfaceMaterialImage("detailOrm", "d.tga", View(Filled(2, 2, {200, 1, 0, 255})));
    Require(HasCode(darkOrm.diagnostics, "SURFACE_MATERIAL_DETAIL_MEAN_LOW"), "细节 ORM 平均粗糙度近 0");
    const auto odd = InspectSurfaceMaterialImage("detailAlbedo", "d.tga", View(Filled(3, 5, {90, 90, 90, 255})));
    Require(HasCode(odd.diagnostics, "SURFACE_MATERIAL_IMAGE_NOT_POWER_OF_TWO") && odd.diagnostics.size() == 1, "不是 2 的幂只是信息");
    const auto large = InspectSurfaceMaterialImage("normal", "l.tga", View(Tga(2049, 2, {})));
    Require(HasCode(large.diagnostics, "SURFACE_MATERIAL_IMAGE_LARGE") && HasCode(large.diagnostics, "SURFACE_MATERIAL_IMAGE_DECODE"),
        "超过 2048 提醒；截断的图解不开");
    const auto huge = InspectSurfaceMaterialImage("normal", "l.tga", View(Tga(8193, 1, {})));
    Require(std::any_of(huge.diagnostics.begin(), huge.diagnostics.end(), [](const auto& d) {
        return d.code == "SURFACE_MATERIAL_IMAGE_TOO_LARGE" && d.severity == DiagnosticSeverity::Error; }), "超过 8192 是错误");
    const auto blp = InspectSurfaceMaterialImage("normal", "n.blp", View(Bytes({'B', 'L', 'P', '1', 0, 0, 0, 0})));
    Require(blp.diagnostics.size() == 1 && blp.diagnostics[0].code == "SURFACE_MATERIAL_IMAGE_FORMAT"
        && blp.diagnostics[0].severity == DiagnosticSeverity::Error, "BLP 游戏一侧读不了");
    const auto bmp = InspectSurfaceMaterialImage("orm", "o.bmp",
        View(Bytes({'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 40, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0})));
    Require(HasCode(bmp.diagnostics, "SURFACE_MATERIAL_IMAGE_NOT_MEASURED") && !bmp.stats, "BMP 编辑器不统计");
    const auto jpeg = InspectSurfaceMaterialImage("normal", "n.jpg", View(Bytes({0xFF, 0xD8, 0xFF, 0xD9})));
    Require(HasCode(jpeg.diagnostics, "SURFACE_MATERIAL_IMAGE_JPEG"), "法线用 JPEG 给信息");
    const auto albedoJpeg = InspectSurfaceMaterialImage("detailAlbedo", "n.jpg", View(Bytes({0xFF, 0xD8, 0xFF, 0xD9})));
    Require(!HasCode(albedoJpeg.diagnostics, "SURFACE_MATERIAL_IMAGE_JPEG"), "细节颜色用 JPEG 不提醒");
}

void ValidateTable() {
    const auto document = SurfaceMaterialTableDocument::Parse(
        "[D001]\nnormal=Synthetic\\missing.png\n"
        "[D002]\nnormal=Synthetic\\locked.png\n"
        "[D003]\nnormal=Synthetic\\flat.tga\n"
        "[D003]\norm=Synthetic\\flat.tga\n"
        "[W001]\nshader=water\n");
    const auto flat = std::make_shared<const std::vector<std::byte>>(Filled(4, 4, {128, 128, 255, 255}));
    int reads = 0;
    SurfaceMaterialImageCache cache;
    SurfaceMaterialValidationSources sources;
    sources.readImage = [&](std::string_view path) {
        ++reads;
        SurfaceMaterialImageSource source;
        if (path == "Synthetic\\flat.tga") { source.state = SurfaceMaterialImageSource::State::Found; source.bytes = flat; }
        else if (path == "Synthetic\\locked.png") {
            source.state = SurfaceMaterialImageSource::State::Failed;
            source.problems.push_back({DiagnosticSeverity::Error, "Test", "LOCKED", "合成读取失败。", std::nullopt});
        }
        return source;
    };
    sources.typeInfo = [](std::string_view rawcode) {
        SurfaceMaterialTypeInfo info;
        if (rawcode == "D001" || rawcode == "D003") { info.category = SurfaceMaterialTypeCategory::Doodad; info.placements = 3; }
        if (rawcode == "W001") { info.category = SurfaceMaterialTypeCategory::Destructible; info.placements = 1; info.blendLayer = false; }
        return info;
    };
    sources.cache = &cache;
    const auto all = ValidateSurfaceMaterials(document, {}, sources);
    Require(all.types.size() == 4 && reads == 4, "整表四种类型、四张图");
    Require(HasCode(all.table, "SURFACE_MATERIAL_SECTION_MERGED"), "同名节信息");
    const auto find = [&](std::string_view rawcode) -> const SurfaceMaterialTypeValidation& {
        return *std::find_if(all.types.begin(), all.types.end(), [&](const auto& type) { return type.rawcode == rawcode; });
    };
    Require(HasCode(find("D001").diagnostics, "SURFACE_MATERIAL_IMAGE_MISSING"), "图片不存在");
    Require(HasCode(find("D002").diagnostics, "SURFACE_MATERIAL_IMAGE_UNREADABLE") && HasCode(find("D002").diagnostics, "SURFACE_MATERIAL_TYPE_UNKNOWN")
        && HasCode(find("D002").diagnostics, "SURFACE_MATERIAL_TYPE_UNPLACED"), "读不了、类型未知、没有摆放");
    const auto& d003 = find("D003");
    Require(d003.images.size() == 2 && d003.images[0].inspection && d003.images[1].inspection
        && d003.images[0].inspection != d003.images[1].inspection, "同一张图按键分别检查");
    Require(HasCode(d003.diagnostics, "SURFACE_MATERIAL_NORMAL_FLAT") && HasCode(d003.diagnostics, "SURFACE_MATERIAL_ORM_CONSTANT"),
        "图片诊断带进类型");
    Require(d003.diagnostics[0].message.starts_with("[D003] 法线 Synthetic\\flat.tga："), "诊断带类型、键名与路径");
    Require(HasCode(find("W001").diagnostics, "SURFACE_MATERIAL_WATER_NOT_BLEND") && find("W001").images.empty(), "水面没有 Blend 层");
    Require(all.Count(DiagnosticSeverity::Error) == 2, "两个错误：图片不存在、读不了");
    const auto again = ValidateSurfaceMaterials(document, "D003", sources);
    Require(again.types.size() == 1 && again.types[0].images[0].inspection == d003.images[0].inspection, "只校验一种类型；缓存命中");
    const auto bare = ValidateSurfaceMaterials(document, "D001", {});
    Require(bare.types.size() == 1 && bare.types[0].diagnostics.empty() && !bare.types[0].type, "不给来源时不查图片与类型");
}

// 游戏一侧（WarVK 拷图）的规则：扩展名不在之列的图不生效；非 ASCII 路径未验证；删去最后一张图整节失效。
void GameSideRules() {
    const auto document = SurfaceMaterialTableDocument::Parse(
        "[D001]\nnormal=Synthetic\\a.gif\n"
        "[D002]\nnormal=Synthetic\\\xE7\x9F\xB3.tga\n");
    const auto flat = std::make_shared<const std::vector<std::byte>>(Filled(2, 2, {128, 128, 255, 255}));
    int reads = 0;
    SurfaceMaterialValidationSources sources;
    sources.readImage = [&](std::string_view) {
        ++reads;
        return SurfaceMaterialImageSource{SurfaceMaterialImageSource::State::Found, flat, {}};
    };
    const auto checked = ValidateSurfaceMaterials(document, {}, sources);
    Require(checked.types.size() == 2 && reads == 1, "扩展名不在之列时不读图");
    Require(HasCode(checked.types[0].diagnostics, "SURFACE_MATERIAL_IMAGE_EXTENSION")
        && checked.Count(DiagnosticSeverity::Error) == 1, "GIF 在游戏里不生效是错误");
    Require(HasCode(checked.types[1].diagnostics, "SURFACE_MATERIAL_IMAGE_PATH_NOT_ASCII"), "非 ASCII 路径提醒");

    auto edited = SurfaceMaterialTableDocument::Parse("[D001]\nnormal=a.png\norm=b.png\nroughness=2\n");
    const auto before = edited;
    Require(edited.Unset("D001", "normal").Succeeded() && !SurfaceMaterialStopsApplying(before, edited, "D001"), "还剩一张图仍生效");
    const auto one = edited;
    Require(edited.Unset("D001", "orm").Succeeded(), "删去最后一张图");
    const auto stops = SurfaceMaterialStopsApplying(one, edited, "D001");
    Require(stops && stops->code == "SURFACE_MATERIAL_SECTION_STOPS_APPLYING" && stops->message.starts_with("[D001]"),
        "删去最后一张图提醒不再生效");
    auto water = edited;
    Require(water.Set("D001", "shader", "water").Succeeded() && !SurfaceMaterialStopsApplying(one, water, "D001"),
        "水面没有图也生效");
    Require(SurfaceMaterialStopsApplying(water, edited, "D001").has_value(), "水面改回表面且没有图时提醒");
    auto removed = one;
    Require(removed.RemoveSection("D001").Succeeded() && !SurfaceMaterialStopsApplying(one, removed, "D001"), "整节删除不提醒");
}
}

int main() {
    try {
        DocumentRoundTripsAndMatchesParser();
        SetEditsInPlace();
        UnsetAndRemove();
        TableFull();
        HeadersAndPixels();
        InspectImages();
        ValidateTable();
        GameSideRules();
        std::cout << "SurfaceMaterialTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
