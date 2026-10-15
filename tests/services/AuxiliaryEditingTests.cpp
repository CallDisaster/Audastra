#include "War3MapCore/Services/AuxiliaryEditing.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "../MapDataFixtures.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
void Need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> bool Code(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
RegionRecord Region(std::string name, int id = 1) {
    RegionRecord r{}; r.name = std::move(name); r.creationNumber = id; r.left = -128; r.right = 256;
    r.bottom = -64; r.top = 512; r.colorBgra = {1, 2, 3, 99}; return r;
}
CameraRecord Camera(std::string name) {
    CameraRecord r{}; r.name = std::move(name); r.targetDistance = 1500; r.fieldOfView = 70;
    r.farClippingPlane = 5000; r.nearClippingPlane = 100; return r;
}
std::shared_ptr<const MapDocument> Source(bool empty = false) {
    std::vector<MapArchiveMember> members{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}};
    if (!empty) {
        RegionsDocument regions{5, {Region("First")}, {}};
        CamerasDocument cameras{0, {Camera("First")}, {}};
        members.push_back({"war3map.w3r", *regions.Serialize().value});
        members.push_back({"war3map.w3c", *cameras.Serialize().value});
    }
    return std::make_shared<const MapDocument>(MapDocument::FromMembers(members));
}
AuxiliaryEditRequest Request(const MapDocument& source, MapDataKind kind, AuxiliaryEditAction action, std::size_t row = 0) {
    AuxiliaryEditRequest request; request.kind = kind; request.action = action;
    const auto identity = AuxiliaryEditing::MemberIdentity(source, kind);
    Need(identity.Succeeded(), "fixture must have member identity"); request.expectedMemberSha256 = *identity.value;
    if (action != AuxiliaryEditAction::Create) {
        request.sourceIndex = row;
        const auto key = MapFieldRowKey(source, kind, row); Need(key.Succeeded(), "fixture row key");
        request.expectedRowKey = *key.value;
    }
    return request;
}

void DirectoryAliasesKeepSourceGuards() {
    struct Scratch {
        std::filesystem::path root = std::filesystem::temp_directory_path() / ("audastra-aux-alias-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Scratch() { std::error_code error; std::filesystem::remove_all(root, error); }
    } scratch;
    const auto write = [](const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Need(static_cast<bool>(stream), "write owned directory alias fixture");
    };
    const auto read = [](const std::filesystem::path& root) {
        const auto directory = DirectoryWorkspace::Open(root); Need(directory.Succeeded(), "snapshot directory alias fixture");
        auto document = MapDocument::FromDirectory(*directory.value); Need(document.Succeeded(), "parse directory alias fixture");
        return std::make_shared<const MapDocument>(std::move(*document.value));
    };
    Trigger::TriggerDocument triggers; AuxiliaryReferenceContext refs{&triggers, true, {}};
    for (const auto* prefix : {"", "map/", "MaP/"}) {
        const auto root = scratch.root / (std::string(prefix).empty() ? "native" : std::string(prefix) == "map/" ? "map-lower" : "map-case");
        write(root / (std::string(prefix) + "war3map.w3i"), MapDataFixtures::FullMapSettingsV25().bytes);
        auto source = read(root); MapDataEditSession missing(*source);
        auto create = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Create); create.record = Region("Created");
        auto plan = AuxiliaryEditing::Prepare(source, create, refs);
        Need(plan.Succeeded() && AuxiliaryEditing::Apply(missing, *plan.value, refs).Succeeded(),
            "root and map directory settings names permit missing region creation");
        Need(missing.Undo() && !missing.Snapshot().Regions() && missing.Redo()
            && missing.Snapshot().Regions()->entries[0].name == "Created", "directory alias creation preserves undo and redo");

        const auto regions = RegionsDocument{5, {Region("First")}, {}}.Serialize();
        const auto cameras = CamerasDocument{0, {Camera("First")}, {}}.Serialize();
        Need(regions.Succeeded() && cameras.Succeeded(), "serialize directory alias tables");
        write(root / (std::string(prefix) + "war3map.w3r"), *regions.value);
        write(root / (std::string(prefix) + "war3map.w3c"), *cameras.value);
        source = read(root); MapDataEditSession session(*source);
        auto update = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Update);
        auto region = source->Regions()->entries[0]; region.left -= 1; update.record = region;
        plan = AuxiliaryEditing::Prepare(source, update, refs); Need(plan.Succeeded(), "prepare directory alias update");
        Need(AuxiliaryEditing::Apply(session, *plan.value, refs).Succeeded() && session.Undo() && session.Redo(),
            "existing root and map directory members permit exact-byte update and recovery");
        MapDataEditSession changed(*source); auto camera = *source->Cameras(); camera.entries[0].roll += 1;
        Need(changed.EditCameras(camera).Succeeded() && Code(AuxiliaryEditing::Apply(changed, *plan.value, refs), "AUX_PLAN_STALE"),
            "normalizing directory names never ignores changed unrelated member bytes");
        if (std::string(prefix).empty()) continue;
        write(root / "war3map.w3r", *regions.value);
        auto ambiguous = read(root);
        Need(Code(AuxiliaryEditing::MemberIdentity(*ambiguous, MapDataKind::Regions), "AUX_SOURCE_UNSAFE"),
            "simultaneous root and map aliases remain ambiguous even with identical bytes");
        MapDataEditSession ambiguousSession(*ambiguous);
        Need(Code(AuxiliaryEditing::Apply(ambiguousSession, *plan.value, refs), "AUX_PLAN_STALE") && !ambiguousSession.IsDirty(),
            "different source state and count cannot borrow a prepared alias plan");
    }
}

