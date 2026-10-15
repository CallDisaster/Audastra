#include "ResourceCheckCommand.h"
#include "MapDataFixtures.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Render/MdxModel.h"

#include <Windows.h>

#include <atomic>
#include <charconv>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using Row = std::vector<std::string>;
using Tally = std::map<std::string, int, std::less<>>;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename T>
T Take(Result<T> result) {
    if (!result.Succeeded()) {
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        throw std::runtime_error("original resource-check fixture was rejected");
    }
    return std::move(*result.value);
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char character : text) bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}

std::string Slk(const Row& headers, const std::vector<Row>& rows) {
    std::string text = "ID;PWXL\n";
    const auto append = [&](const Row& row, std::size_t y) {
        Require(row.size() == headers.size(), "original SLK row has the declared columns");
        for (std::size_t x = 0; x < row.size(); ++x) {
            std::string quoted;
            for (const char character : row[x]) {
                quoted += character;
                if (character == '"') quoted += '"';
            }
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y)
                + ";K\"" + quoted + "\"\n";
        }
    };
    append(headers, 1);
    for (std::size_t y = 0; y < rows.size(); ++y) append(rows[y], y + 2);
    return text + "E\n";
}

class TempDirectory final {
public:
    TempDirectory() : parent_(std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal()) {
        static std::atomic<unsigned> sequence{0};
        path_ = parent_ / ("Audastra-ResourceCheck-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "create an exclusive original fixture directory");
    }
    ~TempDirectory() noexcept {
        try {
            if (std::uncaught_exceptions() != 0) {
                std::cerr << "retained-fixture=" << path_.string() << '\n';
                return;
            }
            const auto absolute = std::filesystem::absolute(path_).lexically_normal();
            if (absolute.parent_path() != parent_
                || !absolute.filename().string().starts_with("Audastra-ResourceCheck-")) return;
            std::error_code error;
            std::filesystem::remove_all(absolute, error);
        } catch (...) {
            // 仅清理本测试独占的临时目录；清理异常不遮住业务验证结果。
        }
    }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path parent_, path_;
};

// 最小原创经典 W3E v11：2x2 顶点、一种地表与悬崖、无水面标记。
std::vector<std::byte> Terrain() {
    MapDataFixtures::Fixture writer;
    writer.Raw("W3E!"); writer.UInt(11); writer.Byte('L'); writer.UInt(1);
    writer.UInt(1); writer.Raw("Ldrt"); writer.UInt(1); writer.Raw("Clf1");
    writer.UInt(2); writer.UInt(2); writer.Float(0); writer.Float(0);
    for (int index = 0; index < 4; ++index) {
        writer.Byte(0); writer.Byte(32); // groundHeightRaw=8192。
        writer.Byte(0); writer.Byte(32); // waterHeightAndFlagsRaw=8192。
        writer.Byte(0); writer.Byte(0); writer.Byte(2);
    }
    return std::move(writer.bytes);
}

std::vector<std::byte> GroundTexture() {
    Image::DecodedImage image;
    image.width = 8; image.height = 8;
    for (std::size_t index = 0; index < 64; ++index) {
        image.rgba.push_back(std::byte{48}); image.rgba.push_back(std::byte{96});
        image.rgba.push_back(std::byte{144}); image.rgba.push_back(std::byte{255});
    }
    return Take(Image::EncodeBlpPaletted(image));
}

// 资源检查只解析模型与其贴图引用；此原创 MDX 不用于渲染验收。
std::vector<std::byte> TextureReferenceModel() {
    constexpr std::string_view texture = "Textures/Missing.blp";
    MapDataFixtures::Fixture writer;
    writer.Raw("MDLX"); writer.Raw("VERS"); writer.UInt(4); writer.UInt(800);
    writer.Raw("TEXS"); writer.UInt(268); writer.UInt(0);
    for (const unsigned char character : texture) writer.Byte(character);
    for (std::size_t index = texture.size(); index < 260; ++index) writer.Byte(0);
    writer.UInt(0);
    auto model = Take(Render::ParseMdx(writer.bytes));
    Require(model.textures.size() == 1 && model.textures.front().path == texture,
        "original MDX reaches the existing texture inspection path");
    return std::move(writer.bytes);
}

void WriteGame(const std::filesystem::path& root) {
    Require(std::filesystem::create_directory(root), "create an owned game-data fixture directory");
    std::vector<Mpq::MpqBuildFile> files;
    const auto add = [&](std::string name, std::string text) {
        files.push_back({std::move(name), Bytes(text), false});
    };
    // 七类默认主表均真实存在，避免类别来源缺席掩盖摆放分类行为。
    add("Units/UnitData.slk", Slk({"alias", "Name"}, {
        {"u001", "Fixed original unit"}, {"u002", "Missing model unit"},
        {"u003", "Missing texture unit"}, {"u004", "Malformed model unit"}}));
    add("Units/UnitUI.slk", Slk({"alias", "file"}, {
        {"u001", "_"}, {"u002", "Models/Missing.mdx"},
        {"u003", "Models/TextureReference.mdx"}, {"u004", "Models/Malformed.mdx"}}));
    for (const auto path : {"Units/UnitBalance.slk", "Units/UnitWeapons.slk", "Units/UnitAbilities.slk",
        "Units/ItemData.slk", "Units/AbilityData.slk", "Units/AbilityBuffData.slk",
        "Units/UpgradeData.slk", "Doodads/Doodads.slk", "Units/DestructableData.slk"})
        add(path, Slk({"alias"}, {}));

    const Row metadata{"ID", "field", "slk", "type", "useUnit", "useHero", "useBuilding", "useItem", "useCreep"};
    add("Units/UnitMetaData.slk", Slk(metadata, {
        {"umdl", "file", "UnitUI", "model", "1", "1", "1", "0", "1"},
        {"ifil", "file", "ItemData", "model", "0", "0", "0", "1", "0"}}));
    add("Doodads/DoodadMetaData.slk", Slk(metadata, {
        {"dfil", "file", "Doodads", "model", "1", "1", "1", "1", "1"}}));
    add("Units/DestructableMetaData.slk", Slk(metadata, {
        {"bfil", "file", "DestructableData", "model", "1", "1", "1", "1", "1"}}));
    for (const auto path : {"Units/AbilityMetaData.slk", "Units/AbilityBuffMetaData.slk", "Units/UpgradeMetaData.slk"})
        add(path, Slk(metadata, {}));

    add("TerrainArt/Terrain.slk", Slk({"tileID", "dir", "file"}, {{"Ldrt", "Original", "Ground"}}));
    add("TerrainArt/CliffTypes.slk", Slk(
        {"cliffID", "texDir", "texFile", "cliffModelDir", "rampModelDir", "groundTile", "upperTile", "cliffClass"},
        {{"Clf1", "Original", "Ground", "Original/Cliffs", "Original/Ramps", "Ldrt", "_", "c1"}}));
    add("TerrainArt/Water.slk", Slk({
        "waterID", "height", "texFile", "numTex", "texRate", "texOffset", "cells", "alphaMode",
        "lighting", "impassable", "mmRed", "mmGreen", "mmBlue", "mmAlpha",
        "Smin_R", "Smin_G", "Smin_B", "Smin_A", "Smax_R", "Smax_G", "Smax_B", "Smax_A",
        "Dmin_R", "Dmin_G", "Dmin_B", "Dmin_A", "Dmax_R", "Dmax_G", "Dmax_B", "Dmax_A"}, {{
        "LSha", "0", "Original/Water", "1", "1", "0", "1", "0", "1", "0",
        "0", "0", "0", "255", "0", "0", "0", "255", "0", "0", "0", "255",
        "0", "0", "0", "255", "0", "0", "0", "255"}}));
    files.push_back({"Original/Ground.blp", GroundTexture(), false});
    files.push_back({"Original/Water00.blp", GroundTexture(), false});
    files.push_back({"Models/TextureReference.mdx", TextureReferenceModel(), false});
    files.push_back({"Models/Malformed.mdx", Bytes("broken original model"), false});
    Require(Mpq::WriteMpqArchive(root / "war3.mpq", files).Succeeded(), "write the original game-data MPQ");
}

void WriteMap(const std::filesystem::path& path) {
    MapData::UnitPlacements units;
    std::int32_t creationId = 1;
    const auto add = [&](std::string rawcode, MapData::RandomUnitConfig random) {
        MapData::UnitPlacement unit;
        unit.rawcode = std::move(rawcode); unit.creationId = creationId++; unit.flags = 2;
        unit.scale = {1.f, 1.f, 1.f}; unit.random = std::move(random);
        units.units.push_back(std::move(unit));
    };
    for (const auto marker : {"uDNR", "bDNR", "iDNR"}) {
        add(marker, std::monostate{});
        add(marker, MapData::RandomAnyUnit{1, 0});
    }
    add("u001", std::monostate{});
    add("u001", MapData::RandomAnyUnit{1, 0}); // mode=0 默认配置仍是普通固定对象。
    add("n999", std::monostate{});
    add("sloc", std::monostate{});
    for (const auto rawcode : {"u002", "u003", "u004"}) add(rawcode, std::monostate{});

    MapData::DoodadPlacements doodads;
    MapData::DoodadPlacement sameName;
    sameName.rawcode = "uDNR"; sameName.creationId = 100; sameName.scale = {1.f, 1.f, 1.f};
    doodads.doodads.push_back(std::move(sameName)); // Doodad 调用域不获得随机单位身份。
    std::vector<Mpq::MpqBuildFile> files{
        {"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes, false},
        {"war3map.w3e", Terrain(), false},
        {"war3mapUnits.doo", Take(units.Serialize()), false},
        {"war3map.doo", Take(doodads.Serialize()), false}};
    Require(Mpq::WriteMpqArchive(path, files).Succeeded(), "write the original resource-check map MPQ");
}

class CapturedStreams final {
public:
    CapturedStreams() : previousOut_(std::cout.rdbuf(out_.rdbuf())), previousErr_(std::cerr.rdbuf(err_.rdbuf())) {}
    ~CapturedStreams() { std::cout.rdbuf(previousOut_); std::cerr.rdbuf(previousErr_); }
    std::string Out() const { return out_.str(); }
    std::string Err() const { return err_.str(); }
    CapturedStreams(const CapturedStreams&) = delete;
    CapturedStreams& operator=(const CapturedStreams&) = delete;
private:
    std::ostringstream out_, err_;
    std::streambuf* previousOut_;
    std::streambuf* previousErr_;
};

Tally Summary(std::string_view output) {
    std::istringstream input{std::string(output)};
    std::string line;
    bool found = false;
    Tally result;
    while (std::getline(input, line)) {
        if (line == "[汇总]") { Require(!found, "command emits one real summary"); found = true; continue; }
        if (!found) continue;
        const auto tab = line.find('\t');
        Require(line.starts_with("  ") && tab != std::string::npos, "summary line has the production format");
        int count = 0;
        const auto number = std::string_view(line).substr(tab + 1);
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), count);
        Require(parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && count > 0,
            "summary retains positive exact counts");
        Require(result.emplace(line.substr(2, tab - 2), count).second, "summary categories are unique");
    }
    Require(found, "real command reaches its final summary after all prerequisite gates");
    return result;
}

