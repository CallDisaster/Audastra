#include "War3MapCore/Services/AuxiliaryTransaction.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using namespace War3Map::MapData;
void Need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> bool Code(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size())); Need(static_cast<bool>(out), "write owned fixture");
}
struct Scratch {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("audastra-aux-seal-" + std::to_string(GetCurrentProcessId())
        + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Scratch() { std::error_code error; std::filesystem::remove_all(root, error); }
};
RegionRecord Region(std::string name, int id) {
    RegionRecord row{}; row.name = std::move(name); row.creationNumber = id; row.left = -128; row.bottom = -64;
    row.right = 256; row.top = 512; row.colorBgra = {1, 2, 3, 97}; return row;
}
CameraRecord Camera(std::string name) {
    CameraRecord row{}; row.name = std::move(name); row.targetDistance = 1500; row.fieldOfView = 70;
    row.farClippingPlane = 5000; row.nearClippingPlane = -0.0f; return row;
}
struct History {
    Scratch scratch;
    Trigger::UiCatalog catalog;
    Trigger::TriggerDocument triggers;
    AuxiliaryReferenceContext refs{&triggers, true, {}};
    MapDocument original = MapDocument::FromMembers(std::array{MapArchiveMember{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}});
    MapDataEditSession edits{original};
    std::vector<AuxiliaryEditRequest> commands;
    History() {
        const auto ui = scratch.root / "ui";
        Write(ui / "config", "base\n"); Write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n");
        for (const auto* file : {"action.txt", "call.txt", "condition.txt", "event.txt"}) Write(ui / "base" / file, "");
        auto loaded = Trigger::UiCatalog::Load(ui); Need(loaded.Succeeded(), "trusted history catalog"); catalog = std::move(*loaded.value);
        auto empty = Trigger::WtgWctCodec{}.CreateEmpty(catalog); Need(empty.Succeeded(), "trusted history pair"); triggers = std::move(*empty.value);
    }
    AuxiliaryEditRequest Request(MapDataKind kind, AuxiliaryEditAction action, std::size_t row = 0) {
        const auto current = edits.Snapshot();
        AuxiliaryEditRequest request; request.kind = kind; request.action = action;
        const auto sha = AuxiliaryEditing::MemberIdentity(current, kind); Need(sha.Succeeded(), "member identity"); request.expectedMemberSha256 = *sha.value;
        if (action != AuxiliaryEditAction::Create) {
            const auto key = MapFieldRowKey(current, kind, row); Need(key.Succeeded(), "row identity");
            request.sourceIndex = row; request.expectedRowKey = *key.value;
        }
        return request;
    }
    void Apply(AuxiliaryEditRequest request) {
        auto plan = AuxiliaryEditing::Prepare(std::make_shared<const MapDocument>(edits.Snapshot()), request, refs);
        Need(plan.Succeeded() && AuxiliaryEditing::Apply(edits, *plan.value, refs).Succeeded(), "apply shared command");
        commands.push_back(std::move(request));
    }
};
void ReplayHistoryAndRejectInjectedBytes() {
    History h;
    auto first = h.Request(MapDataKind::Regions, AuxiliaryEditAction::Create); first.record = Region("First", 10); h.Apply(first);
    auto second = h.Request(MapDataKind::Cameras, AuxiliaryEditAction::Create); second.record = Camera("First View"); h.Apply(second);
    auto update = h.Request(MapDataKind::Regions, AuxiliaryEditAction::Update);
    auto record = h.edits.Snapshot().Regions()->entries[0]; record.name = "Renamed"; record.top = 800; update.record = record; h.Apply(update);
    auto copy = h.Request(MapDataKind::Regions, AuxiliaryEditAction::Create); record.name = "Copy"; record.creationNumber = 11; copy.record = record; h.Apply(copy);
    h.Apply(h.Request(MapDataKind::Regions, AuxiliaryEditAction::Delete, 0));
    const auto sealed = AuxiliaryTransactionSnapshot::Freeze(h.original, h.edits.Snapshot(), h.commands, h.refs);
    Need(sealed.Succeeded() && (*sealed.value)->Members().size() == 2 && (*sealed.value)->HasChanges(), "mixed region/camera sequence seals");
    Need((*sealed.value)->VerifyAgainst(h.original, h.refs).Succeeded(), "same sequence replays");
    auto otherTriggers = h.triggers; otherTriggers.wct.customComment.text = "different source pair";
    const auto otherBytes = Trigger::WtgWctCodec{}.Serialize(otherTriggers, h.catalog); Need(otherBytes.Succeeded(), "different trusted pair bytes");
    const auto otherSource = Trigger::WtgWctCodec{}.Parse(otherBytes.value->wtg, otherBytes.value->wct, h.catalog);
    Need(otherSource.Succeeded() && Code((*sealed.value)->VerifyAgainst(h.original, {&*otherSource.value, true, {}}), "AUX_TRANSACTION_TRIGGER_SOURCE_STALE"), "another valid WTG/WCT source cannot borrow the seal");
    auto forged = h.edits.Snapshot(); MapDataEditSession extra(forged); auto cameras = *forged.Cameras(); cameras.entries[0].roll = 40;
    Need(extra.EditCameras(cameras).Succeeded(), "construct unrelated injected camera bytes");
    Need(Code(AuxiliaryTransactionSnapshot::Freeze(h.original, extra.Snapshot(), h.commands, h.refs), "AUX_TRANSACTION_REPLAY_MISMATCH"), "unrecorded bytes cannot seal");
    auto regions = *h.edits.Snapshot().Regions(); regions.entries[0].top += 4;
    const auto alteredSource = ProjectMapData(h.original, std::array{MapArchiveMember{"war3map.w3r", *regions.Serialize().value}});
    Need(alteredSource.Succeeded() && Code((*sealed.value)->VerifyAgainst(*alteredSource.value, h.refs), "AUX_TRANSACTION_SOURCE_STALE"), "changed source/presence refused");
    Trigger::ArgumentNode argument; argument.value.text = "gg_rct_Renamed"; h.triggers.argumentNodes.push_back(argument);
    Need(Code((*sealed.value)->VerifyAgainst(h.original, h.refs), "AUX_REFERENCED"), "replay rechecks a reference added before delete");
    h.triggers.argumentNodes.clear(); h.refs.completeTriggerSource = false;
    Need(Code((*sealed.value)->VerifyAgainst(h.original, h.refs), "AUX_REFERENCE_COVERAGE"), "unknown reference coverage cannot authorize creation");
    h.refs.completeTriggerSource = true;
    std::vector<AuxiliaryEditRequest> over(129, first);
    Need(Code(AuxiliaryTransactionSnapshot::Freeze(h.original, h.edits.Snapshot(), over, h.refs), "AUX_TRANSACTION_COMMAND_BUDGET"), "command budget enforced before replay");
}
void DirectorySealMissingAndAliasGate() {
    History h; auto create = h.Request(MapDataKind::Regions, AuxiliaryEditAction::Create); create.record = Region("Owned", 1); h.Apply(create);
    const auto frozen = AuxiliaryTransactionSnapshot::Freeze(h.original, h.edits.Snapshot(), h.commands, h.refs); Need(frozen.Succeeded(), "missing seal");
    const auto& bytes = h.original.Member(MapDataKind::Settings).sources.front().data;
    const auto sha = Sha256(bytes); Need(sha.Succeeded(), "settings hash");
    WorkspaceSnapshot source; source.files.push_back({std::filesystem::path("map/war3map.w3i"), bytes.size(), 0, *sha.value});
    const auto pair = Trigger::WtgWctCodec{}.Serialize(h.triggers, h.catalog); Need(pair.Succeeded(), "source pair for strict directory binding");
    source.files.push_back({std::filesystem::path("map/war3map.wtg"), pair.value->wtg.size(), 0, std::string(h.triggers.SourceWtgSha256())});
    source.files.push_back({std::filesystem::path("map/war3map.wct"), pair.value->wct.size(), 0, std::string(h.triggers.SourceWctSha256())});
    Need((*frozen.value)->Seal(source).Succeeded(), "missing W3R is placed beside native W3I");
    source.files.push_back({std::filesystem::path("map/war3mapUnits.doo"), 0, 0, std::string(64, 'c')});
    Need(Code((*frozen.value)->Seal(source), "AUX_TRANSACTION_SOURCE_STALE"), "an omitted real units source cannot bypass waygate coverage");
    source.files.pop_back();
    source.files.push_back({std::filesystem::path("war3map.w3r"), 0, 0, std::string(64, 'a')});
    Need(Code((*frozen.value)->Seal(source), "AUX_TRANSACTION_SOURCE_STALE"), "existing alias cannot be treated as missing");
    source.files.push_back({std::filesystem::path("map/war3map.w3r"), 0, 0, std::string(64, 'b')});
    Need(Code((*frozen.value)->Seal(source), "AUX_TRANSACTION_SOURCE_AMBIGUOUS"), "root/map collision refuses guessing");
    AuxiliaryReferenceContext refs; ObserveAuxiliaryScriptSource(refs, "map/scripts/war3map.j");
    ObserveAuxiliaryScriptSource(refs, "war3map.j"); ObserveAuxiliaryScriptSource(refs, "map/custom/Owned.LUA");
    Need(refs.opaqueScriptSources.size() == 1, "generated JASS excluded, external Lua stays opaque");
}
void SaveServiceRechecksSourceBeforeToolchain() {
    Scratch scratch; History h;
    auto create = h.Request(MapDataKind::Cameras, AuxiliaryEditAction::Create); create.record = Camera("View"); h.Apply(create);
    auto snapshot = AuxiliaryTransactionSnapshot::Freeze(h.original, h.edits.Snapshot(), h.commands, h.refs); Need(snapshot.Succeeded(), "save seal");
    const auto ui = scratch.root / "ui";
    Write(ui / "config", "base\n"); Write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n");
    for (const auto* file : {"action.txt", "call.txt", "condition.txt", "event.txt"}) Write(ui / "base" / file, "");
    const auto catalog = Trigger::UiCatalog::Load(ui); Need(catalog.Succeeded(), "save catalog");
    const auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); Need(empty.Succeeded(), "save empty T");
    const auto pair = Trigger::WtgWctCodec{}.Serialize(*empty.value, *catalog.value); Need(pair.Succeeded(), "save T pair");
    CamerasDocument conflict; conflict.entries.push_back(Camera("Unexpected"));
    const std::array members{MapArchiveMember{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes},
        MapArchiveMember{"war3map.wtg", pair.value->wtg}, MapArchiveMember{"war3map.wct", pair.value->wct},
        MapArchiveMember{"war3map.w3c", *conflict.Serialize().value}};
    const auto source = WriteVerifiedArchive(scratch.root / "source.w3x", members); Need(source.Succeeded(), "write owned MPQ source");
    MapEditSaveRequest request; request.sourceMap = source.value->path; request.expectedSourceSha256 = source.value->sha256;
    request.targetCandidate = scratch.root / "candidate.w3x"; request.toolRoot = request.uiRoot = ui; request.gameRoot = scratch.root;
    request.auxiliaryTransaction = *snapshot.value;
    const auto result = SaveMapEditCandidate(request);
    Need(Code(result, "AUX_TRANSACTION_SOURCE_STALE") && !std::filesystem::exists(request.targetCandidate), "save cannot overwrite a present member using a missing-source seal");
    request.fieldEdits.push_back({std::string(64, 'a'), {}});
    Need(Code(SaveMapEditCandidate(request), "SAVE_OPERATION_SCOPE_INVALID"), "auxiliary cannot mix raw field request");
}
void FinalSaveOutputRejectsTemplateReplacement() {
    History h;
    auto create = h.Request(MapDataKind::Regions, AuxiliaryEditAction::Create); create.record = Region("Edited", 10); h.Apply(create);
    create = h.Request(MapDataKind::Cameras, AuxiliaryEditAction::Create); create.record = Camera("Edited view"); h.Apply(create);
    const auto expected = h.edits.Replacements();
    Need(expected.size() == 2, "freeze the two actual native edits before template merge");
    Need(VerifyAuxiliarySaveOutput(expected, expected, {}).Succeeded(), "unchanged frozen native outputs accepted");
    auto other = h.edits.Snapshot();
    auto regions = *other.Regions(); regions.entries[0].top += 128;
    auto cameras = *other.Cameras(); cameras.entries[0].rotation += 45;
    const auto changedRegions = regions.Serialize(), changedCameras = cameras.Serialize();
    Need(changedRegions.Succeeded() && changedCameras.Succeeded(), "original alternate template members serialize");
    for (const auto& replacement : std::array{
        MapArchiveMember{"WAR3MAP.W3R", *changedRegions.value}, MapArchiveMember{"WAR3MAP.W3C", *changedCameras.value}}) {
        auto final = expected;
        std::vector<MemberReplacement> imports{replacement};
        Need(Script::MergeTemplateImportMembers(final, {}, imports).Succeeded(), "template import retains normal member capability");
        Need(Code(VerifyAuxiliarySaveOutput(expected, final, {}), "SAVE_AUXILIARY_OUTPUT_MISMATCH"), "template cannot replace either frozen S6 edit");
    }
    for (const auto& member : expected) {
        auto final = expected;
        auto duplicate = member;
        std::transform(duplicate.name.begin(), duplicate.name.end(), duplicate.name.begin(), [](char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; });
        std::vector<MemberReplacement> same{duplicate, {"Original/import.bin", {std::byte{42}}}};
        Need(Script::MergeTemplateImportMembers(final, {}, same).Succeeded()
            && VerifyAuxiliarySaveOutput(expected, final, {}).Succeeded(), "case aliases with same bytes and unrelated template imports remain allowed");
        const auto key = member.name;
        final = expected;
        final.erase(std::remove_if(final.begin(), final.end(), [&](const auto& entry) { return entry.name == key; }), final.end());
        Need(Code(VerifyAuxiliarySaveOutput(expected, final, {}), "SAVE_AUXILIARY_OUTPUT_MISMATCH"), "missing edited output rejected");
        final = expected;
        final.push_back(duplicate);
        Need(Code(VerifyAuxiliarySaveOutput(expected, final, {}), "SAVE_AUXILIARY_OUTPUT_MISMATCH"), "case alias duplicate rejected");
        const std::array removals{duplicate.name};
        Need(Code(VerifyAuxiliarySaveOutput(expected, expected, removals), "SAVE_AUXILIARY_OUTPUT_MISMATCH"), "case alias removal rejected");
        auto renamed = expected;
        for (auto& entry : renamed) if (entry.name == key) entry.name = "map/" + entry.name;
        Need(Code(VerifyAuxiliarySaveOutput(expected, renamed, {}), "SAVE_AUXILIARY_OUTPUT_MISMATCH"), "archive map directory is not a root member alias");
    }
    const auto region = std::find_if(expected.begin(), expected.end(), [](const auto& member) { return member.name == "war3map.w3r"; });
    Need(region != expected.end(), "original edited region is present in the frozen output");
    const std::array onlyRegion{*region};
    auto final = expected;
    std::vector<MemberReplacement> unedited{{"war3map.w3c", *changedCameras.value}, {"Original/unrelated.bin", {std::byte{7}}}};
    Need(Script::MergeTemplateImportMembers(final, {}, unedited).Succeeded()
        && VerifyAuxiliarySaveOutput(onlyRegion, final, {}).Succeeded(), "unedited camera and other template members are not prohibited");
    Need(VerifyAuxiliarySaveOutput({}, final, {}).Succeeded(), "no edited auxiliary member adds no import restriction");
    Need(h.edits.IsDirty() && h.edits.Snapshot().Regions()->entries[0].top == 512
        && h.edits.Snapshot().Cameras()->entries[0].rotation == 0, "template merge does not mutate the authored edit session");
}
}

int main() {
    try { ReplayHistoryAndRejectInjectedBytes(); DirectorySealMissingAndAliasGate(); SaveServiceRechecksSourceBeforeToolchain(); FinalSaveOutputRejectsTemplateReplacement(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Auxiliary transaction authored fixtures passed.\n"; return 0;
}
