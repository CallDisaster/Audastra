#include "War3MapCore/MapData/TerrainShadowResources.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
constexpr std::size_t kBudget = 8 * 1024 * 1024;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "operation failed";
        for (const auto& d : result.diagnostics) message += " | " + d.code + ": " + d.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
std::vector<std::byte> Bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-shadow-profiles-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(root), "private actual profile source directory");
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Loose(std::string_view path, std::string_view contents) {
        const auto target = root / IO::PathFromUtf8(path);
        std::filesystem::create_directories(target.parent_path());
        Require(IO::WriteFileBytes(target, Bytes(contents)).Succeeded(), "write owned profile fixture");
    }
    TerrainShadowResources Capture(std::size_t budget = kBudget) const {
        return Take(TerrainShadowResources::Capture(root, Game::NativeEditorStartupRules::Classic127aExpansion, budget));
    }
};

void DirectionUsesActualFullAppendChain() {
    Fixture f;
    f.Loose("MiscGame.txt", "[Light]\nDirection=\"+3\"\n"); // 56链index1早于WorldEditData。
    f.Loose("WorldEditData.txt", "\xef\xbb\xbf[LIGHT]\r\nDirection=0,-4\r\nDirection=100,100,100\r\n");
    const auto owner = f.Capture();
    Require(owner.LightDirection() == std::array<float, 3>{-0.6f, -0.0f, 0.8f},
        "first three values append across actual profiles rather than last-wins or only WorldEditData");
    Require(owner.DependencyFingerprint().size() == 64 && owner.SourceBytes() > 0,
        "actual complete source owner has a dependency identity and read accounting");
    Require(owner.VerifySources(kBudget).Succeeded(), "unchanged actual full chain verifies");
    const auto exact = owner.CaptureBudgetBytes();
    Require(f.Capture(exact).DependencyFingerprint() == owner.DependencyFingerprint(), "exact aggregate profile budget succeeds");
    Require(!TerrainShadowResources::Capture(f.root, Game::NativeEditorStartupRules::Classic127aExpansion, exact - 1).Succeeded(),
        "one byte short cannot issue a partial actual owner");
    f.Loose("MiscData.txt", "[Light]\nDirection=0,0,-1\n");
    Require(!owner.VerifySources(kBudget).Succeeded(), "a previously absent earlier actual provider invalidates frozen direction");
    Require(f.Capture().DependencyFingerprint() != owner.DependencyFingerprint(), "changed earlier winner changes full source identity");
}

void NativeRawKeysAndActualWinnerArePreserved() {
    Fixture f;
    f.Loose("UI/WorldEditData.txt", "[Light]\nDirection=0,0,-1\n");
    f.Loose("WorldEditData.txt", "[Light]\n Direction=8,8,8\nDirection =8,8,8\nDirection=\n[unfinished\nDirection=3\nDirection=\"0\",\"-4\"\n");
    auto owner = f.Capture();
    Require(owner.LightDirection() == std::array<float, 3>{-0.6f, -0.0f, 0.8f},
        "basename wins; raw keys are not trimmed, raw empty is skipped, unfinished section retains previous section");
    // 完整链正文也是依赖；不是只冻结最终三个值。
    f.Loose("WorldEditData.txt", "[Light]\nDirection=3,0,-4\n[Other]\nUnrelated=changed\n");
    Require(!owner.VerifySources(kBudget).Succeeded(), "same direction with changed actual profile bytes invalidates old owner");
}

void UnknownDirectionDoesNotInventDefaults() {
    for (const auto text : {"[Light]\nDirection=3,0\n", "[Light]\nDirection=0,0,0\n",
        "[Light]\nDirection=1,,2\n", "[Light]\nDirection=NaN,0,-1\n", "[Light]\nDirection=\"1\"junk,0,-1\n"}) {
        Fixture f; f.Loose("WorldEditData.txt", text);
        const auto rejected = TerrainShadowResources::Capture(f.root, Game::NativeEditorStartupRules::Classic127aExpansion, kBudget);
        Require(!rejected.Succeeded() && !rejected.value, "missing, nonfinite or unsupported actual direction cannot issue defaults");
    }
    Fixture empty;
    Require(!TerrainShadowResources::Capture(empty.root, Game::NativeEditorStartupRules::Classic127aExpansion, kBudget).Succeeded(),
        "complete known absence is not a default light");
}
} // namespace

int main() {
    try {
        DirectionUsesActualFullAppendChain();
        NativeRawKeysAndActualWinnerArePreserved();
        UnknownDirectionDoesNotInventDefaults();
        std::cout << "Terrain shadow actual profile resources tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
