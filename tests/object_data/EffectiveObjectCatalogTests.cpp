#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Resources;
using ReadResult = Result<std::optional<EffectiveCatalogFile>>;
int failures = 0;

void Check(bool okay, std::string_view message) {
    if (!okay) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char byte : text) bytes.push_back(static_cast<std::byte>(byte));
    return bytes;
}

std::string Table(std::string_view first = "D003", std::string_view file = "Models\\Map.mdl", bool extra = false) {
    std::string text = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"file\"\nC;X3;K\"Name\"\n";
    text += "C;X1;Y2;K\"" + std::string(first) + "\"\nC;X2;K\"" + std::string(file) + "\"\nC;X3;K\"Synthetic\"\n";
    if (extra) text += "C;X1;Y3;K\"d003\"\nC;X2;K\"Models\\Lower.mdl\"\n";
    return text + "E\n";
}

CatalogLoadOptions Options() {
    CatalogLoadOptions options;
    options.stringFiles.clear();
    options.sources = {{ObjectType::Doodad, {"Doodads/Doodads.slk"}, {"Units/Profile.txt"}}};
    return options;
}

struct Source {
    std::string id, identity;
    ResourceLayer layer = ResourceLayer::MapImport;
    int priority = ResourcePriority::MapArchive;
    std::map<std::string, std::vector<std::byte>, std::less<>> files;
    bool failRead = false;
    int reads = 0;

    ResourceSource Register() {
        return {id, layer, priority,
            [this](std::string_view name) { return Result<ResourceProbe>::Success(files.contains(name)
                ? ResourceProbe{ResourceProbeState::Found, std::string(name)} : ResourceProbe{}); },
            [this](std::string_view name) -> Result<std::vector<std::byte>> {
                ++reads;
                if (failRead) return Result<std::vector<std::byte>>::Failure("Test", "BROKEN_SELECTED_SOURCE", "fixture");
                return Result<std::vector<std::byte>>::Success(files.at(std::string(name)));
            }};
    }
};

ObjectCatalogReader Reader(ResourceResolver& resolver, std::span<Source*> sources, std::map<std::string, int>* calls = nullptr) {
    return [&resolver, sources, calls](std::string_view name) -> ReadResult {
        if (calls) ++(*calls)[std::string(name)];
        auto read = resolver.Read(name);
        if (!read.Succeeded()) {
            if (read.diagnostics.size() == 1 && read.diagnostics.front().code == "RESOURCE_NOT_FOUND") return ReadResult::Success(std::nullopt);
            return {{}, read.diagnostics};
        }
        for (const auto* source : sources) if (source->id == read.value->location.sourceId)
            return ReadResult::Success(EffectiveCatalogFile{read.value->location, source->identity, std::move(read.value->bytes)});
        return ReadResult::Failure("Test", "MISSING_IDENTITY", "fixture");
    };
}

void WholeFileAndIdentity() {
    auto options = Options();
    Source map{"map", "map-revision-1", ResourceLayer::MapImport, ResourcePriority::MapArchive};
    Source game{"war3.mpq", "game-root-1", ResourceLayer::Game, ResourcePriority::GameBase};
    map.files["DOODADS\\DOODADS.SLK"] = Bytes(Table());
    game.files["DOODADS\\DOODADS.SLK"] = Bytes(Table("D003", "Models\\Game.mdl", true));
    game.files["UNITS\\PROFILE.TXT"] = Bytes("[D003]\nName=Game profile\n");
    ResourceResolver resolver;
    Check(resolver.AddSource(map.Register()).Succeeded(), "register map");
    Check(resolver.AddSource(game.Register()).Succeeded(), "register game");
    Source* sources[]{&map, &game};
    std::map<std::string, int> calls;
    auto reader = Reader(resolver, sources, &calls);
    auto result = EffectiveObjectCatalog::Capture(reader, options);
    Check(result.Succeeded(), "map SLK plus game profile");
    if (!result.Succeeded()) return;
    const auto* object = result.value->Catalog().Find(ObjectType::Doodad, "D003");
    Check(object && object->Field("file") == "Models\\Map.mdl", "map whole table selected");
    Check(object && !object->nativeOverridesSupported, "map base table read cannot grant native write capability");
    Check(object && object->definitionSourceFile == "Doodads/Doodads.slk"
        && object->nameSourceFile == "Units/Profile.txt", "definition and displayed donor retain logical provenance");
    Check(!result.value->Catalog().Find(ObjectType::Doodad, "d003"), "game-only row not resurrected");
    Check(object && object->fieldSourceFiles.at("name") == "Units/Profile.txt", "field tracks logical contributing profile");
    const auto* input = result.value->Input("DOODADS/Doodads.slk");
    Check(input && input->present && input->location.sourceId == "map" && input->containerIdentity == "map-revision-1"
        && input->sha256.size() == 64, "table retains actual layer, container and digest");
    Check(calls.size() == 2 && calls.begin()->second == 1 && calls.rbegin()->second == 1, "every logical input captured once");
    Check(result.value->Verify(reader).Succeeded(), "same source verifies");
    map.identity = "map-revision-2";
    Check(!result.value->Verify(reader).Succeeded(), "same bytes with different container identity is stale");
}

