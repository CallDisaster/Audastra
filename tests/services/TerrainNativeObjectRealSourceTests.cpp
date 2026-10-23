#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapData/MapPlacements.h"
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
        for (const auto& object : catalog.Entries()) {
            if (object.key.type != ObjectType::Doodad || catalog.Find(ObjectType::Destructable, object.key.rawcode)) continue;
            const auto path = object.fields.find("pathtex");
            if (path == object.fields.end() || path->second.empty() || path->second == "_" || path->second == "none") continue;
            ObjectFieldContext context; context.rawcode = object.key.rawcode; context.baseCode = object.Field("code");
            const auto field = schema.MatchSourceField(ObjectType::Doodad, "pathtex", context);
            if (!field.definition || !field.safeToEdit || field.sourceKey != "pathtex" || field.sourceIndex
                || field.definition->nativeValueType != NativeValueType::String || field.definition->repeat != 0
                || field.definition->data != 0 || Text::LowerAscii(field.definition->sourceTable) != "doodaddata"
                || Text::LowerAscii(field.definition->sourceField) != "pathtex") continue;
            originalDoodadRawcode = object.key.rawcode; originalDoodadPath = path->second; doodadPathField = field.modification; break;
        }
        Require(!originalDoodadRawcode.empty(), "actual metadata contains a standard doodad whose nonempty pathTex can be explicitly overridden");
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
            if (!decoded.Succeeded() || !decoded.value || !decoded.value->attributes || decoded.value->side > 4) continue;
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
        // 五场景复用一次原创地图创建、脚本编译和实际表捕获；只修改自己的摆放/覆盖。
        for (const unsigned index : {0u, 1u, 2u, 3u, 4u}) {
            const bool withDoodad = index != 0, withOriginal = index == 2, withUniform = index == 3, withPattern = index == 4;
            const auto& selectedDoodad = withPattern ? patternedRawcode : withUniform ? uniformRawcode : withOriginal ? originalDoodadRawcode : doodadRawcode;
            const auto scenario = directory.path / (withPattern ? "patterned-doodad" : withUniform ? "uniform-doodad" : withOriginal ? "original-doodad" : withDoodad ? "mixed-doodad" : "destructable");
            Require(std::filesystem::create_directory(scenario), "exclusive real-source scenario directory");
            auto placements = doodads;
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
            if (withOriginal) seed.push_back({"war3map.w3d", originalDoodadW3d});
            auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", seed)).path));
            const auto rules = Take(TerrainPathingSave::CaptureNative(seeded, before, after, game, kSaveBytes));
            const auto oldNative = Take(rules.GenerateNative(before, kSaveBytes));
            const auto newNative = Take(rules.GenerateNative(after, kSaveBytes));
            Require(oldNative != newNative, "each public save scenario crosses a real water pathing threshold");
            std::cout << "selected_object=" << rawcode << " metadata_field=" << walkable.fieldId
                << " doodad=" << (withDoodad ? selectedDoodad : "absent") << " original=" << withOriginal
                << " standard_pathTex=" << (withPattern ? patternedPath : withUniform ? uniformPath : withOriginal ? originalDoodadPath : withDoodad ? doodadPath : "not-applicable")
                << " pathTex_field=" << (withOriginal ? doodadPathField.fieldId : "not-applicable")
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
            Require(readback.HasMember("war3map.w3d") == (withOriginal || hasW3d)
                && (!(withOriginal || hasW3d) || Take(readback.ReadMember("war3map.w3d")) == (withOriginal ? originalDoodadW3d : sourceW3d)),
                "public save preserves W3D content or its actual absence");
            Require(Take(Sha256File(source.SourcePath())) == sourceSha, "public save leaves the source map untouched");
            Check(rules.VerifyReadback(readback, kSaveBytes));
            std::cout << "public_map_edit_save_passed source_sha256=" << sourceSha << " candidate_sha256=" << saved.candidateSha256
                << " native_editor_verified=false game_verified=false\n";
            if (withOriginal) {
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
                    return diagnostic.code == "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED";
                }), "public save must reject actual nonempty pathTex instead of falling back to an empty override");
                Require(!std::filesystem::exists(blocked.targetCandidate), "refused public save must not produce a candidate");
                Require(Take(Sha256File(overridden.SourcePath())) == overridden.SourceSha256()
                    && Take(Sha256File(source.SourcePath())) == sourceSha, "refused public save preserves both source copies");
                std::cout << "nonempty_doodad_override_refused source_sha256=" << overridden.SourceSha256() << '\n';
            }
        }
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