void CompleteCrudAndRecovery() {
    auto source = Source(true); Trigger::TriggerDocument triggers;
    AuxiliaryReferenceContext refs{&triggers, true, {}};
    MapDataEditSession session(*source);
    auto create = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Create);
    create.record = Region("New Region");
    auto plan = AuxiliaryEditing::Prepare(source, create, refs);
    Need(plan.Succeeded() && AuxiliaryEditing::Apply(session, *plan.value, refs).Succeeded(), "missing classic region member creation");
    const auto first = session.LastEdit();
    Need(session.Snapshot().Regions()->entries.size() == 1 && session.Revision() == 1, "one region command");
    Need(session.Undo() && !session.IsDirty() && !session.Snapshot().Regions(), "undo restores missing presence");
    Need(session.Redo() && session.Snapshot().Regions(), "redo restores member");
    auto before = std::make_shared<const MapDocument>(session.Snapshot());
    auto update = Request(*before, MapDataKind::Regions, AuxiliaryEditAction::Update);
    auto record = before->Regions()->entries[0]; record.name = "Renamed";
    record.left = -300; record.weatherId = {std::byte{'R'}, std::byte{'a'}, std::byte{'i'}, std::byte{'n'}};
    record.colorBgra = {4, 5, 6, 99}; update.record = record;
    plan = AuxiliaryEditing::Prepare(before, update, refs);
    Need(plan.Succeeded() && AuxiliaryEditing::Apply(session, *plan.value, refs).Succeeded(), "rename and full region attributes");
    Need(session.Snapshot().Regions()->entries[0].colorBgra == record.colorBgra, "BGRA including reserved byte persisted");
    MapDataEditSession reopened(*source);
    for (const auto& member : session.Replacements()) Need(reopened.SetSerialized(MapDataKind::Regions, member.data).Succeeded(), "project overlay reopen");
    Need(reopened.Snapshot().Regions()->entries[0].weatherId == record.weatherId, "reopened weather retained");
    before = std::make_shared<const MapDocument>(session.Snapshot());
    auto remove = Request(*before, MapDataKind::Regions, AuxiliaryEditAction::Delete);
    plan = AuxiliaryEditing::Prepare(before, remove, refs);
    Need(plan.Succeeded() && AuxiliaryEditing::Apply(session, *plan.value, refs).Succeeded(), "unreferenced region deletion");
    Need(session.Snapshot().Regions()->entries.empty(), "deletion keeps explicit empty W3R");
    Need(session.Undo() && session.Snapshot().Regions()->entries[0].name == "Renamed", "delete undo restores full record");
    Need(!session.Replay(first, true).Succeeded(), "old creation receipt cannot overwrite a changed table");

    before = std::make_shared<const MapDocument>(session.Snapshot());
    auto camera = Request(*before, MapDataKind::Cameras, AuxiliaryEditAction::Create); camera.record = Camera("View");
    plan = AuxiliaryEditing::Prepare(before, camera, refs);
    Need(plan.Succeeded() && AuxiliaryEditing::Apply(session, *plan.value, refs).Succeeded(), "missing classic camera creation");
    before = std::make_shared<const MapDocument>(session.Snapshot());
    auto cameraUpdate = Request(*before, MapDataKind::Cameras, AuxiliaryEditAction::Update);
    auto changed = before->Cameras()->entries[0]; changed.name = "View Two"; changed.rotation = 60; changed.angleOfAttack = 290;
    changed.roll = 25; changed.fieldOfView = 80; changed.farClippingPlane = 6000; cameraUpdate.record = changed;
    plan = AuxiliaryEditing::Prepare(before, cameraUpdate, refs);
    Need(plan.Succeeded() && AuxiliaryEditing::Apply(session, *plan.value, refs).Succeeded(), "camera complete editable fields");
    const auto roundtrip = ParseCameras(*session.Snapshot().Cameras()->Serialize().value);
    Need(roundtrip.Succeeded() && roundtrip.value->entries[0].roll == 25 && roundtrip.value->entries[0].nearClippingPlane == 100,
        "camera native roundtrip retains unknown slot");
    before = std::make_shared<const MapDocument>(session.Snapshot());
    plan = AuxiliaryEditing::Prepare(before, Request(*before, MapDataKind::Cameras, AuxiliaryEditAction::Delete), refs);
    Need(plan.Succeeded() && AuxiliaryEditing::Apply(session, *plan.value, refs).Succeeded() && session.Snapshot().Cameras()->entries.empty(), "camera delete");
}

