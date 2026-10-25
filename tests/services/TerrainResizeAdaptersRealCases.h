#pragma once

#include "../../src/Cli/MapEditSaveCommand.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <bit>
#include <array>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace ResizeAdaptersReal {
using Bytes = std::vector<std::byte>;
inline void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (result.Succeeded() && result.value) return std::move(*result.value);
    std::string message;
    for (const auto& diagnostic : result.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message.empty() ? "missing real canvas resize result" : message);
}
inline void Put(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = std::byte((value >> (i * 8)) & 255);
}
inline Bytes Terrain(const Bytes& original, bool expanded) {
    Require(original.size() == 69 + 65 * 65 * 7, "resize real fixture has independent classic header and vertex layout");
    if (!expanded) return original;
    Bytes output(original.begin(), original.begin() + 69);
    Put(output, 53, 97); Put(output, 57, 97);
    Put(output, 61, std::bit_cast<std::uint32_t>(-8192.0f)); Put(output, 65, std::bit_cast<std::uint32_t>(-8192.0f));
    for (int y = 0; y <= 96; ++y) for (int x = 0; x <= 96; ++x) {
        const auto sx = std::clamp(x - 32, 0, 64), sy = std::clamp(y - 32, 0, 64);
        const auto at = 69 + (std::size_t(sy) * 65 + sx) * 7;
        const auto next = output.size(); output.insert(output.end(), original.begin() + at, original.begin() + at + 7);
        if (x < 96 && y < 96) {
            const bool edge = x < 38 || x >= 90 || y < 36 || y >= 88;
            output[next + 3] = (output[next + 3] & std::byte{0xbf}) | (edge ? std::byte{0x40} : std::byte{0});
        }
    }
    return output;
}
inline std::byte GroundMask(const Game::GameFileContents& file, const std::string& rawcode) {
    const auto rows = Take(Text::ParseSlkCells(std::string_view(reinterpret_cast<const char*>(file.bytes.data()), file.bytes.size()),
        Text::SlkCellMode::CatalogUnique));
    Require(!rows.empty(), "actual resize terrain table has header");
    std::array<int, 4> columns{};
    constexpr std::array<std::string_view, 4> fields{"tileid", "walkable", "flyable", "buildable"};
    for (const auto& [column, value] : rows.begin()->second)
        for (std::size_t i = 0; i < fields.size(); ++i) if (Text::LowerAscii(value) == fields[i]) {
            Require(!columns[i], "resize actual permissions have unique columns"); columns[i] = column;
        }
    for (const auto column : columns) Require(column, "resize actual permissions have complete columns");
    std::byte mask{0x40}; unsigned matches = 0;
    for (auto row = std::next(rows.begin()); row != rows.end(); ++row) {
        const auto id = row->second.find(columns[0]); if (id == row->second.end() || id->second != rawcode) continue;
        ++matches; constexpr std::array<unsigned, 3> bits{2, 4, 8};
        for (std::size_t i = 0; i < bits.size(); ++i) {
            const auto value = row->second.find(columns[i + 1]);
            Require(value != row->second.end() && (value->second == "0" || value->second == "1"), "actual permissions are booleans");
            if (value->second == "0") mask |= std::byte(bits[i]);
        }
    }
    Require(matches == 1, "resize actual ground rawcode is unique"); return mask;
}
inline Bytes Pathing(const Bytes& source, std::byte ground, bool expanded) {
    Require(source.size() == 16 + 256 * 256, "resize initial WPM is complete");
    const unsigned side = expanded ? 384 : 256, delta = expanded ? 128 : 0;
    Bytes output(source.begin(), source.begin() + 16); Put(output, 8, side); Put(output, 12, side);
    for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
        const int sx = int(x) - int(delta), sy = int(y) - int(delta);
        const bool old = sx >= 0 && sy >= 0 && sx < 256 && sy < 256;
        const bool playable = sx >= 24 && sx < 232 && sy >= 16 && sy < 224;
        const auto authored = old ? source[16 + std::size_t(sy) * 256 + sx] & std::byte{0x11} : std::byte{0};
        output.push_back(authored | (playable ? ground : std::byte{0xce}));
    }
    return output;
}
inline Bytes Shadow(const Bytes& source, bool expanded) {
    Require(source.size() == 256 * 256, "resize initial SHD is complete");
    const unsigned side = expanded ? 384 : 256, delta = expanded ? 128 : 0;
    Bytes output(std::size_t(side) * side);
    for (unsigned y = delta; y < side; ++y) for (unsigned x = delta; x < side; ++x) {
        const auto value = source[std::size_t(y - delta) * 256 + x - delta];
        output[std::size_t(y) * side + x] = value == std::byte{0} ? std::byte{0} : std::byte{255};
    }
    return output;
}
struct Invocation { int exit; std::string output, diagnostics; };
inline Invocation Invoke(std::vector<std::wstring> arguments) {
    std::vector<wchar_t*> argv; for (auto& argument : arguments) argv.push_back(argument.data());
    std::ostringstream output, diagnostics;
    auto* oldOutput = std::cout.rdbuf(output.rdbuf()); auto* oldError = std::cerr.rdbuf(diagnostics.rdbuf());
    try {
        const auto exit = RunMapEditSave(static_cast<int>(argv.size()), argv.data());
        std::cout.rdbuf(oldOutput); std::cerr.rdbuf(oldError);
        return {exit, output.str(), diagnostics.str()};
    } catch (...) { std::cout.rdbuf(oldOutput); std::cerr.rdbuf(oldError); throw; }
}
} // namespace ResizeAdaptersReal

