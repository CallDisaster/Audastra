#include "War3MapCore/Resources/ResourceResolver.h"

#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using War3Map::Result;
using War3Map::Resources::ResourceLayer;
using War3Map::Resources::ResourceProbe;
using War3Map::Resources::ResourceProbeState;
namespace Priority = War3Map::Resources::ResourcePriority;
using War3Map::Resources::ResourceResolver;
using War3Map::Resources::ResourceSource;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
bool HasCode(const Result<T>& result, std::string_view code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const unsigned char character : text) bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}

struct FakeBacking {
    std::map<std::string, std::vector<std::byte>> files;
    bool failProbe = false;
    bool failRead = false;
    bool conflict = false;
    int probes = 0;
    int reads = 0;
};

ResourceSource MakeSource(std::string id, ResourceLayer layer, int priority,
    const std::shared_ptr<FakeBacking>& backing) {
    ResourceSource source;
    source.id = std::move(id);
    source.layer = layer;
    source.priority = priority;
    source.probe = [backing](std::string_view canonicalPath) {
        ++backing->probes;
        if (backing->failProbe) {
            return Result<ResourceProbe>::Failure("Fake", "PROBE_FAILURE", "合成查询失败");
        }
        if (backing->conflict) {
            return Result<ResourceProbe>::Success({ResourceProbeState::Conflict, "同名资源无法消歧"});
        }
        const auto found = backing->files.find(std::string(canonicalPath));
        if (found == backing->files.end()) {
            return Result<ResourceProbe>::Success({ResourceProbeState::Missing, {}});
        }
        return Result<ResourceProbe>::Success({ResourceProbeState::Found, found->first});
    };
    source.read = [backing](std::string_view sourcePath) {
        ++backing->reads;
        if (backing->failRead) {
            return Result<std::vector<std::byte>>::Failure("Fake", "READ_FAILURE", "合成读取失败");
        }
        const auto found = backing->files.find(std::string(sourcePath));
        if (found == backing->files.end()) {
            return Result<std::vector<std::byte>>::Failure("Fake", "RACE", "资源在查询后消失");
        }
        return Result<std::vector<std::byte>>::Success(found->second);
    };
    return source;
}

void TestLayerAndArchivePriority() {
    ResourceResolver resolver;
    auto gameBase = std::make_shared<FakeBacking>();
    auto gamePatch = std::make_shared<FakeBacking>();
    auto map = std::make_shared<FakeBacking>();
    auto mapDirectory = std::make_shared<FakeBacking>();
    auto session = std::make_shared<FakeBacking>();
    auto tool = std::make_shared<FakeBacking>();
    for (const auto& backing : {gameBase, gamePatch, map, mapDirectory, session, tool}) {
        backing->files["REPLACEABLETEXTURES\\COMMANDBUTTONS\\BTNTEST.BLP"] = Bytes("present");
    }
    gameBase->files.begin()->second = Bytes("base");
    gamePatch->files.begin()->second = Bytes("patch");
    map->files.begin()->second = Bytes("map");
    mapDirectory->files.begin()->second = Bytes("directory");
    session->files.begin()->second = Bytes("session");
    tool->files.begin()->second = Bytes("tool");

    Require(resolver.AddSource(MakeSource("game-base", ResourceLayer::Game, Priority::GameBase, gameBase)).Succeeded(),
        "game base registration failed");
    Require(resolver.AddSource(MakeSource("map", ResourceLayer::MapImport, Priority::MapArchive, map)).Succeeded(),
        "map registration failed");
    Require(resolver.AddSource(MakeSource("map-dir", ResourceLayer::MapImport,
        Priority::MapDirectory, mapDirectory)).Succeeded(), "map directory registration failed");
    Require(resolver.AddSource(MakeSource("game-patch", ResourceLayer::Game, Priority::GamePatch, gamePatch)).Succeeded(),
        "game patch registration failed");
    Require(resolver.AddSource(MakeSource("session", ResourceLayer::SessionOverride, Priority::SessionOverride, session)).Succeeded(),
        "session registration failed");
    Require(resolver.AddSource(MakeSource("ydwe-extra", ResourceLayer::ToolExtension,
        Priority::ToolExtension, tool)).Succeeded(), "tool extension registration failed");

    constexpr std::string_view path = "ReplaceableTextures/CommandButtons/BTNTest.blp";
    auto read = resolver.Read(path);
    Require(read.Succeeded() && read.value->bytes == Bytes("session"), "session must win");
    Require(read.value->location.sourceId == "session" &&
        read.value->location.layer == ResourceLayer::SessionOverride &&
        read.value->location.sourcePath == "REPLACEABLETEXTURES\\COMMANDBUTTONS\\BTNTEST.BLP",
        "winner provenance must be exact");
    Require(map->probes == 0 && gamePatch->probes == 0, "lower layers must not be consulted");

    Require(resolver.RemoveSource("session"), "session removal failed");
    read = resolver.Read(path);
    Require(read.Succeeded() && read.value->bytes == Bytes("map") &&
        read.value->location.sourceId == "map", "map import must override game");
    Require(resolver.RemoveSource("map"), "map removal failed");
    read = resolver.Read(path);
    Require(read.Succeeded() && read.value->bytes == Bytes("directory") &&
        read.value->location.sourceId == "map-dir", "map archive must override expanded directory");
    Require(resolver.RemoveSource("map-dir"), "map directory removal failed");
    read = resolver.Read(path);
    Require(read.Succeeded() && read.value->bytes == Bytes("patch") &&
        read.value->location.sourceId == "game-patch", "game patch must override game base");
    Require(gameBase->probes == 0, "lower-priority archive must not be consulted after hit");
    Require(resolver.RemoveSource("game-patch"), "patch removal failed");
    read = resolver.Read(path);
    Require(read.Succeeded() && read.value->bytes == Bytes("base"), "game base fallback failed");
    Require(resolver.RemoveSource("game-base"), "base removal failed");
    read = resolver.Read(path);
    Require(read.Succeeded() && read.value->bytes == Bytes("tool") &&
        read.value->location.layer == ResourceLayer::ToolExtension,
        "tool extension must be a distinct lower-priority source");
    resolver.ClearSources();
    Require(HasCode(resolver.Read(path), "RESOURCE_NOT_FOUND"), "empty resolver must report missing");
}