std::size_t Occurrences(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0, position = 0;
    while ((position = haystack.find(needle, position)) != std::string_view::npos) {
        ++count; position += needle.size();
    }
    return count;
}

void RealCommandPreservesResourceErrorsAndPlacementBoundaries() {
    TempDirectory directory;
    const auto map = directory.Path() / "source.w3x";
    const auto game = directory.Path() / "game";
    WriteGame(game); WriteMap(map);
    const auto mapSha = Take(Sha256File(map)), gameSha = Take(Sha256File(game / "war3.mpq"));
    std::string output, error;
    int exitCode = -1;
    {
        CapturedStreams captured;
        exitCode = RunResourceCheck(map, game); // 同一生产命令；没有复制 classify 或伪造诊断。
        output = captured.Out(); error = captured.Err();
    }
    try {
        Require(exitCode == 0 && error.empty(), "complete original inputs reach the existing exit-0 command contract");
        const Tally expected{
            {"PLACEMENT_RANDOM_UNSUPPORTED", 6}, {"PLACEMENT_OBJECT_UNDEFINED", 2},
            {"MODEL_RESOURCE_NOT_FOUND", 1}, {"MODEL_PARSE_MDX_MAGIC", 1}, {"TEXTURE_RESOURCE_NOT_FOUND", 1},
            {"ground-ok/游戏", 1}, {"model-ok/游戏", 1}, {"model-empty", 1}, {"placement-start-location", 1}};
        const auto tally = Summary(output);
        Require(tally == expected, "exact real summary preserves each unsupported placement and independent resource error");
        for (const auto marker : {"uDNR", "bDNR", "iDNR"}) {
            Require(Occurrences(output, std::string("  失败 ") + marker + " 随机摆放占位；") == 2,
                "each reserved marker fails once per serialized placement in both configuration forms");
        }
        Require(Occurrences(output, "  失败 n999\n") == 1 && Occurrences(output, "  失败 uDNR\n") == 1,
            "ordinary unknown and same-name doodad retain ordinary undefined diagnostics");
        Require(output.find("摆放 13，已分类 5；待查对象 4") != std::string::npos,
            "random rows remain placed but unclassified; fixed default-config rows remain classified");
        std::istringstream lines(output);
        std::string line;
        int failureLines = 0;
        while (std::getline(lines, line)) if (line.starts_with("  失败 ")) ++failureLines;
        int errorTotal = 0;
        for (const auto& [code, count] : tally) {
            if (code.starts_with("ground-ok/") || code.starts_with("model-ok/") || code.starts_with("texture-ok/")
                || code == "model-empty" || code == "model-variant-fallback" || code == "placement-start-location") continue;
            errorTotal += count;
        }
        Require(errorTotal == 11 && failureLines == errorTotal,
            "unsupported random placements stay errors and every error has one production failure line");
        Require(Take(Sha256File(map)) == mapSha && Take(Sha256File(game / "war3.mpq")) == gameSha,
            "real command preserves the original source-map and game-archive bytes");
    } catch (...) {
        std::cerr << "actual resource-check stdout:\n" << output << "actual stderr:\n" << error;
        throw;
    }
}
} // namespace

int main() {
    try {
        RealCommandPreservesResourceErrorsAndPlacementBoundaries();
        std::cout << "Resource check command tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