void IdentityAndUnknownSources() {
    auto source = Source(); Trigger::TriggerDocument triggers; AuxiliaryReferenceContext refs{&triggers, true, {}};
    MapDataEditSession session(*source);
    auto request = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Update);
    auto record = source->Regions()->entries[0]; record.left -= 1; request.record = record;
    const auto plan = AuxiliaryEditing::Prepare(source, request, refs); Need(plan.Succeeded(), "prepare update");
    auto other = *source->Regions(); other.entries[0].top += 1;
    Need(session.EditRegions(other).Succeeded() && Code(AuxiliaryEditing::Apply(session, *plan.value, refs), "AUX_PLAN_STALE"), "stale plan rejected");
    request.expectedMemberSha256 = "missing";
    Need(Code(AuxiliaryEditing::Prepare(source, request, refs), "AUX_MEMBER_STALE"), "presence mismatch rejected");
    request = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Update); request.expectedRowKey += "old"; request.record = record;
    Need(Code(AuxiliaryEditing::Prepare(source, request, refs), "AUX_ROW_STALE"), "row mismatch rejected");
    auto empty = std::make_shared<const MapDocument>(MapDocument::FromMembers({}));
    request = Request(*empty, MapDataKind::Cameras, AuxiliaryEditAction::Create); request.record = Camera("New");
    Need(Code(AuxiliaryEditing::Prepare(empty, request, refs), "AUX_CREATE_LAYOUT_UNKNOWN"), "missing W3I is not classic layout proof");
    const std::vector<MapArchiveMember> broken{{"war3map.w3r", {std::byte{5}}}};
    const auto bad = MapDocument::FromMembers(broken);
    Need(Code(AuxiliaryEditing::MemberIdentity(bad, MapDataKind::Regions), "AUX_SOURCE_UNSAFE"), "broken member not empty");
}

