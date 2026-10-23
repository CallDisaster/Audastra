#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"
#include "War3MapCore/MapData/TerrainUniformDoodadFootprint.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Services/MapCreateService.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::ObjectData;
using namespace War3Map::Services;
constexpr std::size_t kSaveBytes = 64u * 1024u * 1024u;

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(const Status& status) {
    if (status.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : status.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) { Check({result.diagnostics}); throw std::runtime_error("missing result"); }
    return std::move(*result.value);
}
std::filesystem::path EnvironmentPath(const wchar_t* name) {
    const DWORD capacity = GetEnvironmentVariableW(name, nullptr, 0);
    if (!capacity) return {};
    std::wstring value(capacity, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), capacity);
    Require(length > 0 && length < capacity, "environment path changed while reading");
    value.resize(length); return value;
}
struct OwnedDirectory {
    std::filesystem::path path;
    bool keep = false;
    OwnedDirectory() {
        const auto evidence = EnvironmentPath(L"FORGE_TERRAIN_SAVE_EVIDENCE_DIR");
        keep = !evidence.empty();
        path = (keep ? evidence : std::filesystem::temp_directory_path()) / ("native-objects-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        Require(std::filesystem::create_directories(path), "exclusive real-source evidence directory");
    }
    ~OwnedDirectory() { if (!keep) { std::error_code error; std::filesystem::remove_all(path, error); } }
};

void SpecialWaterDeletionUsesPublicSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const std::string& rawcode,
    const DoodadPlacements& ordinary, const std::vector<std::byte>& w3b,
    const std::vector<std::byte>& script, bool repeat = false) {
    const auto scenario = directory / (repeat ? "special-water-delete-sequence" : "special-water-delete");
    Require(std::filesystem::create_directory(scenario), "exclusive special water deletion scenario");
    const auto parsed = Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e"))));
    const auto before = *parsed.Snapshot();
    auto placements = ordinary;
    placements.specialDoodads = {{rawcode, 0, {32, 32}}, {rawcode, 0, {44, 44}}};
    const auto beforeDoo = Take(placements.Serialize());
    const std::array<MemberReplacement, 2> members{{{"war3map.doo", beforeDoo}, {"war3map.w3b", w3b}}};
    const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", members)).path));
    const auto brushResources = Take(TerrainPathingSave::CaptureWaterBrushRules(seeded, before, before,
        placements, game, true, kSaveBytes));
    Require(brushResources.WaterBrushRules() && brushResources.WaterSpecialShapes().size() == 2,
        "actual typed resource capture supplies both special model shapes");
    const TerrainStrokePoint point{before.header.offset[0] + 32 * 128, before.header.offset[1] + 32 * 128};
    const auto anchor = Take(CaptureTerrainWaterStrokeAnchor(before, point));
    const TerrainWaterBrush brush{TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 1, 0, 42, anchor};
    auto candidate = Take(PrepareTerrainWaterStroke(before, *brushResources.WaterBrushRules(), brush,
        std::span(&point, 1), placements, brushResources.WaterSpecialShapes()));
    Require(candidate.result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0}
        && candidate.doodads.specialDoodads.size() == 1
        && candidate.doodads.specialDoodads.front().position == placements.specialDoodads.back().position,
        "actual water stroke removes the intersected model and retains the distant swap-last model");
    if (repeat) {
        const auto nextResources = Take(TerrainPathingSave::CaptureWaterBrushRules(seeded, before,
            candidate.terrain, candidate.doodads, game, true, kSaveBytes));
        const TerrainStrokePoint next{before.header.offset[0] + 44 * 128, before.header.offset[1] + 44 * 128};
        const auto nextAnchor = Take(CaptureTerrainWaterStrokeAnchor(candidate.terrain, next));
        const TerrainWaterBrush nextBrush{TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 1, 0, 43, nextAnchor};
        auto second = Take(PrepareTerrainWaterStroke(candidate.terrain, *nextResources.WaterBrushRules(), nextBrush,
            std::span(&next, 1), candidate.doodads, nextResources.WaterSpecialShapes()));
        Require(second.result.removedSpecialSourceRecords == std::vector<std::uint32_t>{0}
            && second.doodads.specialDoodads.empty(), "second stroke consumes the current swap-last DOO state");
        candidate = std::move(second);
    }
    const auto afterDoo = Take(candidate.doodads.Serialize());
    const auto afterW3e = Take(SerializeMapTerrain(candidate.terrain));
    const auto oldRules = Take(TerrainPathingSave::CaptureNative(seeded, before, before, game, kSaveBytes));
    const auto oldNative = Take(oldRules.GenerateNative(before, kSaveBytes));
    const std::array<MemberReplacement, 2> afterMembers{{{"war3map.w3e", afterW3e}, {"war3map.doo", afterDoo}}};
    const auto expectedArchive = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(
        scenario / "expected-inputs.w3x", afterMembers)).path));
    // 前后分别捕获实际DOO，独立检查保存链不会误用删除后的集合去解释旧WPM。
    // 像其它实际资源专项一样，这里复用生成器，不冒充原版或独立算法oracle。
    const auto newRules = Take(TerrainPathingSave::CaptureNative(expectedArchive,
        candidate.terrain, candidate.terrain, game, kSaveBytes));
    const auto newNative = Take(newRules.GenerateNative(candidate.terrain, kSaveBytes));
    Require(oldNative != newNative, "real water deletion changes the complete native pathing contribution");
    auto oldWpm = Take(seeded.ReadMember("war3map.wpm"));
    Require(oldWpm.size() == oldNative.size() + 16 && oldNative.size() == newNative.size(),
        "real water deletion keeps native pathing dimensions");
    auto expectedWpm = oldWpm;
    for (std::size_t i = 0; i < oldNative.size(); ++i) {
        oldWpm[i + 16] = (oldWpm[i + 16] & std::byte{0x11}) | oldNative[i];
        expectedWpm[i + 16] = (oldWpm[i + 16] & std::byte{0x11}) | newNative[i];
    }
    const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", oldWpm}}};
    const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", pathing)).path));
    MapEditSaveRequest request;
    request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
    request.toolRoot = tool; request.gameRoot = game;
    request.expectedSourceSha256 = source.SourceSha256();
    request.expectedTerrainSha256 = Take(Sha256(Take(source.ReadMember("war3map.w3e"))));
    MapEditTerrainStroke stroke;
    stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{32, 32}};
    TerrainWaterStrokeOptions options;
    options.shape = TerrainBrushShape::Square; options.randomSeed = 42;
    stroke.waterBrush = options; request.terrainStrokes = {stroke};
    if (repeat) {
        stroke.points = {{44, 44}};
        stroke.waterBrush->randomSeed = 43;
        request.terrainStrokes.push_back(stroke);
    }
    const auto saved = Take(SaveMapEditCandidate(request));
    const auto readback = Take(MapArchiveSession::Open(saved.candidate));
    Require(saved.scriptPreserved && Take(readback.ReadMember("war3map.j")) == script,
        "public special deletion preserves the checked source script");
    Require(Take(readback.ReadMember("war3map.w3e")) == afterW3e
        && Take(readback.ReadMember("war3map.doo")) == afterDoo
        && Take(readback.ReadMember("war3map.wpm")) == expectedWpm,
        "public save reads back complete coupled W3E, DOO and WPM with the correct old and new model sets");
    Require(Take(readback.ReadMember("war3map.w3b")) == w3b,
        "public special deletion preserves every native destructable byte");
    for (const auto* member : {"war3map.shd", "war3map.mmp"}) {
        Require(readback.HasMember(member) == source.HasMember(member)
            && (!source.HasMember(member) || Take(readback.ReadMember(member)) == Take(source.ReadMember(member))),
            "water deletion keeps existing shadow and minimap icon bytes or their absence");
    }
    Check(newRules.VerifyReadback(readback, kSaveBytes));
    if (repeat) {
        // 实际资源下补齐journal重放到正常地形保存计划的闭环；
        // 工程容器/Qt安装另有独立回归，这里不冒充原版或游戏验收。
        const auto baselineTerrain = Take(source.ReadMember("war3map.w3e"));
        const auto baselinePathing = Take(source.ReadMember("war3map.wpm"));
        const auto document = MapDocument::FromArchive(source);
        std::vector<TerrainStrokeIntent> intents;
        for (const auto& item : request.terrainStrokes) {
            TerrainStrokeIntent intent;
            intent.waterBrush = item.waterBrush;
            intent.followDoodads = false;
            for (const auto& sample : item.points) {
                intent.points.push_back({before.header.offset[0] + sample.x * 128,
                    before.header.offset[1] + sample.y * 128});
            }
            intents.push_back(std::move(intent));
        }
        const auto replayed = Take(TerrainPathingSave::ReplayStrokes(source, document,
            baselineTerrain, baselinePathing, intents, game));
        const auto journal = Take(TerrainPathingSave::CaptureJournal(source, *replayed.session));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal));
        const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
        Require(decoded.strokes.size() == 2, "actual resource journal retains both complete water strokes");
        auto restored = Take(TerrainPathingSave::RestoreJournal(source, document,
            baselineTerrain, baselinePathing, decoded, *replayed.session, game));
        Require(restored.receipts.size() == 2 && restored.session->RequiresTerrainStrokeJournal(),
            "actual source replay restores complete receipts and private special deletion lineage");
        const auto plan = Take(TerrainPathingSave::PrepareForSave(source, *restored.session, game));
        auto replacements = restored.session->Replacements();
        Check(plan.InstallPathingOutput(*restored.session, replacements));
        const auto written = Take(source.SaveCandidate(scenario / "journal-restored.w3x", replacements));
        const auto reopened = Take(MapArchiveSession::Open(written.path));
        Check(plan.VerifyReadback(reopened));
        Require(Take(reopened.ReadMember("war3map.w3e")) == afterW3e
            && Take(reopened.ReadMember("war3map.doo")) == afterDoo
            && Take(reopened.ReadMember("war3map.wpm")) == expectedWpm
            && Take(reopened.ReadMember("war3map.j")) == script,
            "journal-restored actual-resource save preserves complete terrain, placements, pathing and checked script");
        for (const auto* member : {"war3map.shd", "war3map.mmp"}) {
            Require(reopened.HasMember(member) == source.HasMember(member)
                && (!source.HasMember(member) || Take(reopened.ReadMember(member)) == Take(source.ReadMember(member))),
                "actual-resource journal save preserves baked shadow and minimap icon bytes or absence");
        }
        std::cout << "actual_journal_restore_save_passed strokes=2 wire_bytes=" << wire.size()
            << " source_sha256=" << source.SourceSha256() << " candidate_sha256=" << written.sha256
            << " native_editor_verified=false game_verified=false\n";
    }
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(),
        "public water deletion leaves its original map copy untouched");
    std::cout << (repeat ? "public_special_water_delete_sequence_passed object=" : "public_special_water_delete_passed object=") << rawcode
        << " source_sha256=" << source.SourceSha256() << " candidate_sha256=" << saved.candidateSha256
        << " native_editor_verified=false game_verified=false\n";
}

void DryRampChangeUsesPublicSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const DoodadPlacements& doodads,
    const std::vector<std::byte>& w3b, const std::vector<std::byte>& script, bool create, bool distantWater = false) {
    const auto scenario = directory / (std::string(create ? "dry-ramp-create" : "dry-ramp-remove")
        + (distantWater ? "-with-distant-water" : ""));
    Require(std::filesystem::create_directory(scenario), "exclusive dry ramp change scenario");
    auto before = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(before.header.width > 34 && before.header.height > 33, "dry ramp fixture fits the original terrain");
    for (std::uint32_t y = 0; y < before.header.height; ++y) for (std::uint32_t x = 0; x < before.header.width; ++x) {
        auto& vertex = before.vertices[std::size_t{y} * before.header.width + x];
        Require(!vertex.HasWater() && !vertex.IsRamp(), "original dry ramp fixture starts without water or ramps");
        vertex.cliffAndLayerRaw = static_cast<std::uint8_t>((vertex.cliffAndLayerRaw & 0xf0) | (x <= 31 ? 3 : 2));
    }
    if (!create) for (const auto x : {31u, 32u, 33u}) before.vertices[32 * before.header.width + x].textureAndFlagsRaw |= 0x10;
    if (distantWater) {
        auto& wet = before.vertices[4 * before.header.width + 4];
        wet.textureAndFlagsRaw |= 0x40;
        wet.waterHeightAndFlagsRaw = static_cast<std::uint16_t>((wet.groundHeightRaw & 0x3fff) + 1024);
    }
    RefreshTerrainDerived(before, 0, 0, before.header.width, before.header.height);
    const auto beforeW3e = Take(SerializeMapTerrain(before));
    const auto doo = Take(doodads.Serialize());
    const std::array<MemberReplacement, 3> members{{{"war3map.w3e", beforeW3e}, {"war3map.doo", doo}, {"war3map.w3b", w3b}}};
    const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", members)).path));
    const auto rules = Take(TerrainPathingSave::CaptureNative(seeded, before, before, game, kSaveBytes));
    const auto oldNative = Take(rules.GenerateNative(before, kSaveBytes));
    auto after = before;
    for (const auto x : {31u, 32u, 33u}) {
        auto& flags = after.vertices[32 * after.header.width + x].textureAndFlagsRaw;
        flags = static_cast<std::uint8_t>(create ? flags | 0x10 : flags & 0xef);
    }
    const auto afterW3e = Take(SerializeMapTerrain(after));
    const std::array<MemberReplacement, 1> afterMembers{{{"war3map.w3e", afterW3e}}};
    const auto expectedArchive = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "expected-inputs.w3x", afterMembers)).path));
    const auto newRules = Take(TerrainPathingSave::CaptureNative(expectedArchive, after, after, game, kSaveBytes));
    const auto newNative = Take(newRules.GenerateNative(after, kSaveBytes));
    Require(oldNative != newNative, "real dry ramp change updates native pathing at the cliff wall");
    auto oldWpm = Take(seeded.ReadMember("war3map.wpm"));
    Require(oldWpm.size() == oldNative.size() + 16 && newNative.size() == oldNative.size(), "real ramp pathing dimensions");
    auto expectedWpm = oldWpm;
    // 实际资源专项复用生成器造旧来源，只证明公共链与真实winner；独立字节oracle在CPU/保存测试。
    for (std::size_t i = 0; i < oldNative.size(); ++i) {
        oldWpm[i + 16] = (oldWpm[i + 16] & std::byte{0x11}) | oldNative[i];
        expectedWpm[i + 16] = (oldWpm[i + 16] & std::byte{0x11}) | newNative[i];
    }
    const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", oldWpm}}};
    const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", pathing)).path));
    MapEditSaveRequest request;
    request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
    request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = source.SourceSha256();
    request.expectedTerrainSha256 = Take(Sha256(beforeW3e));
    MapEditTerrainStroke stroke; stroke.brush.kind = TerrainBrushKind::Ramp; stroke.brush.value = create;
    stroke.points = {{before.header.offset[0] + 31.5 * 128, before.header.offset[1] + 32 * 128}};
    request.terrainStrokes = {stroke};
    const auto saved = Take(SaveMapEditCandidate(request));
    const auto readback = Take(MapArchiveSession::Open(saved.candidate));
    Require(saved.scriptPreserved && Take(readback.ReadMember("war3map.j")) == script, "public dry ramp save preserves checked script");
    Require(Take(readback.ReadMember("war3map.w3e")) == afterW3e && Take(readback.ReadMember("war3map.wpm")) == expectedWpm,
        "public dry ramp change reads back complete coupled W3E and WPM");
    Require(Take(readback.ReadMember("war3map.doo")) == doo && Take(readback.ReadMember("war3map.w3b")) == w3b,
        "public dry ramp save preserves native ordinary objects");
    Check(newRules.VerifyReadback(readback, kSaveBytes));
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "public dry ramp save leaves source untouched");
    std::cout << (create ? "public_dry_ramp_create_passed source_sha256=" : "public_dry_ramp_remove_passed source_sha256=") << source.SourceSha256()
        << " candidate_sha256=" << saved.candidateSha256 << " distant_water=" << distantWater
        << " native_editor_verified=false game_verified=false\n";
}

