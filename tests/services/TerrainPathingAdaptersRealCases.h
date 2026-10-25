#pragma once

#include "../../src/Cli/MapEditSaveCommand.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace War3Map::TerrainSaveTests {
namespace PathingAdaptersRealDetail {
inline void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& diagnostic : result.diagnostics)
        message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message.empty() ? "missing actual CLI pathing save result" : message);
}
struct Invocation {
    int exitCode;
    std::string output, diagnostics;
};
inline Invocation Invoke(std::vector<std::wstring> arguments) {
    std::vector<wchar_t*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    std::ostringstream output, diagnostics;
    auto* previousOutput = std::cout.rdbuf(output.rdbuf());
    auto* previousError = std::cerr.rdbuf(diagnostics.rdbuf());
    try {
        const auto exitCode = RunMapEditSave(static_cast<int>(argv.size()), argv.data());
        std::cout.rdbuf(previousOutput); std::cerr.rdbuf(previousError);
        return {exitCode, output.str(), diagnostics.str()};
    } catch (...) {
        std::cout.rdbuf(previousOutput); std::cerr.rdbuf(previousError); throw;
    }
}
inline std::wstring Ascii(std::string_view value) { return {value.begin(), value.end()}; }
inline std::vector<std::byte> ExpectedStartMarkers(const MapArchiveSession& source,
    const MapData::TerrainGridSnapshot& terrain, const std::filesystem::path& game) {
    const auto settings = Take(MapData::MapSettings::Parse(Take(source.ReadMember("war3map.w3i"))));
    const auto units = Take(MapData::ParseUnitPlacements(Take(source.ReadMember("war3mapUnits.doo"))));
    Require(settings.FullyParsed() && settings.formatVersion == 25 && (settings.flags & 0x400u)
        && settings.cameraMargins == std::array<std::int32_t, 4>{6, 6, 4, 8}
        && settings.playableWidth == 52 && settings.playableHeight == 52
        && terrain.header.offset == std::array<float, 2>{-4096, -4096},
        "independent MMP oracle requires the complete actual authored W3I playable rectangle and terrain origin");
    Require(units.units.size() == 1 && units.units[0].rawcode == "sloc" && units.units[0].owner == 0
        && units.units[0].position == std::array<float, 3>{0, 0, 0}
        && settings.players.size() == 1 && settings.players[0].number == 0
        && settings.players[0].startX == 0 && settings.players[0].startY == 0,
        "actual original placements contain exactly the authored player0 start marker, not an empty unit collection");
    auto mounted = Take(Game::GameDataSource::MountStrict(game, terrain.header.mainTileset));
    const auto skins = Take(mounted.ReadFileBoundedWithSource("UI/war3skins.txt", 2u * 1024u * 1024u));
    Require(!skins.archiveName.empty(), "start-marker color uses an actual winning game startup profile");
    std::string_view text(reinterpret_cast<const char*>(skins.bytes.data()), skins.bytes.size());
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    std::string prefix; unsigned definitions = 0;
    Text::VisitProfileFields(text, [&](std::string_view section, std::string_view key, std::string_view value) {
        if (section == "default" && key == "teamcolor") { prefix = value; ++definitions; }
    });
    Require(definitions == 1 && !prefix.empty(), "actual startup TeamColor prefix is explicit and unambiguous");
    const auto tga = prefix + "00.tga", blp = prefix + "00.blp";
    Require(!source.HasMember(tga) && !source.HasMember(blp),
        "authored pathing fixture has no hidden map-layer override of the actual startup team-color image");
    const auto path = mounted.HasFile(tga) ? tga : blp;
    const auto colorSource = Take(mounted.ReadFileBoundedWithSource(path, 2u * 1024u * 1024u));
    Require(!colorSource.archiveName.empty(), "start-marker color image has an actual winning game container");
    const auto image = Take(Image::DecodeTexture(path, colorSource.bytes, 1024u * 1024u));
    Require(image.Valid() && !image.rgba.empty() && image.rgba.size() % 4 == 0,
        "actual startup team-color image is complete RGBA data");
    std::uint64_t red = 0, green = 0, blue = 0;
    for (std::size_t pixel = 0; pixel < image.rgba.size(); pixel += 4) {
        red += std::to_integer<unsigned>(image.rgba[pixel]);
        green += std::to_integer<unsigned>(image.rgba[pixel + 1]);
        blue += std::to_integer<unsigned>(image.rgba[pixel + 2]);
    }
    // 独立wire字面值：版本0、1条、type2。已核原图世界可玩区为
    // [-3328,-3584,3328,3072]，起点(0,0)映射x=128、y=256-trunc(3584/6656*256)=119。
    // 不调用正式MMP生成器、frame、坐标投影或颜色平均函数造expected。
    std::vector<std::byte> expected;
    for (const auto value : {0u, 1u, 2u, 128u, 119u})
        for (unsigned shift = 0; shift < 32; shift += 8) expected.push_back(std::byte((value >> shift) & 255));
    const auto count = image.rgba.size() / 4;
    expected.insert(expected.end(), {std::byte(blue / count), std::byte(green / count), std::byte(red / count), std::byte{255}});
    return expected;
}
} // namespace PathingAdaptersRealDetail