void TestCanonicalPathAndNoCache() {
    ResourceResolver resolver;
    auto backing = std::make_shared<FakeBacking>();
    backing->files["UI\\ICON.BLP"] = Bytes("one");
    Require(resolver.AddSource(MakeSource("game", ResourceLayer::Game, Priority::GameBase, backing)).Succeeded(),
        "source registration failed");
    auto read = resolver.Read("ui/icon.blp");
    Require(read.Succeeded() && read.value->location.canonicalPath == "UI\\ICON.BLP" &&
        read.value->bytes == Bytes("one"), "case and separator normalization failed");
    backing->files["UI\\ICON.BLP"] = Bytes("two");
    read = resolver.Read("UI\\ICON.BLP");
    Require(read.Succeeded() && read.value->bytes == Bytes("two") && backing->reads == 2,
        "resource mutation must be visible without cache invalidation");
    backing->files.erase("UI\\ICON.BLP");
    Require(HasCode(resolver.Read("ui/icon.blp"), "RESOURCE_NOT_FOUND"),
        "deleted resource must not be served from stale cache");

    for (const auto& invalid : {"", "../x", "a/../b", "a/./b", "C:\\x", "/x",
            "a//b", "a\\", "CON.blp", "a/NUL.txt", "x?y"}) {
        Require(HasCode(resolver.Read(invalid), "RESOURCE_PATH_UNSAFE"),
            "unsafe path must fail before querying providers");
    }
}

void TestConflictAndFailuresBlockFallback() {
    ResourceResolver resolver;
    auto first = std::make_shared<FakeBacking>();
    auto second = std::make_shared<FakeBacking>();
    auto game = std::make_shared<FakeBacking>();
    first->files["SOUNDS\\TEST.WAV"] = Bytes("first");
    second->files["SOUNDS\\TEST.WAV"] = Bytes("second");
    game->files["SOUNDS\\TEST.WAV"] = Bytes("game");
    Require(resolver.AddSource(MakeSource("first", ResourceLayer::MapImport, Priority::MapArchive, first)).Succeeded(),
        "first source registration failed");
    Require(resolver.AddSource(MakeSource("second", ResourceLayer::MapImport, Priority::MapArchive, second)).Succeeded(),
        "second source registration failed");
    Require(resolver.AddSource(MakeSource("game", ResourceLayer::Game, Priority::GameBase, game)).Succeeded(),
        "game source registration failed");
    Require(HasCode(resolver.Read("Sounds/Test.wav"), "RESOURCE_CONFLICT"),
        "same-rank collision must report conflict");
    Require(game->probes == 0, "collision must not fall back to game data");

    Require(resolver.RemoveSource("second"), "second source removal failed");
    first->conflict = true;
    Require(HasCode(resolver.Read("Sounds/Test.wav"), "RESOURCE_CONFLICT"),
        "intra-source ambiguity must report conflict");
    first->conflict = false;
    first->failProbe = true;
    Require(HasCode(resolver.Read("Sounds/Test.wav"), "RESOURCE_PROBE_FAILED"),
        "probe failure must be explicit");
    first->failProbe = false;
    first->failRead = true;
    Require(HasCode(resolver.Read("Sounds/Test.wav"), "RESOURCE_READ_FAILED"),
        "selected source read failure must be explicit");
    Require(game->probes == 0 && game->reads == 0,
        "provider failures must not silently use lower-priority resource");
}

void TestSourceRegistration() {
    ResourceResolver resolver;
    auto backing = std::make_shared<FakeBacking>();
    auto other = std::make_shared<FakeBacking>();
    backing->files["TEST.BLP"] = Bytes("game");
    other->files["TEST.BLP"] = Bytes("custom");
    Require(!resolver.AddSource({}).Succeeded(), "empty source must be rejected");
    Require(resolver.AddSource(MakeSource("game", ResourceLayer::Game, Priority::GameBase, backing)).Succeeded(),
        "first registration failed");
    Require(!resolver.AddSource(MakeSource("game", ResourceLayer::MapImport, Priority::MapArchive, backing)).Succeeded(),
        "duplicate source identity must be rejected");
    Require(!resolver.RemoveSource("missing"), "removing unknown source must be a no-op");
    Require(resolver.AddSource(MakeSource("custom", ResourceLayer::Other, 700, other)).Succeeded(),
        "custom source registration failed");
    const auto read = resolver.Read("test.blp");
    Require(read.Succeeded() && read.value->bytes == Bytes("custom") &&
        read.value->location.layer == ResourceLayer::Other,
        "custom layer priority must be injectable rather than enum-bound");
}

} // namespace

int main() {
    try {
        TestLayerAndArchivePriority();
        TestCanonicalPathAndNoCache();
        TestConflictAndFailuresBlockFallback();
        TestSourceRegistration();
        std::cout << "ResourceResolver: 4 scenario groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ResourceResolver: " << exception.what() << '\n';
        return 1;
    }
}