void RealResourcesUsePublicMapEditSave(const std::filesystem::path& tool, const std::filesystem::path& game) {
    OwnedDirectory directory;
    try {
        MapCreateRequest create;
        create.options.name = "Native object pathing regression";
        create.target = directory.path / "created.w3x"; create.workspace = directory.path / "seed";
        create.toolRoot = tool; create.gameRoot = game;
        const auto created = Take(CreateMapCandidate(create));
        Require(created.scriptRebuilt, "original map has a real-toolchain checked script");
        auto original = Take(MapArchiveSession::Open(created.path));
        const auto sourceScript = Take(original.ReadMember("war3map.j"));
        const auto parsed = Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e"))));
        auto before = *parsed.Snapshot();
        Require(before.header.width > 32 && before.header.height > 32, "original map contains the edited center");
        auto mounted = Take(Game::GameDataSource::MountStrict(game, before.header.mainTileset));
        constexpr std::array<const char*, 4> names{"Units/DestructableData.slk", "Doodads/Doodads.slk",
            "Units/DestructableMetaData.slk", "Doodads/DoodadMetaData.slk"};
        std::array<Game::GameFileContents, names.size()> captured;
        for (std::size_t i = 0; i < names.size(); ++i) {
            captured[i] = Take(mounted.ReadFileBoundedWithSource(names[i], 2u * 1024u * 1024u));
            std::cout << names[i] << " winner=" << captured[i].archiveName << " bytes=" << captured[i].bytes.size()
                << " sha256=" << Take(Sha256(captured[i].bytes)) << '\n';
            Require(!original.HasMember(names[i]), "fixture must use actual game-winning object tables");
        }
        const auto reader = [&](const std::string& name) -> Result<std::vector<std::byte>> {
            for (std::size_t i = 0; i < names.size(); ++i)
                if (Text::LowerAscii(name) == Text::LowerAscii(names[i])) return Result<std::vector<std::byte>>::Success(captured[i].bytes);
            return Result<std::vector<std::byte>>::Failure("test", "UNEXPECTED_OBJECT_SOURCE", "测试不能用未捕获的物编来源。");
        };
        CatalogLoadOptions catalogOptions;
        catalogOptions.sources = {{ObjectType::Destructable, {names[0]}, {}}, {ObjectType::Doodad, {names[1]}, {}}};
        catalogOptions.stringFiles.clear(); catalogOptions.maximumFileBytes = 2u * 1024u * 1024u;
        catalogOptions.rejectInvalidPresentSources = true;
        const auto catalog = Take(StandardObjectCatalog::Load(reader, catalogOptions));
        ObjectFieldSchemaOptions schemaOptions;
        schemaOptions.sources = {{ObjectType::Destructable, names[2]}, {ObjectType::Doodad, names[3]}};
        schemaOptions.stringFiles.clear(); schemaOptions.worldEditDataFile.clear(); schemaOptions.unitEditorDataFile.clear();
        schemaOptions.maximumFileBytes = 2u * 1024u * 1024u;
        const auto schema = Take(ObjectFieldSchema::Load(reader, schemaOptions));
        std::string rawcode;
        std::string doodadRawcode, doodadPath;
        std::string originalDoodadRawcode, originalDoodadPath;
        std::string uniformRawcode, uniformPath;
        Game::GameFileContents uniformSource;
        std::string patternedRawcode, patternedPath;
        Game::GameFileContents patternedSource;
        ModificationKey walkable, doodadPathField;
        for (const auto& object : catalog.Entries()) {
            if (object.key.type != ObjectType::Destructable || object.Field("walkable") != "0"
                || catalog.Find(ObjectType::Doodad, object.key.rawcode)) continue;
            ObjectFieldContext context; context.rawcode = object.key.rawcode; context.baseCode = object.Field("code");
            const auto field = schema.MatchSourceField(ObjectType::Destructable, "walkable", context);
            if (!field.definition || !field.safeToEdit || field.sourceKey != "walkable" || field.sourceIndex
                || field.definition->nativeValueType != NativeValueType::Int32 || field.definition->repeat != 0
                || field.definition->data != 0 || Text::LowerAscii(field.definition->sourceTable) != "destructabledata") continue;
            const auto type = Text::LowerAscii(field.definition->valueType);
            if (type != "bool" && type != "int") continue;
            rawcode = object.key.rawcode; walkable = field.modification; break;
        }
        Require(!rawcode.empty(), "actual metadata contains an unambiguous standard nonwalkable destructable");
        for (const auto& object : catalog.Entries()) {
            if (object.key.type != ObjectType::Doodad || catalog.Find(ObjectType::Destructable, object.key.rawcode)) continue;
            const auto path = object.fields.find("pathtex");
            if (path == object.fields.end() || (!path->second.empty() && path->second != "_" && path->second != "none")) continue;
            doodadRawcode = object.key.rawcode; doodadPath = path->second; break;
        }
        Require(!doodadRawcode.empty(), "actual game table contains a uniquely classified standard doodad with explicitly disabled pathTex");
        const auto pathField = [&](const std::string& id) {
            const auto* object = catalog.Find(ObjectType::Doodad, id);
            Require(object != nullptr, "actual standard doodad exists");
            ObjectFieldContext context; context.rawcode = id; context.baseCode = object->Field("code");
            const auto field = schema.MatchSourceField(ObjectType::Doodad, "pathtex", context);
            Require(field.definition && field.safeToEdit && field.sourceKey == "pathtex" && !field.sourceIndex
                && field.definition->nativeValueType == NativeValueType::String && field.definition->repeat == 0
                && field.definition->data == 0 && Text::LowerAscii(field.definition->sourceTable) == "doodaddata"
                && Text::LowerAscii(field.definition->sourceField) == "pathtex", "actual doodad metadata has a unique zero-index pathTex");
            return field.modification;
        };
        for (const auto& object : catalog.Entries()) {
            if (object.key.type != ObjectType::Doodad || catalog.Find(ObjectType::Destructable, object.key.rawcode)) continue;
            const auto category = object.Field("category");
            if (category.size() != 1 || category[0] < 0x21 || category[0] > 0x7e
                || category == "_" || category == "C") continue;
            const auto path = std::string(object.Field("pathtex"));
            if (!Text::LowerAscii(path).ends_with(".tga")) continue;
            auto file = mounted.ReadFileBoundedWithSource(path, 512u * 1024u);
            if (!file.Succeeded() || !file.value) continue;
            const auto decoded = DecodeUniformDoodadFootprintTga(file.value->bytes);
            if (!decoded.Succeeded() || !decoded.value || !(decoded.value->attributes & 0x33) || decoded.value->side > 4) continue;
            uniformRawcode = object.key.rawcode; uniformPath = path; uniformSource = std::move(*file.value); break;
        }
        Require(!uniformRawcode.empty(), "actual game table has a non-C standard doodad with a supported nonempty uniform TGA");
        Require(!original.HasMember(uniformPath), "uniform fixture must capture the actual game-winning TGA");
        std::cout << "uniform_path=" << uniformPath << " winner=" << uniformSource.archiveName
            << " bytes=" << uniformSource.bytes.size() << " sha256=" << Take(Sha256(uniformSource.bytes)) << '\n';
        for (const auto& object : catalog.Entries()) {
            if (object.key.type != ObjectType::Doodad || catalog.Find(ObjectType::Destructable, object.key.rawcode)) continue;
            const auto category=object.Field("category"), fixed=object.Field("fixedrot");
            if(category.size()!=1 || category[0]<0x21 || category[0]>0x7e || category=="_" || category=="C" || fixed.empty()) continue;
            float degrees=0; const auto converted=std::from_chars(fixed.data(),fixed.data()+fixed.size(),degrees);
            if(converted.ec!=std::errc{} || converted.ptr!=fixed.data()+fixed.size() || !std::isfinite(degrees) || degrees>360 || degrees < -360) continue;
            const auto path=std::string(object.Field("pathtex"));
            if(!Text::LowerAscii(path).ends_with(".tga")) continue;
            auto file=mounted.ReadFileBoundedWithSource(path,512u*1024u);
            if(!file.Succeeded() || !file.value) continue;
            const auto decoded=DecodeDoodadFootprintTga(file.value->bytes);
            if(!decoded.Succeeded() || !decoded.value || (decoded.value->uniform && decoded.value->width==decoded.value->height)) continue;
            patternedRawcode=object.key.rawcode; patternedPath=path; patternedSource=std::move(*file.value); break;
        }
        Require(!patternedRawcode.empty(),"actual game table has a non-C standard doodad with a patterned or rectangular supported TGA");
        Require(!original.HasMember(patternedPath),"patterned fixture captures the actual game-winning TGA");
        std::cout<<"patterned_path="<<patternedPath<<" winner="<<patternedSource.archiveName
            <<" bytes="<<patternedSource.bytes.size()<<" sha256="<<Take(Sha256(patternedSource.bytes))<<'\n';
        originalDoodadRawcode = uniformRawcode; originalDoodadPath = uniformPath;
        doodadPathField = pathField(uniformRawcode);
        auto originalDoodad = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
        Check(originalDoodad.AddOriginalObject(originalDoodadRawcode));
        Check(originalDoodad.AddModification({ObjectType::Doodad, originalDoodadRawcode}, doodadPathField, NativeValueType::String, std::string{}));
        const auto originalDoodadW3d = Take(NativeObjectCodec(ObjectType::Doodad).Serialize(originalDoodad));
        auto native = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
        Check(native.AddOriginalObject(rawcode));
        Check(native.AddModification({ObjectType::Destructable, rawcode}, walkable, NativeValueType::Int32, std::int32_t{0}));
        const auto w3b = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(native));
        DoodadPlacements doodads; DoodadPlacement placed;
        placed.rawcode = rawcode; placed.creationId = 1; placed.flags = 2; placed.scale = {1, 1, 1};
        placed.position = {before.header.offset[0] + 32 * 128, before.header.offset[1] + 32 * 128, 0};
        doodads.doodads.push_back(placed);
        for (auto& vertex : before.vertices) {
            vertex.textureAndFlagsRaw |= 0x40;
            vertex.waterHeightAndFlagsRaw = static_cast<std::uint16_t>((vertex.waterHeightAndFlagsRaw & 0xc000) | (8192 + 160 * 4));
        }
        RefreshTerrainDerived(before, 0, 0, before.header.width, before.header.height);
        const auto beforeW3e = Take(SerializeMapTerrain(before));
        auto after = before;
        const auto center = std::size_t{32} * before.header.width + 32;
        after.vertices[center].waterHeightAndFlagsRaw = static_cast<std::uint16_t>((after.vertices[center].waterHeightAndFlagsRaw & 0xc000) | 8192);
        const auto expectedW3e = Take(SerializeMapTerrain(after));
        const bool hasW3d = original.HasMember("war3map.w3d");
        const auto sourceW3d = hasW3d ? Take(original.ReadMember("war3map.w3d")) : std::vector<std::byte>{};
        // 九场景复用一次原创地图创建、脚本编译和实际表捕获；特殊DOO也走公开保存。
        constexpr std::array<const char*, 9> scenarioNames{"destructable", "mixed-doodad", "original-doodad", "uniform-doodad",
            "patterned-doodad", "original-patterned", "custom-patterned", "custom-cleared", "special-doodad"};
        for (unsigned index = 0; index < scenarioNames.size(); ++index) {
            const bool withSpecial = index == 8;
            const bool withDoodad = index != 0 && !withSpecial, withOriginal = index == 2 || index == 5, withUniform = index == 3;
            const bool withPattern = index == 4 || index == 5 || index == 6, withCustom = index == 6 || index == 7;
            const bool withNative = withOriginal || withCustom;
            const std::string selectedDoodad = withCustom ? "Zr01" : withPattern ? patternedRawcode : withUniform ? uniformRawcode
                : withOriginal ? originalDoodadRawcode : doodadRawcode;
            auto nativeW3d = originalDoodadW3d;
            if (index >= 5 && !withSpecial) {
                const auto& parent = withPattern ? patternedRawcode : uniformRawcode;
                auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
                if (withCustom) {
                    Require(!catalog.Find(ObjectType::Doodad, selectedDoodad) && !catalog.Find(ObjectType::Destructable, selectedDoodad),
                        "original custom test ID must not shadow game data");
                    Check(document.AddCustomObject(selectedDoodad, parent));
                } else Check(document.AddOriginalObject(selectedDoodad));
                Check(document.AddModification({ObjectType::Doodad, selectedDoodad}, pathField(parent),
                    NativeValueType::String, withPattern ? patternedPath : std::string{}));
                nativeW3d = Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document));
            }
            const auto scenario = directory.path / scenarioNames[index];
            Require(std::filesystem::create_directory(scenario), "exclusive real-source scenario directory");
            auto placements = doodads;
            if (withSpecial) placements.specialDoodads.push_back({uniformRawcode, 0, {34,32}});
            if (withDoodad) {
                auto added = placed; added.rawcode = selectedDoodad; added.creationId = 2;
                if (withUniform || withPattern) {
                    // 非格点摆放，与水位修改区分离，避免静态位掩盖真实变化。
                    added.position[0] += 256.5f; added.position[1] += 0.25f;
                }
                placements.doodads.push_back(added);
                if (withUniform || withPattern) {
                    added.creationId = 3; added.position[0] += 64;
                    placements.doodads.push_back(std::move(added));
                }
            }
            const auto doo = Take(placements.Serialize());
            std::vector<MemberReplacement> seed{{"war3map.w3e", beforeW3e}, {"war3map.doo", doo}, {"war3map.w3b", w3b}};
            if (withNative) seed.push_back({"war3map.w3d", nativeW3d});
            auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", seed)).path));
            const auto rules = Take(TerrainPathingSave::CaptureNative(seeded, before, after, game, kSaveBytes));
            const auto oldNative = Take(rules.GenerateNative(before, kSaveBytes));
            const auto newNative = Take(rules.GenerateNative(after, kSaveBytes));
            Require(oldNative != newNative, "each public save scenario crosses a real water pathing threshold");
            std::cout << "selected_object=" << rawcode << " metadata_field=" << walkable.fieldId
                << " doodad=" << (withDoodad ? selectedDoodad : "absent") << " original=" << withOriginal << " custom=" << withCustom
                << " special=" << (withSpecial ? uniformRawcode : "absent")
                << " standard_pathTex=" << (withPattern ? patternedPath : (withUniform || withCustom || withSpecial) ? uniformPath : withOriginal ? originalDoodadPath : withDoodad ? doodadPath : "not-applicable")
                << " pathTex_field=" << (withNative ? pathField(withPattern ? patternedRawcode : uniformRawcode).fieldId : "not-applicable")
                << " captured_bytes=" << rules.SourceBytes() << " cumulative_capture_budget=" << rules.CaptureBudgetBytes() << '\n';
            auto wpm = Take(seeded.ReadMember("war3map.wpm"));
            Require(wpm.size() == oldNative.size() + 16 && oldNative.size() == newNative.size(), "real pathing shape matches original fixture");
            auto expectedWpm = wpm;
            for (std::size_t i = 0; i < oldNative.size(); ++i) {
                wpm[i + 16] = (wpm[i + 16] & std::byte{0x11}) | oldNative[i];
                expectedWpm[i + 16] = (wpm[i + 16] & std::byte{0x11}) | newNative[i];
            }
            // 此原图一致性夹具复用生成器，只验证实际资源和公开保存链；不是原版算法对照。
            const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", std::move(wpm)}}};
            auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", pathing)).path));
            const auto sourceSha = Take(Sha256File(source.SourcePath()));
            MapEditSaveRequest request;
            request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
            request.toolRoot = tool; request.gameRoot = game;
            request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(beforeW3e));
            MapEditTerrainStroke stroke;
            stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{32, 32}};
            stroke.brush.kind = TerrainBrushKind::WaterHeight; stroke.brush.radius = 0; stroke.brush.waterHeight = 0;
            request.terrainStrokes = {stroke}; // 保留Automatic，覆盖最终小地图准备的正常保存路径。
            const auto saved = Take(SaveMapEditCandidate(request));
            Require(saved.scriptPreserved, "public terrain save preserves the original checked script");
            const auto readback = Take(MapArchiveSession::Open(saved.candidate));
            Require(Take(readback.ReadMember("war3map.w3e")) == expectedW3e, "complete independently edited W3E matches public output");
            Require(Take(readback.ReadMember("war3map.wpm")) == expectedWpm, "complete WPM matches frozen actual source rules");
            Require(Take(readback.ReadMember("war3map.j")) == sourceScript, "public save preserves every source script byte");
            Require(Take(readback.ReadMember("war3map.w3b")) == w3b && Take(readback.ReadMember("war3map.doo")) == doo,
                "public save preserves native object and placement bytes");
            Require(readback.HasMember("war3map.w3d") == (withNative || hasW3d)
                && (!(withNative || hasW3d) || Take(readback.ReadMember("war3map.w3d")) == (withNative ? nativeW3d : sourceW3d)),
                "public save preserves W3D content or its actual absence");
            Require(Take(Sha256File(source.SourcePath())) == sourceSha, "public save leaves the source map untouched");
            Check(rules.VerifyReadback(readback, kSaveBytes));
            std::cout << "public_map_edit_save_passed source_sha256=" << sourceSha << " candidate_sha256=" << saved.candidateSha256
                << " native_editor_verified=false game_verified=false\n";
            if (index == 2) {
                auto overrideDocument = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
                Check(overrideDocument.AddOriginalObject(selectedDoodad));
                Check(overrideDocument.AddModification({ObjectType::Doodad, selectedDoodad}, doodadPathField,
                    NativeValueType::String, originalDoodadPath));
                const std::array<MemberReplacement, 1> related{{{"war3map.w3d", Take(NativeObjectCodec(ObjectType::Doodad).Serialize(overrideDocument))}}};
                const auto overridden = Take(MapArchiveSession::Open(Take(source.SaveCandidate(scenario / "related-override.w3x", related)).path));
                auto blocked = request;
                blocked.sourceMap = overridden.SourcePath(); blocked.expectedSourceSha256 = overridden.SourceSha256();
                blocked.targetCandidate = scenario / "must-not-exist.w3x";
                const auto rejected = SaveMapEditCandidate(blocked);
                Require(!rejected.Succeeded() && !rejected.value && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
                    return diagnostic.code == "TERRAIN_PATHING_SOURCE_UNEXPLAINED";
                }), "restoring actual nonempty pathTex without its old WPM footprint must reject the unexplained source");
                Require(!std::filesystem::exists(blocked.targetCandidate), "refused public save must not produce a candidate");
                Require(Take(Sha256File(overridden.SourcePath())) == overridden.SourceSha256()
                    && Take(Sha256File(source.SourcePath())) == sourceSha, "refused public save preserves both source copies");
                std::cout << "nonempty_doodad_wpm_mismatch_refused source_sha256=" << overridden.SourceSha256() << '\n';
            }
        }
        SpecialWaterDeletionUsesPublicSave(original, directory.path, tool, game, uniformRawcode, doodads, w3b, sourceScript);
        SpecialWaterDeletionUsesPublicSave(original, directory.path, tool, game, uniformRawcode, doodads, w3b, sourceScript, true);
        for (const bool distantWater : {false, true}) for (const bool createRamp : {false, true})
            DryRampChangeUsesPublicSave(original, directory.path, tool, game, doodads, w3b, sourceScript, createRamp, distantWater);
    } catch (...) {
        directory.keep = true;
        std::cerr << "Real-source failure artifacts preserved at " << IO::PathUtf8(directory.path) << '\n';
        throw;
    }
}
} // namespace

int main() {
    try {
        const auto tool = EnvironmentPath(L"FORGE_YDWE_ROOT");
        const auto game = EnvironmentPath(L"FORGE_WORLD_GAME_ROOT");
        if (tool.empty() || game.empty()) {
            std::cout << "SKIP native object public save: FORGE_YDWE_ROOT / FORGE_WORLD_GAME_ROOT unset\n";
            return 77;
        }
        RealResourcesUsePublicMapEditSave(tool, game); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