void EmptyAndMissing() {
    Source map{"map", "map-1", ResourceLayer::MapImport, ResourcePriority::MapArchive};
    Source game{"game", "game-1", ResourceLayer::Game, ResourcePriority::GameBase};
    map.files["UNITS\\PROFILE.TXT"] = {};
    game.files["DOODADS\\DOODADS.SLK"] = Bytes(Table("D003", "Models\\Game.mdl", true));
    game.files["UNITS\\PROFILE.TXT"] = Bytes("[D003]\nName=Should not appear\n");
    ResourceResolver resolver;
    Check(resolver.AddSource(map.Register()).Succeeded() && resolver.AddSource(game.Register()).Succeeded(), "register empty profile sources");
    Source* sources[]{&map, &game};
    auto reader = Reader(resolver, sources);
    auto result = EffectiveObjectCatalog::Capture(reader, Options());
    Check(result.Succeeded(), "empty profile is a legal selected source");
    if (!result.Succeeded()) return;
    Check(result.value->Catalog().Find(ObjectType::Doodad, "D003")->displayName == "Synthetic", "empty map profile masks lower text");
    Check(result.value->Catalog().Find(ObjectType::Doodad, "D003")->nativeOverridesSupported,
        "an empty unrelated profile contributes no fields and does not revoke game-base write capability");
    Check(result.value->Catalog().Find(ObjectType::Doodad, "d003") != nullptr, "rawcode case remains distinct");
    const auto* profile = result.value->Input("Units/Profile.txt");
    Check(profile && profile->present && profile->byteSize == 0 && profile->sha256.size() == 64, "empty is present with digest");
    map.files.erase("UNITS\\PROFILE.TXT");
    Check(!result.value->Verify(reader).Succeeded(), "removing empty mask invalidates snapshot");

    game.files.erase("UNITS\\PROFILE.TXT");
    auto missing = EffectiveObjectCatalog::Capture(reader, Options());
    Check(missing.Succeeded() && !missing.value->Input("Units/Profile.txt")->present, "all-source missing is recorded");
    map.files["UNITS\\PROFILE.TXT"] = {};
    Check(missing.Succeeded() && !missing.value->Verify(reader).Succeeded(), "missing-to-empty is a source change");
}

void ErrorsDoNotFallBack() {
    Source map{"map", "map-1", ResourceLayer::MapImport, ResourcePriority::MapArchive};
    Source game{"game", "game-1", ResourceLayer::Game, ResourcePriority::GameBase};
    map.files["DOODADS\\DOODADS.SLK"] = Bytes(Table());
    game.files["DOODADS\\DOODADS.SLK"] = Bytes(Table("D003", "Models\\Wrong.mdl"));
    ResourceResolver resolver;
    Check(resolver.AddSource(map.Register()).Succeeded() && resolver.AddSource(game.Register()).Succeeded(), "register error sources");
    Source* sources[]{&map, &game};
    auto reader = Reader(resolver, sources);
    map.failRead = true;
    Check(!EffectiveObjectCatalog::Capture(reader, Options()).Succeeded() && game.reads == 0, "selected read failure never falls back");
    map.failRead = false;
    map.files["DOODADS\\DOODADS.SLK"] = Bytes("ID;PWXL\nC;X0;Y1;K\"alias\"\nE\n");
    // 另一个类别可正常读取时，坏表也不能像旧目录那样被忽略。
    map.files["UNITS\\ITEMDATA.SLK"] = Bytes(Table("I001"));
    auto options = Options();
    options.sources.push_back({ObjectType::Item, {"Units/ItemData.slk"}, {}});
    Check(!EffectiveObjectCatalog::Capture(reader, options).Succeeded() && game.reads == 0, "invalid selected table cannot hide behind valid category");
    map.files["DOODADS\\DOODADS.SLK"] = Bytes(Table());
    options = Options(); options.maximumFileBytes = 8;
    Check(!EffectiveObjectCatalog::Capture(reader, options).Succeeded() && game.reads == 0, "selected over-budget table refused");
    Check(!EffectiveObjectCatalog::Capture([](std::string_view) -> ReadResult { return {}; }, Options()).Succeeded(), "empty failure not interpreted as missing");
    Check(!EffectiveObjectCatalog::Capture([](std::string_view) -> ReadResult { throw std::runtime_error("fixture"); }, Options()).Succeeded(), "source exception retained as error");
}