void ReferencesAndCollisions() {
    auto source = Source(); Trigger::TriggerDocument triggers; AuxiliaryReferenceContext refs{&triggers, true, {}};
    auto add = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Create); add.record = Region("First ", 2);
    Need(Code(AuxiliaryEditing::Prepare(source, add, refs), "AUX_SYMBOL_DUPLICATE"), "converted symbols conflict");
    auto remove = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Delete);
    Trigger::ArgumentNode arg; arg.type = 1; arg.value.text = "gg_rct_First"; triggers.argumentNodes.push_back(arg);
    Need(Code(AuxiliaryEditing::Prepare(source, remove, refs), "AUX_REFERENCED"), "even orphan/deep parameter reference is retained");
    triggers.argumentNodes.clear(); triggers.wct.customCode.text = "// external dispatch remains opaque";
    Need(Code(AuxiliaryEditing::Prepare(source, remove, refs), "AUX_SCRIPT_REFERENCES_UNKNOWN"), "unknown custom code cannot prove no references");
    auto update = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Update);
    auto record = source->Regions()->entries[0]; record.top += 1; update.record = record;
    Need(AuxiliaryEditing::Prepare(source, update, refs).Succeeded(), "opaque code does not block coordinate editing");
    triggers.wct.customCode.text.clear(); refs.completeTriggerSource = false;
    Need(Code(AuxiliaryEditing::Prepare(source, remove, refs), "AUX_REFERENCE_COVERAGE"), "unloaded trigger source cannot prove absence");
    refs.completeTriggerSource = true;
    auto units = ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()); UnitPlacement waygate;
    waygate.rawcode = "hfoo"; waygate.scale = {1,1,1}; waygate.creationId = 3; waygate.waygateRegionId = 1;
    units.value->units.push_back(waygate);
    auto projected = ProjectMapData(*source, std::vector<MapArchiveMember>{{"war3mapUnits.doo", *units.value->Serialize().value}});
    auto linked = std::make_shared<const MapDocument>(std::move(*projected.value));
    remove = Request(*linked, MapDataKind::Regions, AuxiliaryEditAction::Delete);
    Need(Code(AuxiliaryEditing::Prepare(linked, remove, refs), "REGION_WAYGATE_REFERENCE"), "real waygate references protect deletion");
}

void InvalidUpdatesAndAtomicPublish() {
    auto source = Source(); Trigger::TriggerDocument triggers; AuxiliaryReferenceContext refs{&triggers, true, {}};
    auto request = Request(*source, MapDataKind::Cameras, AuxiliaryEditAction::Update);
    auto camera = source->Cameras()->entries[0]; camera.roll = 12; request.record = camera;
    auto plan = AuxiliaryEditing::Prepare(source, request, refs); Need(plan.Succeeded(), "valid camera plan");
    MapDataEditSession session(*source);
    auto rejected = AuxiliaryEditing::Apply(session, *plan.value, refs, [](const auto&, const auto&, const auto&) {
        return Status::Failure("Test", "DECLINED", "declined");
    });
    Need(Code(rejected, "DECLINED") && session.Revision() == 0 && !session.IsDirty(), "prepublish refusal does not publish partial bytes");
    camera.nearClippingPlane = 150; request.record = camera;
    Need(Code(AuxiliaryEditing::Prepare(source, request, refs), "CAMERA_CLASSIC_SLOT_READ_ONLY"), "unknown slot cannot be edited");
    camera.nearClippingPlane = 100; camera.fieldOfView = 180; request.record = camera;
    Need(Code(AuxiliaryEditing::Prepare(source, request, refs), "CAMERA_RANGE_INVALID"), "invalid FOV rejected");
    request = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Update);
    auto region = source->Regions()->entries[0]; region.ambientSound = "gg_snd_Missing"; request.record = region;
    Need(Code(AuxiliaryEditing::Prepare(source, request, refs), "REGION_SOUND_UNAVAILABLE"), "missing sound table cannot be guessed");
    region.ambientSound.clear(); region.colorBgra[3] ^= 1; request.record = region;
    Need(Code(AuxiliaryEditing::Prepare(source, request, refs), "REGION_COLOR_RESERVED"), "reserved color byte cannot be changed");
    region.colorBgra[3] ^= 1; region.weatherId = {std::byte{'R'}, std::byte{'\''}, std::byte{'n'}, std::byte{'!'}}; request.record = region;
    Need(Code(AuxiliaryEditing::Prepare(source, request, refs), "REGION_WEATHER_INVALID"), "weather cannot inject rawcode delimiters");
}

