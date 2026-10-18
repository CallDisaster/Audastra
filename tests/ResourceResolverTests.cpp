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
using War3Map::Resources::ResourceReadOrigin;
using War3Map::Resources::ResourceReadProvenance;
using War3Map::Resources::ResourceReadResult;

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
        read.value->bytes == Bytes("one") && !read.value->location.readProvenance,
        "legacy read keeps case normalization and explicitly unrecorded provenance");
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

void TestRichReadsFreezeWinnerAndKeepReadKeys() {
    ResourceResolver resolver;
    auto backing = std::make_shared<FakeBacking>();
    auto source = MakeSource("game", ResourceLayer::Game, Priority::GameBase, backing);
    source.probe = [](std::string_view) {
        return Result<ResourceProbe>::Success({ResourceProbeState::Found, "original-read-key.bin"});
    };
    int richReads = 0;
    source.readWithProvenance = [&richReads](std::string_view key) {
        Require(key == "original-read-key.bin", "actual container must not replace the original read key");
        ++richReads;
        return Result<ResourceReadResult>::Success({Bytes(richReads == 1 ? "patch bytes" : "base bytes"),
            ResourceReadProvenance{ResourceReadOrigin::GameArchive,
                richReads == 1 ? "war3patch.mpq" : "war3.mpq", "Textures/Actual.blp", "fixture-game-root"}});
    };
    Require(resolver.AddSource(std::move(source)).Succeeded(), "register rich and legacy callbacks");
    const auto located = resolver.Locate("Textures/Requested.blp");
    Require(located.Succeeded() && !located.value->readProvenance && richReads == 0 && backing->reads == 0,
        "Locate cannot claim or invoke a completed read winner");
    const auto first = resolver.Read("Textures/Requested.blp");
    const auto second = resolver.Read("Textures/Requested.blp");
    Require(first.Succeeded() && second.Succeeded() && first.value->location.readProvenance
        && second.value->location.readProvenance && richReads == 2 && backing->reads == 0,
        "rich callback takes precedence without invoking legacy read");
    Require(first.value->bytes == Bytes("patch bytes") && second.value->bytes == Bytes("base bytes")
        && first.value->location.readProvenance->containerIdentity == "war3patch.mpq"
        && second.value->location.readProvenance->containerIdentity == "war3.mpq",
        "each byte result retains the winner from that same callback invocation");
    Require(first.value->location.sourcePath == "original-read-key.bin"
        && first.value->location.readProvenance->memberPath == "Textures/Actual.blp"
        && first.value->location.readProvenance->origin == ResourceReadOrigin::GameArchive,
        "logical read key and actual read provenance remain independent");
    const auto locatedAgain = resolver.Locate("Textures/Requested.blp");
    Require(locatedAgain.Succeeded() && !locatedAgain.value->readProvenance && richReads == 2,
        "Locate never reuses a prior successful read winner");
    Require(resolver.RemoveSource("game"), "remove rich fixture");
    Require(first.value->location.readProvenance->containerIdentity == "war3patch.mpq",
        "completed winner is owned independently of the resolver and later reads");

    auto richOnly = MakeSource("rich-only", ResourceLayer::Other, 700, backing);
    richOnly.read = {};
    richOnly.readWithProvenance = [](std::string_view) {
        return Result<ResourceReadResult>::Success({{}, ResourceReadProvenance{
            ResourceReadOrigin::SessionOwned, "owned-version:1", "Empty.bin", "fixture-map-sha"}});
    };
    backing->files["EMPTY.BIN"] = {};
    Require(resolver.AddSource(std::move(richOnly)).Succeeded(), "rich-only callback is a valid source");
    const auto empty = resolver.Read("Empty.bin");
    Require(empty.Succeeded() && empty.value->bytes.empty() && empty.value->location.readProvenance,
        "real empty content retains its actual rich winner");
}

void TestBadRichReadsNeverFallBack() {
    ResourceResolver resolver;
    auto upper = std::make_shared<FakeBacking>(), lower = std::make_shared<FakeBacking>();
    upper->files["TEST.BLP"] = Bytes("legacy upper"); lower->files["TEST.BLP"] = Bytes("lower");
    auto source = MakeSource("upper", ResourceLayer::MapImport, Priority::MapArchive, upper);
    int mode = 0, richReads = 0;
    source.readWithProvenance = [&mode, &richReads](std::string_view) -> Result<ResourceReadResult> {
        ++richReads;
        if (mode == 5) return Result<ResourceReadResult>::Failure("Fixture", "RICH_FAILURE", "真实读取失败");
        if (mode == 6) throw std::runtime_error("rich callback exception");
        ResourceReadResult read{Bytes("must not publish"), ResourceReadProvenance{
            ResourceReadOrigin::MapArchive, "fixture.w3x", "Test.blp", "fixture-map-sha"}};
        if (mode == 0) read.provenance.reset();
        if (mode == 1) read.provenance->origin = static_cast<ResourceReadOrigin>(999);
        if (mode == 2) read.provenance->containerIdentity.clear();
        if (mode == 3) read.provenance->memberPath = "../unsafe.blp";
        if (mode == 4) read.provenance->containerIdentity.reserve(128u * 1024u + 1);
        if (mode == 7) read.provenance->sourceContext = std::string("bad\0context", 11);
        if (mode == 8) {
            for (auto* text : {&read.provenance->containerIdentity, &read.provenance->memberPath, &read.provenance->sourceContext}) {
                text->reserve(96u * 1024u);
                Require(text->capacity() <= 128u * 1024u, "aggregate fixture stays within each individual capacity cap");
            }
            Require(read.provenance->containerIdentity.capacity() + read.provenance->memberPath.capacity()
                + read.provenance->sourceContext.capacity() > 256u * 1024u, "aggregate fixture exceeds only the total capacity cap");
        }
        return Result<ResourceReadResult>::Success(std::move(read));
    };
    Require(resolver.AddSource(std::move(source)).Succeeded()
        && resolver.AddSource(MakeSource("lower", ResourceLayer::Game, Priority::GameBase, lower)).Succeeded(),
        "register fail-closed rich source and lower control");
    for (mode = 0; mode < 9; ++mode) {
        const auto read = resolver.Read("Test.blp");
        Require(!read.Succeeded() && !read.value && HasCode(read, "RESOURCE_READ_FAILED"),
            "invalid rich winner, error or exception cannot publish partial content");
        if (mode == 5) Require(HasCode(read, "RICH_FAILURE"), "rich source diagnostics are retained");
        else if (mode != 6) Require(HasCode(read, "RESOURCE_PROVENANCE_INVALID"), "invalid winner has an explicit diagnostic");
    }
    Require(richReads == 9 && upper->reads == 0 && lower->probes == 0 && lower->reads == 0,
        "failed rich reads cannot invoke legacy or lower-priority fallback");
}

} // namespace

int main() {
    try {
        TestLayerAndArchivePriority();
        TestCanonicalPathAndNoCache();
        TestConflictAndFailuresBlockFallback();
        TestSourceRegistration();
        TestRichReadsFreezeWinnerAndKeepReadKeys();
        TestBadRichReadsNeverFallBack();
        std::cout << "ResourceResolver: 6 scenario groups passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "ResourceResolver: " << exception.what() << '\n';
        return 1;
    }
}
