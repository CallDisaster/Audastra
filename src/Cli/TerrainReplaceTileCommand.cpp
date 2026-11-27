#include "TerrainReplaceTileCommand.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Services/MapEditSaveService.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

int Invalid(std::string_view message) {
    std::cerr << "TerrainTile.Replace: SAVE_ARGUMENT_INVALID: " << message << '\n';
    return 2;
}

int Failed(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    return 1;
}

int Failed(const char* code, const std::string& message) {
    std::cerr << "TerrainTile.Replace: " << code << ": " << message << '\n';
    return 1;
}

bool Rawcode(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
} // namespace

int RunTerrainReplaceTile(int argc, wchar_t** argv) {
    if (argc < 5)
        return Invalid("usage: terrain-replace-tile <source-map> <from-rawcode> <to-rawcode> --game-root <war3> --tool-root <ydwe> [--out <new.w3x>]");
    const std::filesystem::path source(argv[2]);
    const auto from = Utf8(argv[3]), to = Utf8(argv[4]);
    if (!Rawcode(from) || !Rawcode(to)) return Invalid("rawcode must be four printable ASCII characters");
    if (from == to) return Invalid("from and to rawcodes are the same");
    std::optional<std::filesystem::path> game, tool, out;
    for (int index = 5; index < argc; index += 2) {
        const std::wstring_view flag(argv[index]);
        auto* value = flag == L"--game-root" ? &game : flag == L"--tool-root" ? &tool : flag == L"--out" ? &out : nullptr;
        if (!value) return Invalid("unknown terrain-replace-tile option");
        if (value->has_value()) return Invalid("option cannot be repeated");
        if (index + 1 >= argc || argv[index + 1][0] == L'\0') return Invalid("option requires a value");
        *value = std::filesystem::path(argv[index + 1]);
    }
    if (!game || !tool) return Invalid("--game-root and --tool-root are required");
    const auto target = out ? *out
        : source.parent_path() / (source.stem().wstring() + L".tiles" + source.extension().wstring());
    std::error_code error;
    if (std::filesystem::exists(target, error) || error)
        return Failed("TERRAIN_TILE_OUT_EXISTS", "候选文件已存在或无法检查，未覆盖：" + Utf8(target));

    auto sourceSha = War3Map::Sha256File(source);
    if (!sourceSha.Succeeded() || !sourceSha.value) return Failed(sourceSha.diagnostics);
    auto archive = War3Map::MapArchiveSession::Open(source);
    if (!archive.Succeeded() || !archive.value) return Failed(archive.diagnostics);
    auto terrainBytes = archive.value->ReadMember("war3map.w3e");
    if (!terrainBytes.Succeeded() || !terrainBytes.value) return Failed(terrainBytes.diagnostics);
    auto terrainSha = War3Map::Sha256(*terrainBytes.value);
    if (!terrainSha.Succeeded() || !terrainSha.value) return Failed(terrainSha.diagnostics);
    auto terrain = War3Map::MapData::ParseMapTerrain(*terrainBytes.value);
    if (!terrain.Succeeded() || !terrain.value || !terrain.value->Snapshot()) return Failed(terrain.diagnostics);
    const auto& ground = terrain.value->Snapshot()->header.groundTilesets;
    const auto found = std::find(ground.begin(), ground.end(), from);
    if (found == ground.end()) return Failed("TERRAIN_TILE_FROM_MISSING", "地面贴图表里没有“" + from + "”。");
    const auto slot = static_cast<std::uint32_t>(found - ground.begin());
    const bool remap = std::find(ground.begin(), ground.end(), to) != ground.end();

    War3Map::Services::MapEditSaveRequest request;
    request.sourceMap = source; request.targetCandidate = target;
    request.toolRoot = *tool; request.gameRoot = *game;
    request.expectedSourceSha256 = *sourceSha.value; request.expectedTerrainSha256 = *terrainSha.value;
    War3Map::Services::MapEditTerrainStroke stroke;
    stroke.textureSlot = War3Map::Services::TerrainTextureSlotCommand{
        remap ? War3Map::Services::TerrainTextureSlotOperation::Remap : War3Map::Services::TerrainTextureSlotOperation::Replace,
        War3Map::MapData::TerrainTextureTable::Ground, slot, from, to};
    request.terrainStrokes.push_back(std::move(stroke));
    auto saved = War3Map::Services::SaveMapEditCandidate(request);
    if (!saved.Succeeded() || !saved.value) return Failed(saved.diagnostics);
    for (const auto& diagnostic : saved.diagnostics)
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    const auto& receipt = *saved.value;
    std::cout << "candidate=" << Utf8(receipt.candidate)
              << "\noperation=" << (remap ? "remap" : "replace")
              << "\nfrom=" << from << "\nto=" << to << "\nfrom_slot=" << slot
              << "\nsource_sha256=" << receipt.sourceSha256
              << "\ncandidate_sha256=" << receipt.candidateSha256
              << "\noriginal_terrain_sha256=" << receipt.originalTerrainSha256
              << "\nsaved_terrain_sha256=" << receipt.savedTerrainSha256
              << "\nsaved_pathing_sha256=" << receipt.savedPathingSha256
              << "\nmember_count=" << receipt.memberCount
              << "\noriginal_editor_validated=false\nruntime_validated=false\n";
    return 0;
}