void ReferenceChecksAtPublishAndUnknownPreservation() {
    auto source = Source(); Trigger::TriggerDocument triggers; AuxiliaryReferenceContext refs{&triggers, true, {}};
    MapDataEditSession session(*source);
    const auto remove = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Delete);
    const auto plan = AuxiliaryEditing::Prepare(source, remove, refs);
    Need(plan.Succeeded(), "prepare unreferenced deletion");
    Trigger::ArgumentNode argument; argument.value.text = "gg_rct_First"; triggers.argumentNodes.push_back(argument);
    Need(Code(AuxiliaryEditing::Apply(session, *plan.value, refs), "AUX_REFERENCED") && session.Revision() == 0,
        "reference added after prepare must prevent publication");
    triggers.argumentNodes.clear(); refs.opaqueScriptSources.push_back("scripts/new.lua");
    Need(Code(AuxiliaryEditing::Apply(session, *plan.value, refs), "AUX_SCRIPT_REFERENCES_UNKNOWN") && !session.IsDirty(),
        "new external script invalidates prior reference coverage");
    refs.opaqueScriptSources.clear();
    auto create = Request(*source, MapDataKind::Regions, AuxiliaryEditAction::Create); create.record = Region("New", 2);
    const auto createPlan = AuxiliaryEditing::Prepare(source, create, refs); Need(createPlan.Succeeded(), "prepare new symbol");
    argument.value.text = "set gg_rct_New = Rect(0., 0., 1., 1.)"; triggers.argumentNodes.push_back(argument);
    Trigger::EcaNode custom; custom.name.text = "CustomScriptCode"; custom.arguments = {0}; triggers.ecaNodes.push_back(custom);
    Need(Code(AuxiliaryEditing::Apply(session, *createPlan.value, refs), "AUX_SCRIPT_SYMBOL_CONFLICT") && !session.IsDirty(),
        "custom-script action collision is checked at publication");
    custom.arguments = {99}; triggers.ecaNodes[0] = custom;
    Need(Code(AuxiliaryEditing::Prepare(source, create, refs), "AUX_REFERENCE_COVERAGE"), "broken custom argument coverage is not empty");
    triggers.ecaNodes.clear(); triggers.argumentNodes.clear();

    auto regions = *source->Regions(); regions.entries[0].ambientSound = "unresolved original sound";
    regions.entries[0].weatherId = {std::byte{0x01}, std::byte{0}, std::byte{0}, std::byte{0}};
    auto cameras = *source->Cameras(); cameras.entries[0].nearClippingPlane = 4e9f;
    SoundsDocument sounds; SoundRecord sound{}; sound.name = "Ambient"; sound.flags = 0x80000000u;
    sounds.entries.push_back(sound);
    const auto soundBytes = sounds.Serialize(); Need(soundBytes.Succeeded(), "unknown sound flags can be preserved");
    auto projected = ProjectMapData(*source, std::vector<MapArchiveMember>{{"war3map.w3r", *regions.Serialize().value},
        {"war3map.w3c", *cameras.Serialize().value}, {"war3map.w3s", *soundBytes.value}});
    Need(projected.Succeeded(), "source with unknown preserved fields");
    auto unknown = std::make_shared<const MapDocument>(std::move(*projected.value)); MapDataEditSession unknownSession(*unknown);
    auto update = Request(*unknown, MapDataKind::Regions, AuxiliaryEditAction::Update);
    auto region = regions.entries[0]; region.top += 1; update.record = region;
    const auto updatePlan = AuxiliaryEditing::Prepare(unknown, update, refs);
    Need(updatePlan.Succeeded() && AuxiliaryEditing::Apply(unknownSession, *updatePlan.value, refs).Succeeded(),
        "unrelated edits preserve unknown sound and weather without interpreting them");
    Need(unknownSession.Snapshot().Regions()->entries[0].ambientSound == region.ambientSound
        && unknownSession.Snapshot().Sounds()->entries[0].flags == sound.flags, "unknown source text and flags retained");
    region.ambientSound = "gg_snd_Ambient"; update.record = region;
    Need(Code(AuxiliaryEditing::Prepare(unknown, update, refs), "REGION_SOUND_UNRESOLVED"), "new binding cannot assume unknown sound flags");
    auto cameraUpdate = Request(*unknown, MapDataKind::Cameras, AuxiliaryEditAction::Update);
    auto camera = cameras.entries[0]; camera.roll += 5; cameraUpdate.record = camera;
    Need(AuxiliaryEditing::Prepare(unknown, cameraUpdate, refs).Succeeded(), "unknown tenth slot is not assigned an unproven coordinate range");
}
} // namespace
int main() {
    try {
        CompleteCrudAndRecovery(); IdentityAndUnknownSources(); ReferencesAndCollisions(); InvalidUpdatesAndAtomicPublish();
        ReferenceChecksAtPublishAndUnknownPreservation();
        DirectoryAliasesKeepSourceGuards();
        std::cout << "AuxiliaryEditing: 6 scenario groups passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