// 使用现有真实资源/工具链原创地图。直接调用产品CLI解析入口，不把参数门禁当成存图证明。
// MCP同协议的解析已有独立测试；本用例不声明已完成MCP真实存图或原版/游戏验收。
inline void CheckRealTerrainPathingCliSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    using namespace PathingAdaptersRealDetail;
    constexpr std::size_t kBytes = 64u * 1024u * 1024u;
    constexpr std::size_t kWidth = 256;
    const auto scenario = directory / "pathing-cli-public-save";
    Require(std::filesystem::create_directory(scenario), "exclusive actual CLI pathing-save directory");
    const auto originalSha = Take(Sha256File(original.SourcePath()));
    const auto terrainBytes = Take(original.ReadMember("war3map.w3e"));
    const auto before = *Take(MapData::ParseMapTerrain(terrainBytes)).Snapshot();
    Require(before.header.width == 65 && before.header.height == 65,
        "actual CLI pathing save uses the original classic64-cell map");
    const auto actualRules = Take(Services::TerrainPathingSave::CaptureNative(original, before, before, game, kBytes));
    const auto native = Take(actualRules.GenerateNative(before, kBytes));
    auto sourceWpm = Take(original.ReadMember("war3map.wpm"));
    Require(sourceWpm.size() == 16 + kWidth * kWidth && sourceWpm.size() == native.size() + 16,
        "actual CLI pathing save captures complete original/native WPM dimensions");
    for (std::size_t i = 0; i < native.size(); ++i)
        sourceWpm[16 + i] = (sourceWpm[16 + i] & std::byte{0x11}) | native[i];
    const auto pixel = [](std::size_t x, std::size_t y) { return 16 + y * kWidth + x; };
    constexpr std::array<std::size_t, 3> targetX{96, 112, 128};
    constexpr std::array<std::byte, 3> bits{std::byte{0x02}, std::byte{0x04}, std::byte{0x08}};
    const std::array<std::wstring, 3> names{L"walk", L"fly", L"build"};
    for (const auto x : {80u, 96u, 112u, 128u, 160u})
        Require((sourceWpm[pixel(x, 128)] & std::byte{0x0e}) == std::byte{0},
            "named-bit literal points start on genuinely unrestricted actual native ground");
    // 来源限制与未知保留位属于作者输入。擦除命令不得清掉这些位。
    sourceWpm[pixel(80, 128)] |= std::byte{0x1f};
    sourceWpm[pixel(96, 128)] |= std::byte{0x15};
    sourceWpm[pixel(112, 128)] |= std::byte{0x19};
    sourceWpm[pixel(128, 128)] |= std::byte{0x13};
    const std::array replacements{MemberReplacement{"war3map.wpm", sourceWpm}};
    const auto source = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "source.w3x", replacements)).path));
    const auto sourceSha = source.SourceSha256(), terrainSha = Take(Sha256(terrainBytes));
    const auto expectedMarkers = ExpectedStartMarkers(source, before, game);
    const auto arguments = [&](const std::filesystem::path& target) {
        return std::vector<std::wstring>{L"war3-map-cli", L"map-edit-save", source.SourcePath().wstring(),
            Ascii(sourceSha), Ascii(terrainSha), tool.wstring(), game.wstring(), target.wstring()};
    };
    const auto append = [&](std::vector<std::wstring>& args, const std::wstring& kind,
        std::size_t x, std::size_t y, bool blocked, bool square = false) {
        args.insert(args.end(), {L"--pathing-stroke", kind, square ? L"square" : L"circle",
            std::to_wstring(double(before.header.offset[0]) + double(x) * 32.0 + 16.0),
            std::to_wstring(double(before.header.offset[1]) + double(y) * 32.0 + 16.0),
            L"1", blocked ? L"true" : L"false"});
    };
    for (std::size_t type = 0; type < names.size(); ++type) {
        const auto target = scenario / ("saved-" + std::to_string(type) + ".w3x");
        auto args = arguments(target);
        append(args, names[type], 80, 128, false); // 不能擦掉源图限制。
        append(args, names[type], 160, 128, true);
        append(args, names[type], 160, 128, false); // 可撤回本请求新增限制。
        append(args, names[type], targetX[type], 128, true, type == 1);
        append(args, names[type], targetX[type], 128, true, type == 1); // 重复设置不扩足迹。
        const auto result = Invoke(std::move(args));
        if (result.exitCode != 0)
            throw std::runtime_error("actual CLI pathing save failed: " + result.diagnostics);
        const auto output = Take(MapArchiveSession::Open(target));
        auto expected = sourceWpm;
        // 期望只按CLI公开的名称->位映射和size1单像素构造，不调用产品笔刷/footprint。
        expected[pixel(targetX[type], 128)] |= bits[type];
        Require(Take(output.ReadMember("war3map.wpm")) == expected,
            "every actual saved WPM byte matches independent named-bit/single-pixel expectation");
        const bool addsMarkers = !source.HasMember("war3map.mmp");
        Require(output.Members().size() == source.Members().size() + (addsMarkers ? 1 : 0),
            "actual pathing-only CLI save preserves original named-member coverage and only adds the generated MMP when absent");
        Require(Take(output.ReadMember("war3map.mmp")) == expectedMarkers,
            "actual default sloc produces the complete independent MMP type/position/startup-color expectation");
        for (const auto& member : source.Members()) {
            Require(output.HasMember(member.name), "actual pathing-only CLI save retains every source member");
            if (member.name != "war3map.wpm" && member.name != "war3map.mmp")
                Require(Take(output.ReadMember(member.name)) == Take(source.ReadMember(member.name)),
                    "actual pathing-only CLI save preserves complete terrain/script/placement/shadow/minimap bytes");
        }
        Require(result.output.find("candidate_sha256=" + output.SourceSha256()) != std::string::npos
            && result.output.find("source_sha256=" + sourceSha) != std::string::npos
            && result.output.find("applied_strokes=5") != std::string::npos,
            "actual CLI receipt binds fully reopened output and all five ordered requests");
        Require(Take(Sha256File(source.SourcePath())) == sourceSha,
            "actual pathing CLI SaveAs leaves its original source archive unchanged");
        std::cout << "pathing-cli-real type=" << type << " source_sha=" << sourceSha
            << " output_sha=" << output.SourceSha256() << '\n';
    }
    const auto reject = [&](std::vector<std::wstring> args, const std::filesystem::path& target,
        std::string_view diagnostic, int expectedExit = 1) {
        const auto result = Invoke(std::move(args));
        Require(result.exitCode == expectedExit && result.output.empty()
            && result.diagnostics.find(diagnostic) != std::string::npos && !std::filesystem::exists(target),
            "failed actual CLI pathing request reports its gate and never publishes a target or success receipt");
        Require(Take(Sha256File(source.SourcePath())) == sourceSha,
            "failed actual pathing CLI save never changes its source");
    };
    const auto staleSource = scenario / "stale-source-rejected.w3x";
    auto args = arguments(staleSource); args[3] = std::wstring(64, L'0'); append(args, L"walk", 96, 128, true);
    reject(std::move(args), staleSource, "SAVE_SOURCE_STALE");
    const auto staleTerrain = scenario / "stale-terrain-rejected.w3x";
    args = arguments(staleTerrain); args[4] = std::wstring(64, L'0'); append(args, L"fly", 112, 128, true);
    reject(std::move(args), staleTerrain, "SAVE_TERRAIN_STALE");
    const auto outside = scenario / "outside-rejected.w3x";
    args = arguments(outside); append(args, L"build", 256, 128, true);
    reject(std::move(args), outside, "SAVE_STROKE_POINT_BOUNDS");
    const auto invalid = scenario / "invalid-size-rejected.w3x";
    args = arguments(invalid); append(args, L"walk", 96, 128, true); args[13] = L"9";
    reject(std::move(args), invalid, "SAVE_ARGUMENT_INVALID", 2);
    Require(Take(Sha256File(original.SourcePath())) == originalSha,
        "all actual CLI pathing scenarios keep their original real-toolchain fixture read-only");
}
} // namespace War3Map::TerrainSaveTests