void SameBytesHigherLayerAndSwitch() {
    Source map{"map", "map-1", ResourceLayer::MapImport, ResourcePriority::MapArchive};
    Source game{"game", "game-1", ResourceLayer::Game, ResourcePriority::GameBase};
    game.files["DOODADS\\DOODADS.SLK"] = Bytes(Table());
    ResourceResolver resolver;
    Check(resolver.AddSource(map.Register()).Succeeded() && resolver.AddSource(game.Register()).Succeeded(), "register layer change sources");
    Source* sources[]{&map, &game};
    auto reader = Reader(resolver, sources);
    auto initial = EffectiveObjectCatalog::Capture(reader, Options());
    Check(initial.Succeeded() && !initial.value->HasNonGameInputs(), "game-only catalog provenance");
    Check(initial.Succeeded() && initial.value->Catalog().Find(ObjectType::Doodad, "D003")->nativeOverridesSupported,
        "game base remains eligible for existing native override path");
    map.files["DOODADS\\DOODADS.SLK"] = game.files.begin()->second;
    Check(initial.Succeeded() && !initial.value->Verify(reader).Succeeded(), "equal bytes from new higher source invalidate");
    auto firstMap = EffectiveObjectCatalog::Capture(reader, Options());
    map.identity = "map-2";
    map.files["DOODADS\\DOODADS.SLK"] = Bytes(Table("D003", "Models\\Second.mdl"));
    auto secondMap = EffectiveObjectCatalog::Capture(reader, Options());
    Check(firstMap.Succeeded() && secondMap.Succeeded()
        && firstMap.value->Catalog().Find(ObjectType::Doodad, "D003")->Field("file") == "Models\\Map.mdl"
        && secondMap.value->Catalog().Find(ObjectType::Doodad, "D003")->Field("file") == "Models\\Second.mdl", "two maps with same rawcode remain independent");
}

void LeaseAndInputs() {
    auto options = Options();
    options.sources[0].profileFiles = {"Units/Profile.txt", "units\\PROFILE.txt"};
    const auto names = EffectiveObjectCatalog::InputNames(options);
    Check(names.Succeeded() && names.value->size() == 2, "fixed input domain is canonical and unique");
    options.sources[0].slkFiles = {"../unsafe.slk"};
    Check(!EffectiveObjectCatalog::InputNames(options).Succeeded(), "unsafe logical input refused");
    Source map{"map", "map-1", ResourceLayer::MapImport, ResourcePriority::MapArchive};
    map.files["DOODADS\\DOODADS.SLK"] = Bytes(Table());
    ResourceResolver resolver; Check(resolver.AddSource(map.Register()).Succeeded(), "register lease fixture");
    Source* sources[]{&map}; bool valid = true;
    auto loaded = EffectiveObjectCatalog::Capture(Reader(resolver, sources), Options(), [&] {
        return valid ? Status::Ok() : Status::Failure("Test", "LEASE_CHANGED", "fixture");
    });
    valid = false;
    Check(loaded.Succeeded() && !loaded.value->VerifyCapturedSources().Succeeded(), "owned source lease can revoke catalog qualification");
}

