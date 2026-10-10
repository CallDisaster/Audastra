#include "TerrainBlightSaveCommand.h"

#include "War3MapCore/Services/TerrainBlightSaveService.h"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

std::optional<std::uint32_t> ParseCoordinate(std::string_view text) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::string PathUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

void PrintDiagnostics(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    }
}

} // namespace

int RunTerrainBlightSave(const std::filesystem::path& sourceMap,
    std::string_view sourceSha256, std::string_view terrainSha256,
    const std::filesystem::path& toolRoot, const std::filesystem::path& gameRoot,
    std::string_view vertexX, std::string_view vertexY, std::string_view radius,
    std::string_view operation, const std::filesystem::path& candidate,
    std::optional<std::vector<std::string>> effectiveLayers) {
    const auto x = ParseCoordinate(vertexX), y = ParseCoordinate(vertexY), r = ParseCoordinate(radius);
    if (!x || !y || !r || (operation != "set" && operation != "clear")) {
        std::cerr << "TerrainBlight.Save: SAVE_ARGUMENT_INVALID: expected unsigned x/y/radius and set|clear\n";
        return 2;
    }
    War3Map::Services::TerrainBlightSaveRequest request;
    request.sourceMap = sourceMap;
    request.targetCandidate = candidate;
    request.toolRoot = toolRoot;
    request.gameRoot = gameRoot;
    request.expectedSourceSha256 = sourceSha256;
    request.expectedTerrainSha256 = terrainSha256;
    request.effectiveLayers = std::move(effectiveLayers);
    request.vertexX = *x;
    request.vertexY = *y;
    request.radius = *r;
    request.setBlight = operation == "set";
    auto saved = War3Map::Services::SaveTerrainBlightCandidate(request);
    PrintDiagnostics(saved.diagnostics);
    if (!saved.Succeeded() || !saved.value) return 1;
    const auto& receipt = *saved.value;
    std::cout << "candidate=" << PathUtf8(receipt.candidate)
              << "\nsource_sha256=" << receipt.sourceSha256
              << "\ncandidate_sha256=" << receipt.candidateSha256
              << "\noriginal_terrain_sha256=" << receipt.originalTerrainSha256
              << "\nsaved_terrain_sha256=" << receipt.savedTerrainSha256
              << "\nsaved_pathing_sha256=" << receipt.savedPathingSha256
              << "\nmember_count=" << receipt.memberCount
              << "\nscript_rebuilt=true"
              << "\nlayer_count=" << receipt.effectiveLayers.size();
    for (std::size_t index = 0; index < receipt.effectiveLayers.size(); ++index) {
        std::cout << "\nlayer_" << index << '=' << receipt.effectiveLayers[index];
    }
    std::cout << "\noriginal_editor_validated=false\nruntime_validated=false\n";
    return 0;
}