inline void CheckRealCanvasResizeSave(const MapArchiveSession& original, const std::filesystem::path& directory,
    const std::filesystem::path& tool, const std::filesystem::path& game) {
    namespace R = ResizeAdaptersReal;
    const auto scenario = directory / "canvas-resize-real-cli";
    R::Require(std::filesystem::create_directory(scenario), "exclusive real canvas resize scenario");
    const auto originalTerrain = R::Take(original.ReadMember("war3map.w3e"));
    const auto grid = *R::Take(MapData::ParseMapTerrain(originalTerrain)).Snapshot();
    R::Require(grid.header.width == 65 && grid.header.height == 65
        && grid.header.offset == std::array<float, 2>{-4096, -4096}, "real canvas source retains original 64-cell geometry");
    for (const auto& vertex : grid.vertices) R::Require(vertex.groundHeightRaw == 8192
        && vertex.CliffLevel() == 2 && vertex.GroundTextureIndex() == 0 && !vertex.HasWater()
        && !vertex.IsRamp() && !vertex.IsBoundary() && !vertex.IsBlight(), "real resize literal is flat dry ground");
    const auto settings = R::Take(MapData::MapSettings::Parse(R::Take(original.ReadMember("war3map.w3i"))));
    R::Require(settings.cameraMargins == std::array<std::int32_t, 4>{6, 6, 4, 8}, "real resize retains authored playable rectangle");
    auto gameData = R::Take(Game::GameDataSource::MountStrict(game, grid.header.mainTileset));
    const auto actualTable = R::Take(gameData.ReadFileBoundedWithSource("TerrainArt/Terrain.slk", 2u * 1024u * 1024u));
    R::Require(!actualTable.archiveName.empty() && !original.HasMember("TerrainArt/Terrain.slk"), "resize uses actual game winning ground table");
    const auto ground = R::GroundMask(actualTable, grid.header.groundTilesets[0]);
    auto seedWpm = R::Take(original.ReadMember("war3map.wpm"));
    R::Require(seedWpm.size() == 16 + 256 * 256, "resize source has actual full WPM");
    for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x)
        seedWpm[16 + std::size_t(y) * 256 + x] = std::byte((x % 7 == 1 ? 1 : 0) | (y % 11 == 2 ? 0x10 : 0));
    seedWpm = R::Pathing(seedWpm, ground, false);
    R::Bytes seedShadow(256 * 256); seedShadow[73 * 256 + 91] = std::byte{2}; seedShadow[38 * 256 + 157] = std::byte{0x80};
    const std::array<MemberReplacement, 2> seed{{{"war3map.wpm", seedWpm}, {"war3map.shd", seedShadow}}};
    auto source = R::Take(MapArchiveSession::Open(R::Take(original.SaveCandidate(scenario / "source.w3x", seed)).path));
    const auto initialSource = source.SourcePath(); const auto initialSha = source.SourceSha256();
    const auto originalScript = R::Take(original.ReadMember("war3map.j"));
    for (const bool expanded : {true, false}) {
        const auto target = scenario / (expanded ? "expanded.w3x" : "cropped-back.w3x");
        const auto terrainSha = R::Take(Sha256(R::Take(source.ReadMember("war3map.w3e"))));
        std::vector<std::wstring> args{L"AudastraCli", L"map-edit-save", source.SourcePath().wstring(),
            std::wstring(source.SourceSha256().begin(), source.SourceSha256().end()), std::wstring(terrainSha.begin(), terrainSha.end()),
            tool.wstring(), game.wstring(), target.wstring(), L"--terrain-resize",
            // 第二笔相对扩展后的西南角；[32,32,96,96]才恢复原64×64地格。
            expanded ? L"-32" : L"32", expanded ? L"-32" : L"32", expanded ? L"64" : L"96", expanded ? L"64" : L"96"};
        const auto invoked = R::Invoke(std::move(args));
        if (invoked.exit) throw std::runtime_error("real canvas CLI save failed: " + invoked.diagnostics);
        auto saved = R::Take(MapArchiveSession::Open(target));
        R::Require(R::Take(saved.ReadMember("war3map.w3e")) == R::Terrain(originalTerrain, expanded), "real resize entire W3E matches independent literal");
        R::Require(R::Take(saved.ReadMember("war3map.wpm")) == R::Pathing(seedWpm, ground, expanded), "real resize entire WPM preserves original P0 at world positions");
        R::Require(R::Take(saved.ReadMember("war3map.shd")) == R::Shadow(seedShadow, expanded), "real resize SHD rebases world pixels and writes native 00/FF");
        auto expectedSettings = settings;
        if (expanded) expectedSettings.cameraMargins = {38, 6, 36, 8};
        R::Require(R::Take(saved.ReadMember("war3map.w3i")) == R::Take(expectedSettings.Serialize()), "real resize preserves playable world coordinates in full W3I");
        for (const auto* name : {"war3mapUnits.doo", "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.wtg", "war3map.wct"})
            if (source.HasMember(name)) R::Require(R::Take(saved.ReadMember(name)) == R::Take(source.ReadMember(name)), "real resize preserves ordinary spatial and trigger members");
        R::Require(R::Take(saved.ReadMember("war3map.j")) == originalScript, "compiled initializer stays equivalent at identical playable world coordinates");
        R::Require(invoked.output.find("candidate_sha256=" + saved.SourceSha256()) != std::string::npos
            && invoked.output.find("applied_strokes=1") != std::string::npos, "real CLI receipt binds reopened output and exact resize count");
        R::Require(R::Take(Sha256File(initialSource)) == initialSha && R::Take(Sha256File(original.SourcePath())) == original.SourceSha256(),
            "actual toolchain resize never changes its source archives");
        std::cout << "canvas-resize-cli-real expanded=" << expanded << " source_sha=" << source.SourceSha256()
            << " output_sha=" << saved.SourceSha256() << '\n';
        source = std::move(saved);
    }

    // 组合场景复用原始来源与上面的独立literal，另写产物，不改变扩展/裁回验收叶。
    const auto base = R::Take(MapArchiveSession::Open(initialSource));
    const auto arguments = [&](const std::filesystem::path& target) {
        const auto terrainSha = R::Take(Sha256(R::Take(base.ReadMember("war3map.w3e"))));
        return std::vector<std::wstring>{L"AudastraCli", L"map-edit-save", base.SourcePath().wstring(),
            std::wstring(initialSha.begin(), initialSha.end()), std::wstring(terrainSha.begin(), terrainSha.end()),
            tool.wstring(), game.wstring(), target.wstring(), L"--terrain-resize", L"-32", L"-32", L"64", L"64"};
    };
    const auto checkSharedOutputs = [&](const MapArchiveSession& saved) {
        R::Require(R::Take(saved.ReadMember("war3map.wpm")) == R::Pathing(seedWpm, ground, true),
            "composed resize independently preserves full world-positioned WPM including authored bits");
        R::Require(R::Take(saved.ReadMember("war3map.shd")) == R::Shadow(seedShadow, true),
            "composed resize independently rebases the complete SHD and writes native 00/FF");
        for (const auto* name : {"war3mapUnits.doo", "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.wtg", "war3map.wct"})
            if (base.HasMember(name)) R::Require(R::Take(saved.ReadMember(name)) == R::Take(base.ReadMember(name)),
                "composed resize preserves complete ordinary spatial and trigger members");
        R::Require(R::Take(Sha256File(initialSource)) == initialSha
            && R::Take(Sha256File(original.SourcePath())) == original.SourceSha256(),
            "composed real CLI saves preserve both original source archives");
    };
    auto expandedSettings = settings;
    expandedSettings.cameraMargins = {38, 6, 36, 8};
    {
        const auto target = scenario / "expanded-with-name.w3x";
        auto args = arguments(target);
        const auto document = MapDocument::FromArchive(base);
        const auto* field = MapData::FindMapField(MapDataKind::Settings, "map_name");
        R::Require(field != nullptr, "real resize composition uses the published map-name field");
        const auto row = R::Take(MapData::MapFieldRowKey(document, MapDataKind::Settings, 0));
        const auto memberSha = R::Take(Sha256(R::Take(base.ReadMember("war3map.w3i"))));
        const auto before = MapData::FormatMapFieldRaw(R::Take(MapData::ReadMapField(document, MapDataKind::Settings, 0, *field)));
        args.insert(args.end(), {L"--field", L"settings", L"0", std::wstring(row.begin(), row.end()),
            std::wstring(memberSha.begin(), memberSha.end()), L"map_name", std::wstring(before.begin(), before.end()),
            L"Resize with settings"});
        const auto invoked = R::Invoke(std::move(args));
        if (invoked.exit) throw std::runtime_error("real resize plus settings CLI save failed: " + invoked.diagnostics);
        const auto saved = R::Take(MapArchiveSession::Open(target));
        const auto terrainBytes = R::Take(saved.ReadMember("war3map.w3e"));
        R::Require(terrainBytes == R::Terrain(originalTerrain, true),
            "map-name composition cannot replace resized geometry with the original source W3E");
        const auto geometry = R::Take(MapData::ParseMapTerrain(terrainBytes));
        const auto finalSettingsBytes = R::Take(saved.ReadMember("war3map.w3i"));
        const auto finalSettings = R::Take(MapData::MapSettings::Parse(finalSettingsBytes));
        auto expectedSettings = expandedSettings; expectedSettings.mapName = "Resize with settings";
        // W3E/WPM/SHD预期独立；完整W3I比较复用Settings writer，以下再直接核typed字段。
        R::Require(finalSettingsBytes == R::Take(expectedSettings.Serialize())
            && geometry.Snapshot()->header.width == 97 && geometry.Snapshot()->header.height == 97
            && geometry.Snapshot()->header.offset == std::array<float, 2>{-8192, -8192}
            && finalSettings.FullyParsed() && finalSettings.formatVersion == 25
            && finalSettings.mapName == "Resize with settings" && (finalSettings.flags & 0x400u)
            && finalSettings.cameraMargins == std::array<std::int32_t, 4>{38, 6, 36, 8}
            && finalSettings.playableWidth == 52 && finalSettings.playableHeight == 52
            && finalSettings.cameraBounds == settings.cameraBounds,
            "actual combined W3I retains complete expanded margins, playable area and the edited name");
        const auto script = R::Take(saved.ReadMember("war3map.j"));
        R::Require(std::string_view(reinterpret_cast<const char*>(script.data()), script.size())
                .find("call SetMapName( \"Resize with settings\" )") != std::string_view::npos,
            "combined resize rebuilds the checked initializer using the final map name");
        checkSharedOutputs(saved);
        R::Require(invoked.output.find("candidate_sha256=" + saved.SourceSha256()) != std::string::npos
            && invoked.output.find("applied_strokes=1") != std::string::npos
            && invoked.output.find("applied_field_edits=1") != std::string::npos,
            "combined CLI receipt binds the reopened resize and settings output");
    }
    {
        const auto target = scenario / "expanded-then-current-frame-raise.w3x";
        auto args = arguments(target);
        args.insert(args.end(), {L"--stroke", L"raise", L"circle", L"48", L"48", L"0", L"1",
            L"--stroke", L"raise", L"circle", L"80", L"80", L"0", L"2"});
        const auto invoked = R::Invoke(std::move(args));
        if (invoked.exit) throw std::runtime_error("real resize then current-frame raise CLI save failed: " + invoked.diagnostics);
        const auto saved = R::Take(MapArchiveSession::Open(target));
        auto expectedTerrain = R::Terrain(originalTerrain, true);
        // 扩展后原点为(-8192,-8192)，48对应世界-2048；误用旧原点会落到80。
        // 80又大于旧网格最大索引64，但位于新可玩区内，不能被旧尺寸提前拒绝。
        const auto first = 69 + (48u * 97u + 48u) * 7u, second = 69 + (80u * 97u + 80u) * 7u;
        R::Require(expectedTerrain.at(first) == std::byte{0} && expectedTerrain.at(first + 1) == std::byte{0x20}
            && expectedTerrain.at(second) == std::byte{0} && expectedTerrain.at(second + 1) == std::byte{0x20},
            "independent current-frame height literals start at the actual raw height 8192");
        expectedTerrain[first] = std::byte{4}; expectedTerrain[second] = std::byte{8};
        R::Require(R::Take(saved.ReadMember("war3map.w3e")) == expectedTerrain,
            "complete real W3E changes only raw heights at the two requested current-frame vertices");
        R::Require(R::Take(saved.ReadMember("war3map.w3i")) == R::Take(expandedSettings.Serialize())
            && R::Take(saved.ReadMember("war3map.j")) == originalScript,
            "current-frame height composition preserves expanded W3I and identical playable-world initializer");
        checkSharedOutputs(saved);
        R::Require(invoked.output.find("candidate_sha256=" + saved.SourceSha256()) != std::string::npos
            && invoked.output.find("applied_strokes=3") != std::string::npos
            && invoked.output.find("applied_field_edits=0") != std::string::npos,
            "current-frame CLI receipt binds one resize and both ordered vertex raises");
    }
}
} // namespace War3Map::TerrainSaveTests