void TrustedStringsAndProfileValues() {
    auto options = Options();
    options.stringFiles = {"UI/Strings.txt"};
    Source map{"map", "map-1", ResourceLayer::MapImport, ResourcePriority::MapArchive};
    Source game{"game", "game-1", ResourceLayer::Game, ResourcePriority::GameBase};
    map.files["DOODADS\\DOODADS.SLK"] = Bytes(Table());
    map.files["UNITS\\PROFILE.TXT"] = Bytes("[D003]\nName=\"Alpha,Beta\",ignored\nArt=Textures\\CaseSensitive.blp\n[d003]\nName=lower\n");
    game.files["UI\\STRINGS.TXT"] = Bytes("WESTRING_SYNTHETIC=Trusted\n");
    ResourceResolver resolver; Check(resolver.AddSource(map.Register()).Succeeded()
        && resolver.AddSource(game.Register()).Succeeded(), "register trusted string and CSV fixture");
    Source* sources[]{&map, &game};
    auto reader = Reader(resolver, sources);
    auto loaded = EffectiveObjectCatalog::Capture(reader, options);
    Check(loaded.Succeeded(), "trusted strings and preserved profile values");
    if (loaded.Succeeded()) {
        const auto* object = loaded.value->Catalog().Find(ObjectType::Doodad, "D003");
        Check(object && object->displayName == "Alpha,Beta"
            && object->Field("name") == "\"Alpha,Beta\",ignored"
            && object->Field("art") == "Textures\\CaseSensitive.blp", "CSV and path are not normalized or reparsed");
        Check(!loaded.value->Catalog().Find(ObjectType::Doodad, "d003"), "profile-only section cannot create a ghost object");
        Check(loaded.value->Input("UI/Strings.txt")->gameOnly, "trusted dependency scope retained for revalidation");
    }
    map.files["UI\\STRINGS.TXT"] = Bytes("WESTRING_SYNTHETIC=Hostile\n");
    Check(!EffectiveObjectCatalog::Capture(reader, options).Succeeded(), "generic capture refuses a map-sourced trusted string table");
}

void StrictProfileHeaderAndLegacyCompatibility() {
    Source map{"map", "map-1", ResourceLayer::MapImport, ResourcePriority::MapArchive};
    Source game{"game", "game-1", ResourceLayer::Game, ResourcePriority::GameBase};
    game.files["DOODADS\\DOODADS.SLK"] = Bytes(Table("D003", "Models\\Game.mdl", true));
    game.files["UNITS\\PROFILE.TXT"] = Bytes("[D003]\nName=Lower profile must stay masked\n");
    map.files["UNITS\\PROFILE.TXT"] = Bytes("// [comment\n; [another comment\n\n  [D003\nName=Broken upper source\n");
    ResourceResolver resolver;
    Check(resolver.AddSource(map.Register()).Succeeded() && resolver.AddSource(game.Register()).Succeeded(),
        "register malformed selected profile over an original game base");
    Source* sources[]{&map, &game};
    auto reader = Reader(resolver, sources);
    const auto rejected = EffectiveObjectCatalog::Capture(reader, Options());
    Check(!rejected.Succeeded() && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "OBJECT_CATALOG_PROFILE_SYNTAX" && diagnostic.message.find("Units/Profile.txt") != std::string::npos
            && diagnostic.message.find("第 4 行") != std::string::npos;
    }), "unclosed selected section is an error with logical path and physical line");
    Check(game.reads == 1, "malformed higher profile cannot fall back to the valid lower profile");

    const auto legacy = StandardObjectCatalog::Load([&](const std::string& path) -> Result<std::vector<std::byte>> {
        auto resolved = resolver.Read(path);
        if (!resolved.Succeeded()) return {{}, resolved.diagnostics};
        return Result<std::vector<std::byte>>::Success(std::move(resolved.value->bytes));
    }, Options());
    Check(legacy.Succeeded() && legacy.value->Find(ObjectType::Doodad, "D003")->displayName == "Synthetic",
        "legacy non-strict parser retains the former skip behavior without lower-file fallback");

    map.files["UNITS\\PROFILE.TXT"] = Bytes("\xEF\xBB\xBF// [comment\r\n; [another comment\r\n"
        "[D003]\r\nName=First\r\nName=Second\r\n[D003] // retained suffix\r\n"
        "Name=\"Last,Name\",ignored\r\nArt=Textures\\CaseSensitive.blp\r\n[d003]\r\nName=lower\r\n");
    const auto accepted = EffectiveObjectCatalog::Capture(reader, Options());
    Check(accepted.Succeeded(), "strict profile still accepts BOM, CRLF, comments and repeated sections and keys");
    if (accepted.Succeeded()) {
        const auto* upper = accepted.value->Catalog().Find(ObjectType::Doodad, "D003");
        const auto* lower = accepted.value->Catalog().Find(ObjectType::Doodad, "d003");
        Check(upper && lower && upper->displayName == "Last,Name" && lower->displayName == "lower"
            && upper->Field("art") == "Textures\\CaseSensitive.blp", "last-wins values, rawcode case, CSV and paths are unchanged");
    }
}
} // namespace

int main() {
    WholeFileAndIdentity();
    EmptyAndMissing();
    ErrorsDoNotFallBack();
    SameBytesHigherLayerAndSwitch();
    LeaseAndInputs();
    TrustedStringsAndProfileValues();
    StrictProfileHeaderAndLegacyCompatibility();
    if (failures) std::cerr << failures << " checks failed\n";
    return failures ? 1 : 0;
}
