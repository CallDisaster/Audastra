#include "TerrainWalkableBridgeRealTests.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iostream>
#include <span>
#include <stdexcept>

namespace War3Map::TerrainSaveTests {
namespace {
using namespace MapData;
using namespace Services;
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

void CheckBridgeNativeOverrides(const MapArchiveSession& source, const TerrainGridSnapshot& before,
    const std::filesystem::path& directory, const std::filesystem::path& game,
    const ObjectData::StandardObjectEntry& object, const ObjectData::ObjectFieldSchema& schema,
    const ObjectData::StandardObjectCatalog& catalog, const MapEditSaveRequest& baseRequest) {
    using namespace ObjectData;
    std::array<std::string, 2> customRawcodes;
    for (std::size_t category = 0; category < customRawcodes.size(); ++category) {
        for (const char suffix : std::string_view("0123456789")) {
            const auto candidate = std::string(category ? "ZCR" : "ZbR") + suffix;
            if (!catalog.Find(ObjectType::Destructable, candidate) && !catalog.Find(ObjectType::Doodad, candidate)) {
                customRawcodes[category] = candidate;
                break;
            }
        }
        Require(!customRawcodes[category].empty(), "actual custom bridge has a unique test rawcode for each category");
    }
    ObjectFieldContext context; context.rawcode = object.key.rawcode; context.baseCode = object.Field("code");
    const std::array<const char*, 3> names{"walkable", "pathtex", "fixedrot"};
    std::array<ObjectFieldMatch, names.size()> fields;
    for (std::size_t i = 0; i < names.size(); ++i) {
        fields[i] = schema.MatchSourceField(ObjectType::Destructable, names[i], context);
        const auto& match = fields[i];
        Require(match.definition && match.safeToEdit && match.sourceKey == names[i] && !match.sourceIndex
            && match.definition->repeat == 0 && match.definition->data == 0
            && match.definition->useSpecific.empty() && match.definition->notSpecific.empty()
            && Text::LowerAscii(match.definition->sourceTable) == "destructabledata",
            "actual bridge metadata has unique nonindexed DestructableData addresses");
    }
    Require(fields[0].definition->nativeValueType == NativeValueType::Int32
        && fields[1].definition->nativeValueType == NativeValueType::String
        && (fields[2].definition->nativeValueType == NativeValueType::Real
            || fields[2].definition->nativeValueType == NativeValueType::Unreal),
        "actual bridge metadata supplies the exact three native wire types");
    std::cout << "walkable_bridge_override_fields=" << fields[0].modification.fieldId << ','
        << fields[1].modification.fieldId << ',' << fields[2].modification.fieldId
        << " fixed_rot_wire=" << static_cast<int>(*fields[2].definition->nativeValueType) << '\n';
    for (const unsigned kind : {0u, 1u, 2u}) for (const bool explicitFields : {false, true}) {
        const bool custom = kind != 0, categoryC = kind == 2;
        const auto scenario = directory / (std::string(categoryC ? "custom-c-" : custom ? "custom-" : "original-")
            + (explicitFields ? "typed-fields" : "empty"));
        Require(std::filesystem::create_directory(scenario), "exclusive actual bridge original scenario");
        auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
        const ObjectKey key{ObjectType::Destructable, custom ? customRawcodes[categoryC ? 1 : 0] : object.key.rawcode};
        if (custom) Check(document.AddCustomObject(key.rawcode, object.key.rawcode));
        else Check(document.AddOriginalObject(key.rawcode));
        if (explicitFields) {
            Check(document.AddModification(key, fields[0].modification, NativeValueType::Int32, std::int32_t{1}));
            Check(document.AddModification(key, fields[1].modification, NativeValueType::String,
                std::string(object.Field("pathtex"))));
            Check(document.AddModification(key, fields[2].modification,
                *fields[2].definition->nativeValueType, 90.0f));
        }
        const auto nativeBytes = Take(NativeObjectCodec(ObjectType::Destructable).Serialize(document));
        auto placements = Take(ParseDoodadPlacements(Take(source.ReadMember("war3map.doo"))));
        Require(placements.doodads.size() == 1 && placements.doodads.front().rawcode == object.key.rawcode,
            "actual bridge native scenarios replace only their single synthetic placement identity");
        placements.doodads.front().rawcode = key.rawcode;
        if (categoryC) {
            // 覆盖非原点摆放的C类桥；两种网格的独立字节预期由Core小矩阵验证。
            placements.doodads.front().position[0] += 32;
            placements.doodads.front().position[1] += 32;
            Require(key.rawcode[1] == 'C' && !catalog.Find(ObjectType::Doodad, key.rawcode),
                "custom bridge category comes from the placed new ID with no doodad category to inherit");
        }
        const std::array<MemberReplacement, 2> replacement{{{"war3map.w3b", nativeBytes},
            {"war3map.doo", Take(placements.Serialize())}}};
        const auto seeded = Take(MapArchiveSession::Open(Take(source.SaveCandidate(scenario / "seeded.w3x", replacement)).path));
        const auto rules = Take(TerrainPathingSave::CaptureNative(seeded, before, before, game, kSaveBytes));
        const auto oldNative = Take(rules.GenerateNative(before, kSaveBytes));
        if (categoryC) {
            const DoodadPlacements empty;
            const std::array<MemberReplacement, 1> noBridge{{{"war3map.doo", Take(empty.Serialize())}}};
            const auto control = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "unbridged.w3x", noBridge)).path));
            const auto controlRules = Take(TerrainPathingSave::CaptureNative(control, before, before, game, kSaveBytes));
            Require(oldNative != Take(controlRules.GenerateNative(before, kSaveBytes)),
                "actual shifted C bridge suppresses nonzero terrain contribution");
            Require(Take(Sha256File(control.SourcePath())) == control.SourceSha256(),
                "actual C bridge control source remains unchanged");
        }
        auto wpm = Take(source.ReadMember("war3map.wpm"));
        Require(wpm.size() == oldNative.size() + 16, "actual bridge override WPM dimensions");
        for (std::size_t i = 0; i < oldNative.size(); ++i) wpm[i + 16] = (wpm[i + 16] & std::byte{0x11}) | oldNative[i];
        const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", wpm}}};
        const auto input = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", pathing)).path));
        auto request = baseRequest; request.sourceMap = input.SourcePath(); request.expectedSourceSha256 = input.SourceSha256();
        request.targetCandidate = scenario / "saved.w3x";
        const auto saved = Take(SaveMapEditCandidate(request));
        const auto readback = Take(MapArchiveSession::Open(saved.candidate));
        const auto after = *Take(ParseMapTerrain(Take(readback.ReadMember("war3map.w3e")))).Snapshot();
        const auto afterRules = Take(TerrainPathingSave::CaptureNative(readback, after, after, game, kSaveBytes));
        const auto newNative = Take(afterRules.GenerateNative(after, kSaveBytes));
        Require(newNative.size() == oldNative.size() && newNative != oldNative,
            "actual bridge Original save changes distant water pathing");
        for (std::size_t i = 0; i < newNative.size(); ++i) wpm[i + 16] = (wpm[i + 16] & std::byte{0x11}) | newNative[i];
        Require(saved.scriptPreserved && Take(readback.ReadMember("war3map.wpm")) == wpm,
            "actual bridge Original save reads back complete resource-derived WPM");
        for (const auto& member : input.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp") continue;
            Require(Take(readback.ReadMember(member.name)) == member.data,
                "actual bridge Original save preserves W3B and every unrelated member");
        }
        Check(afterRules.VerifyReadback(readback, kSaveBytes));
        Require(Take(Sha256File(input.SourcePath())) == input.SourceSha256(), "actual bridge Original source is unchanged");
        std::cout << "walkable_bridge_" << (custom ? "custom" : "original")
            << "_save=" << (explicitFields ? "typed-fields" : "empty") << " rawcode=" << key.rawcode
            << " parent=" << (custom ? object.key.rawcode : "-")
            << " placed_category_c=" << categoryC
            << " source_sha256=" << input.SourceSha256() << " candidate_sha256=" << saved.candidateSha256
            << " independent_wpm_oracle=false native_editor_verified=false game_verified=false\n";
    }
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(),
        "all actual bridge override scenarios preserve their shared source");
}
}

void CheckRealWalkableBridgeSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game, const ObjectData::StandardObjectCatalog& catalog,
    const ObjectData::ObjectFieldSchema& schema, const Game::GameDataSource& mounted) {
    const auto scenario = directory / "standard-walkable-bridge";
    Require(std::filesystem::create_directory(scenario), "exclusive actual walkable bridge scenario");
    std::string rawcode;
    for (const auto& object : catalog.Entries()) {
        if (object.key.type != ObjectData::ObjectType::Destructable || object.Field("walkable") != "1"
            || object.key.rawcode.size() != 4 || object.key.rawcode[1] == 'C'
            || catalog.Find(ObjectData::ObjectType::Doodad, object.key.rawcode)) continue;
        const auto path = std::string(object.Field("pathtex"));
        if (!Text::LowerAscii(path).ends_with(".tga")) continue;
        const auto rotationText = object.Field("fixedrot");
        if (rotationText.empty()) continue;
        float rotation = 0;
        const auto parsed = std::from_chars(rotationText.data(), rotationText.data() + rotationText.size(), rotation);
        if (parsed.ec != std::errc{} || parsed.ptr != rotationText.data() + rotationText.size()
            || !std::isfinite(rotation) || rotation < -360 || rotation > 360) continue;
        auto file = mounted.ReadFileBoundedWithSource(path, 512u * 1024u);
        if (!file.Succeeded() || !file.value) continue;
        const auto footprint = DecodeDoodadFootprintTga(file.value->bytes);
        if (!footprint.Succeeded() || !footprint.value || footprint.value->width > 32 || footprint.value->height > 32) continue;
        bool colored = false;
        for (std::size_t i = 0; i < footprint.value->pixels.size(); i += footprint.value->pixelBytes)
            colored = colored || footprint.value->pixels[i] != std::byte{}
                || footprint.value->pixels[i + 1] != std::byte{} || footprint.value->pixels[i + 2] != std::byte{};
        if (!colored) continue;
        Require(!original.HasMember(path), "actual bridge fixture must use the captured game-winning footprint");
        rawcode = object.key.rawcode;
        std::cout << "walkable_bridge_object=" << rawcode << " path=" << path << " fixed_rot=" << rotation
            << " winner=" << file.value->archiveName << " bytes=" << file.value->bytes.size()
            << " sha256=" << Take(Sha256(file.value->bytes)) << '\n';
        break;
    }
    Require(!rawcode.empty(), "actual game has an unambiguous standard live bridge and supported nonempty footprint");
    const auto dry = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(dry.header.width > 56 && dry.header.height > 56, "bridge regression map contains both edited areas");
    const DoodadPlacements empty;
    const auto waterRules = Take(TerrainPathingSave::CaptureWaterBrushRules(original, dry, dry, empty, game, true, kSaveBytes));
    Require(waterRules.WaterBrushRules() != nullptr, "actual source provides complete water brush rules");
    const TerrainStrokePoint point{dry.header.offset[0] + 32 * 128, dry.header.offset[1] + 32 * 128};
    // 原创脚本的两名单位在(-512,0)/(-512,512)；小水池避免出生点落入深水。
    // 这里只改善人工验收样例，不把可加载或离线WPM结果当作桥上/桥下运动证明。
    const TerrainWaterBrush brush{TerrainWaterBrushKind::Deep, TerrainBrushShape::Square, 2, 0, 42,
        Take(CaptureTerrainWaterStrokeAnchor(dry, point))};
    const auto before = Take(PrepareTerrainWaterStroke(dry, *waterRules.WaterBrushRules(), brush, std::span(&point, 1), empty)).terrain;
    Require(point.x == 0 && point.y == 0, "bridge fixture pool shares the scripted world origin");
    Require(!before.vertices[std::size_t{32} * before.header.width + 28].HasWater()
        && !before.vertices[std::size_t{36} * before.header.width + 28].HasWater(),
        "both scripted unit spawn requests remain on dry terrain; in-game spawn and movement are unverified");
    const auto beforeBytes = Take(SerializeMapTerrain(before));
    DoodadPlacement bridge;
    bridge.rawcode = rawcode; bridge.flags = 2; bridge.lifePercent = 100; bridge.creationId = 1;
    bridge.position = {static_cast<float>(point.x), static_cast<float>(point.y), 0};
    bridge.scale = {1, 1, 1};
    DoodadPlacements placements; placements.doodads.push_back(bridge);
    const auto doo = Take(placements.Serialize());
    const std::array<MemberReplacement, 2> members{{{"war3map.w3e", beforeBytes}, {"war3map.doo", doo}}};
    const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", members)).path));
    const auto rules = Take(TerrainPathingSave::CaptureNative(seeded, before, before, game, kSaveBytes));
    const auto oldNative = Take(rules.GenerateNative(before, kSaveBytes));
    // 只用于证明实际资源参与合成；独立字面算法预期由Core/Services小矩阵给出。
    const std::array<MemberReplacement, 1> noBridge{{{"war3map.doo", Take(empty.Serialize())}}};
    const auto control = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "unbridged-inputs.w3x", noBridge)).path));
    const auto controlRules = Take(TerrainPathingSave::CaptureNative(control, before, before, game, kSaveBytes));
    Require(oldNative != Take(controlRules.GenerateNative(before, kSaveBytes)), "actual live bridge suppresses nonzero terrain contribution");
    auto wpm = Take(seeded.ReadMember("war3map.wpm"));
    Require(wpm.size() == oldNative.size() + 16, "actual bridge source has complete WPM dimensions");
    for (std::size_t i = 0; i < oldNative.size(); ++i) wpm[i + 16] = (wpm[i + 16] & std::byte{0x11}) | oldNative[i];
    const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", wpm}}};
    const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", pathing)).path));
    MapEditSaveRequest request;
    request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "saved.w3x";
    request.toolRoot = tool; request.gameRoot = game;
    request.expectedSourceSha256 = source.SourceSha256(); request.expectedTerrainSha256 = Take(Sha256(beforeBytes));
    MapEditTerrainStroke stroke; stroke.pointSpace = MapEditPointSpace::TerrainVertex; stroke.points = {{48, 48}};
    TerrainWaterStrokeOptions options; options.kind = TerrainWaterBrushKind::Shallow;
    options.shape = TerrainBrushShape::Square; options.cliffTexture = 0; options.randomSeed = 43;
    stroke.waterBrush = options; request.terrainStrokes = {stroke};
    const auto saved = Take(SaveMapEditCandidate(request));
    const auto readback = Take(MapArchiveSession::Open(saved.candidate));
    const auto after = *Take(ParseMapTerrain(Take(readback.ReadMember("war3map.w3e")))).Snapshot();
    Require(!before.vertices[std::size_t{48} * before.header.width + 48].HasWater()
        && after.vertices[std::size_t{48} * after.header.width + 48].HasWater(),
        "public save creates distant water while retaining the live bridge");
    const auto newRules = Take(TerrainPathingSave::CaptureNative(readback, after, after, game, kSaveBytes));
    const auto newNative = Take(newRules.GenerateNative(after, kSaveBytes));
    Require(newNative.size() == oldNative.size() && newNative != oldNative, "bridge map save changes distant terrain pathing");
    for (std::size_t i = 0; i < newNative.size(); ++i) wpm[i + 16] = (wpm[i + 16] & std::byte{0x11}) | newNative[i];
    Require(saved.scriptPreserved && Take(readback.ReadMember("war3map.wpm")) == wpm,
        "actual bridge public save reads back the complete resource-derived WPM");
    for (const auto& member : source.Members()) {
        const auto name = Text::LowerAscii(member.name);
        if (name == "war3map.w3e" || name == "war3map.wpm" || name == "war3mapmap.blp") continue;
        Require(Take(readback.ReadMember(member.name)) == member.data, "actual bridge save preserves every unrelated member");
    }
    Check(newRules.VerifyReadback(readback, kSaveBytes));
    Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256(), "actual bridge source stays unchanged");
    const auto* standard = catalog.Find(ObjectData::ObjectType::Destructable, rawcode);
    Require(standard != nullptr, "selected actual bridge retains its standard catalog identity");
    CheckBridgeNativeOverrides(source, before, scenario, game, *standard, schema, catalog, request);
    placements.doodads.front().lifePercent = 0;
    const std::array<MemberReplacement, 1> dead{{{"war3map.doo", Take(placements.Serialize())}}};
    const auto deadSource = Take(MapArchiveSession::Open(Take(source.SaveCandidate(scenario / "dead-source.w3x", dead)).path));
    request.sourceMap = deadSource.SourcePath(); request.expectedSourceSha256 = deadSource.SourceSha256();
    request.targetCandidate = scenario / "dead-must-not-exist.w3x";
    const auto rejected = SaveMapEditCandidate(request);
    Require(!rejected.Succeeded() && !rejected.value && !std::filesystem::exists(request.targetCandidate)
        && std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_WALKABLE_DESTRUCTABLE_STATE";
        }),
        "unsupported dead bridge state must not produce a saved candidate");
    Require(Take(Sha256File(deadSource.SourcePath())) == deadSource.SourceSha256(), "refused bridge save preserves its source");
    std::cout << "walkable_bridge_public_save_passed source_sha256=" << source.SourceSha256()
        << " candidate_sha256=" << saved.candidateSha256 << " native_editor_verified=false game_verified=false\n";
}
}
