#include "McpAuxiliaryTools.h"
#include "McpWorkspace.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <QJsonArray>
#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using namespace War3Map::Mcp;
void Need(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
QString Code(const QJsonObject& result) {
    const auto rows = result.value("diagnostics").toArray(); return rows.isEmpty() ? QString{} : rows[0].toObject().value("code").toString();
}
QJsonObject RegionFields(const char* name, int id) {
    return {{"name", name}, {"creation_number", id}, {"left", -128}, {"bottom", -64}, {"right", 256}, {"top", 512},
        {"weather_id", ""}, {"ambient_sound", ""}, {"color_bgr", QJsonArray{1, 2, 3}}};
}
QJsonObject CameraFields(const char* name) {
    return {{"name", name}, {"target_x", 10}, {"target_y", 20}, {"z_offset", 30}, {"rotation", 90}, {"angle_of_attack", 300},
        {"target_distance", 1500}, {"roll", 0}, {"field_of_view", 70}, {"far_clipping_plane", 5000}};
}
struct HostFixture {
    Trigger::TriggerDocument triggers;
    std::shared_ptr<const MapDocument> original = std::make_shared<const MapDocument>(MapDocument::FromMembers(
        std::array{MapArchiveMember{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}}));
    MapDataEditSession edits{*original};
    AuxiliaryToolHost host;
    std::uint64_t epoch = 0;
    int publishes = 0;
    bool reject = false;
    HostFixture() {
        host.identity = [&] { return QJsonObject{{"project_identity", "owned-map"}, {"revision", QString::number(epoch)}}; };
        host.context = [&] {
            MapDataToolContext context; context.original = original; context.document = std::make_shared<const MapDocument>(edits.Snapshot());
            context.binding = {"owned-map", 1, epoch, std::string(64, 'a')}; return Result<MapDataToolContext>::Success(std::move(context));
        };
        host.references = [&] { return AuxiliaryReferenceContext{&triggers, true, {}}; };
        host.apply = [&](const PreparedAuxiliaryEdit& plan, const MapDataPrepublishValidator& validate) -> Result<MapDataEditSession::EditReceipt> {
            const auto applied = AuxiliaryEditing::Apply(edits, plan, host.references(), [&](const auto& before, const auto& after, const auto& receipt) {
                const auto checked = validate(before, after, receipt, {}); if (!checked.Succeeded()) return checked;
                return reject ? Status::Failure("Test", "HOST_DENIED", "发布前拒绝") : Status::Ok();
            });
            if (!applied.Succeeded()) return {{}, applied.diagnostics};
            ++publishes; ++epoch; return Result<MapDataEditSession::EditReceipt>::Success(edits.LastEdit());
        };
    }
    QJsonObject Arguments(MapDataKind kind, bool existing = false, std::size_t row = 0) {
        const auto identity = host.identity(); const auto document = edits.Snapshot();
        const auto sha = AuxiliaryEditing::MemberIdentity(document, kind); Need(sha.Succeeded(), "actual member identity");
        QJsonObject args{{"project_identity", identity.value("project_identity")}, {"expected_revision", identity.value("revision")},
            {"kind", kind == MapDataKind::Regions ? "regions" : "cameras"}, {"expected_member_sha256", QString::fromStdString(*sha.value)}};
        if (existing) {
            const auto key = MapFieldRowKey(document, kind, row); Need(key.Succeeded(), "actual row identity");
            args.insert("source", QJsonObject{{"source_index", QString::number(static_cast<qulonglong>(row))}, {"row_key", QString::fromStdString(*key.value)}});
        }
        return args;
    }
};
void FacadeCrudDryRunAndOriginalUndo() {
    HostFixture f;
    auto create = f.Arguments(MapDataKind::Regions); create.insert("fields", RegionFields("First", 1)); create.insert("dry_run", true);
    const auto preview = InvokeAuxiliaryTool("auxiliary_create", create, f.host);
    Need(preview.value("ok").toBool() && !preview.value("applied").toBool() && f.publishes == 0 && !f.edits.IsDirty(), "dry run produces candidate identity without command");
    create.remove("dry_run"); const auto added = InvokeAuxiliaryTool("auxiliary_create", create, f.host);
    Need(added.value("ok").toBool() && added.value("applied").toBool() && added.contains("result_identity") && f.publishes == 1, "missing region creation uses shared original undo");
    auto update = f.Arguments(MapDataKind::Regions, true); update.insert("fields", QJsonObject{{"name", "Renamed"}, {"left", -300}, {"color_bgr", QJsonArray{9, 8, 7}}});
    Need(InvokeAuxiliaryTool("auxiliary_update", update, f.host).value("ok").toBool(), "rename and full known fields");
    Need(Code(InvokeAuxiliaryTool("auxiliary_update", update, f.host)) == "AUX_REVISION_CONFLICT", "stale revision rejected");
    auto copy = f.Arguments(MapDataKind::Regions, true); copy.insert("fields", QJsonObject{{"name", "Second"}, {"creation_number", 2}});
    Need(InvokeAuxiliaryTool("auxiliary_copy", copy, f.host).value("applied").toBool() && f.edits.Snapshot().Regions()->entries.size() == 2, "copy preserves original record with explicit new identity");
    const auto staleDelete = f.Arguments(MapDataKind::Regions, true);
    Trigger::ArgumentNode arg; arg.value.text = "gg_rct_Renamed"; f.triggers.argumentNodes.push_back(arg);
    Need(Code(InvokeAuxiliaryTool("auxiliary_delete", staleDelete, f.host)) == "AUX_REFERENCED", "actual T reference blocks deletion");
    f.triggers.argumentNodes.clear();
    Need(InvokeAuxiliaryTool("auxiliary_delete", staleDelete, f.host).value("applied").toBool() && f.edits.Snapshot().Regions()->entries.size() == 1, "delete keeps remaining record");
    Need(f.edits.Undo() && f.edits.Snapshot().Regions()->entries.size() == 2, "original undo restores deleted full row"); ++f.epoch;
    Need(f.edits.Redo() && f.edits.Snapshot().Regions()->entries.size() == 1, "original redo restores native bytes"); ++f.epoch;
    auto noOp = f.Arguments(MapDataKind::Regions, true); noOp.insert("fields", QJsonObject{{"name", "Second"}});
    const auto count = f.publishes; const auto revision = f.edits.Revision();
    Need(!InvokeAuxiliaryTool("auxiliary_update", noOp, f.host).value("applied").toBool() && f.publishes == count && f.edits.Revision() == revision, "no-op consumes no undo command");
    auto camera = f.Arguments(MapDataKind::Cameras); camera.insert("fields", CameraFields("View"));
    Need(InvokeAuxiliaryTool("auxiliary_create", camera, f.host).value("applied").toBool(), "missing camera create");
    auto changeCamera = f.Arguments(MapDataKind::Cameras, true); changeCamera.insert("fields", QJsonObject{{"roll", 45}, {"target_x", 200}});
    Need(InvokeAuxiliaryTool("auxiliary_update", changeCamera, f.host).value("applied").toBool(), "camera numeric update");
    Need(std::bit_cast<std::uint32_t>(f.edits.Snapshot().Cameras()->entries[0].nearClippingPlane) == 0, "unknown classic tenth slot retained");
    auto copyCamera = f.Arguments(MapDataKind::Cameras, true); copyCamera.insert("fields", QJsonObject{{"name", "Second View"}});
    Need(InvokeAuxiliaryTool("auxiliary_copy", copyCamera, f.host).value("applied").toBool(), "camera copy");
    Need(InvokeAuxiliaryTool("auxiliary_delete", f.Arguments(MapDataKind::Cameras, true), f.host).value("applied").toBool(), "camera delete");
    auto refused = f.Arguments(MapDataKind::Cameras, true); refused.insert("fields", QJsonObject{{"roll", 99}}); f.reject = true;
    const auto bytes = f.edits.Snapshot().Member(MapDataKind::Cameras).sources.front().data;
    Need(Code(InvokeAuxiliaryTool("auxiliary_update", refused, f.host)) == "HOST_DENIED" && f.edits.Snapshot().Member(MapDataKind::Cameras).sources.front().data == bytes, "prepublish rejection leaves bytes and undo unchanged");
    refused.insert("fields", QJsonObject{{"near_clipping_plane", 10}});
    Need(Code(InvokeAuxiliaryTool("auxiliary_update", refused, f.host)) == "AUX_ARGUMENT_UNKNOWN", "unknown slot is absent from writable protocol");
}
void PartialUpdatesPreserveNativeBytes() {
    HostFixture f;
    auto create = f.Arguments(MapDataKind::Regions); create.insert("fields", RegionFields("Initial", 1));
    Need(InvokeAuxiliaryTool("auxiliary_create", create, f.host).value("applied").toBool(), "retention region source");
    auto regions = *f.edits.Snapshot().Regions();
    regions.entries[0].name = std::string("\x81\x40", 2); regions.entries[0].colorBgra[3] = 177;
    regions.entries[0].weatherId = {std::byte{0xff}, std::byte{1}, std::byte{2}, std::byte{3}};
    Need(f.edits.EditRegions(regions).Succeeded(), "owned native opaque fields fixture"); ++f.epoch;
    auto update = f.Arguments(MapDataKind::Regions, true); update.insert("fields", QJsonObject{{"top", 900}});
    Need(InvokeAuxiliaryTool("auxiliary_update", update, f.host).value("applied").toBool(), "partial native region update");
    const auto afterRegion = f.edits.Snapshot();
    const auto& row = afterRegion.Regions()->entries[0];
    Need(row.name == regions.entries[0].name && row.colorBgra[3] == 177 && row.weatherId == regions.entries[0].weatherId, "unmentioned native name, weather and reserved color byte retained");
    auto camera = f.Arguments(MapDataKind::Cameras); camera.insert("fields", CameraFields("View"));
    Need(InvokeAuxiliaryTool("auxiliary_create", camera, f.host).value("applied").toBool(), "retention camera source");
    auto cameras = *f.edits.Snapshot().Cameras(); cameras.entries[0].nearClippingPlane = std::bit_cast<float>(0x80000000u);
    cameras.entries[0].targetY = std::bit_cast<float>(0x3f800001u);
    Need(f.edits.EditCameras(cameras).Succeeded(), "owned tenth slot bit pattern"); ++f.epoch;
    update = f.Arguments(MapDataKind::Cameras, true); update.insert("fields", QJsonObject{{"rotation", 45}});
    Need(InvokeAuxiliaryTool("auxiliary_update", update, f.host).value("applied").toBool(), "partial camera update");
    const auto final = f.edits.Snapshot();
    Need(std::bit_cast<std::uint32_t>(final.Cameras()->entries[0].nearClippingPlane) == 0x80000000u
        && std::bit_cast<std::uint32_t>(final.Cameras()->entries[0].targetY) == 0x3f800001u, "read-only slot and untouched float bits retained");
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path()); std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); Need(static_cast<bool>(out), "write owned workspace");
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, std::as_bytes(std::span(text))); }
struct WorkspaceFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("audastra-mcp-aux-" + std::to_string(GetCurrentProcessId())
        + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<McpWorkspace> workspace;
    WorkspaceFixture() {
        const auto ui = root / "ui";
        Write(ui / "config", "base\n"); Write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n");
        for (const auto* file : {"action.txt", "event.txt", "condition.txt", "call.txt"}) Write(ui / "base" / file, "");
        const auto catalog = Trigger::UiCatalog::Load(ui); Need(catalog.Succeeded(), "workspace catalog");
        const auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); Need(empty.Succeeded(), "workspace empty T");
        const auto pair = Trigger::WtgWctCodec{}.Serialize(*empty.value, *catalog.value); Need(pair.Succeeded(), "workspace WTG/WCT pair");
        Write(root / "source/map/war3map.wtg", pair.value->wtg); Write(root / "source/map/war3map.wct", pair.value->wct);
        Write(root / "source/map/war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes);
        auto opened = McpWorkspace::Open(root / "source", ui); Need(opened.Succeeded(), "open actual standalone workspace"); workspace = std::move(*opened.value);
    }
    ~WorkspaceFixture() { workspace.reset(); std::error_code error; std::filesystem::remove_all(root, error); }
    PreparedAuxiliaryEdit Plan(MapDataKind kind, AuxiliaryEditAction action, std::string name = {}, int id = 0) {
        auto context = workspace->MapDataContext("owned-workspace", 1); Need(context.Succeeded(), "real typed context");
        AuxiliaryEditRequest request; request.kind = kind; request.action = action;
        request.expectedMemberSha256 = *AuxiliaryEditing::MemberIdentity(*context.value->document, kind).value;
        if (action != AuxiliaryEditAction::Create) request.expectedRowKey = *MapFieldRowKey(*context.value->document, kind, 0).value;
        if (action != AuxiliaryEditAction::Delete) {
            if (kind == MapDataKind::Regions) {
                RegionRecord row{}; row.name = std::move(name); row.creationNumber = id; row.left = -128; row.bottom = -64; row.right = 256; row.top = 512;
                request.record = row;
            } else {
                CameraRecord row{}; row.name = std::move(name); row.targetDistance = 1500; row.fieldOfView = 70; row.farClippingPlane = 5000; request.record = row;
            }
        }
        auto plan = AuxiliaryEditing::Prepare(context.value->document, request, workspace->AuxiliaryReferences()); Need(plan.Succeeded(), "shared actual workspace plan"); return std::move(*plan.value);
    }
};
void StandaloneSequenceExportReseedAndRollback() {
    WorkspaceFixture f; const auto revision = f.workspace->Revision();
    const auto begun = f.workspace->BeginEdit(revision); Need(begun.Succeeded(), "begin original EditorSession transaction");
    Need(f.workspace->ApplyAuxiliaryPlan(f.Plan(MapDataKind::Regions, AuxiliaryEditAction::Create, "Region", 1)).Succeeded(), "standalone missing W3R");
    Need(f.workspace->ApplyAuxiliaryPlan(f.Plan(MapDataKind::Cameras, AuxiliaryEditAction::Create, "View")).Succeeded(), "standalone missing W3C in same transaction");
    Need(f.workspace->ApplyAuxiliaryPlan(f.Plan(MapDataKind::Regions, AuxiliaryEditAction::Update, "Renamed", 1)).Succeeded(), "second structural command replays earlier region/camera creation");
    const auto preview = f.workspace->PreviewEdit(*begun.value, revision); Need(preview.Succeeded() && preview.value->changes.size() == 2, "same EditorSession contains only two sealed member overlays");
    for (const auto& change : preview.value->changes) Need(change.kind == "map-auxiliary" && change.beforeSha256.empty(), "new native members remain explicitly missing in original source");
    const auto validation = f.workspace->ValidateEdit(*begun.value, revision);
    if (!validation.satisfied) for (const auto& diagnostic : validation.diagnostics)
        std::cerr << diagnostic.subsystem << '/' << diagnostic.code << ": " << diagnostic.message << '\n';
    Need(validation.satisfied, "shared validation accepts only replayed structure");
    Need(f.workspace->ExportCandidate(*begun.value, revision, f.root / "exported").Succeeded(), "directory candidate export");
    const auto regionBytes = IO::ReadFileBytes(f.root / "exported/map/war3map.w3r", 1024 * 1024);
    const auto cameraBytes = IO::ReadFileBytes(f.root / "exported/map/war3map.w3c", 1024 * 1024);
    Need(regionBytes.Succeeded() && cameraBytes.Succeeded(), "new native files exported beside source W3I");
    const auto regions = ParseRegions(*regionBytes.value);
    const auto cameras = ParseCameras(*cameraBytes.value);
    Need(regions.Succeeded() && regions.value->entries[0].name == "Renamed" && cameras.Succeeded() && cameras.value->entries[0].name == "View", "native files reopen with complete values");
    Need(!std::filesystem::exists(f.root / "source/map/war3map.w3r"), "source directory remains read-only");
    const auto next = f.workspace->BeginEdit(f.workspace->Revision()); Need(next.Succeeded(), "subsequent edit reseeds committed command sequence");
    Need(f.workspace->ApplyAuxiliaryPlan(f.Plan(MapDataKind::Regions, AuxiliaryEditAction::Delete)).Succeeded(), "delete through reseeded history");
    Need(f.workspace->Rollback(*next.value, f.workspace->Revision()).Succeeded(), "rollback discards only active changes");
    const auto current = f.workspace->MapDataContext("owned-workspace", 1);
    Need(current.Succeeded() && current.value->document->Regions()->entries[0].name == "Renamed", "rollback restores committed document identity and records");
    const auto reentry = f.workspace->BeginEdit(f.workspace->Revision()); Need(reentry.Succeeded(), "begin callback reentry fixture");
    auto reentryPlan = f.Plan(MapDataKind::Regions, AuxiliaryEditAction::Delete);
    const auto rejected = f.workspace->ApplyAuxiliaryPlan(reentryPlan, [&](const auto&, const auto&, const auto&) {
        return f.workspace->Rollback(*reentry.value, f.workspace->Revision());
    });
    Need(!rejected.Succeeded() && !f.workspace->HasActiveTransaction(), "callback rollback cannot publish into a missing or replacement transaction");
    const auto stale = f.workspace->BeginEdit(f.workspace->Revision()); Need(stale.Succeeded(), "begin external source rejection");
    auto plan = f.Plan(MapDataKind::Cameras, AuxiliaryEditAction::Delete);
    Write(f.root / "source/map/war3map.w3c", "broken external member");
    Need(!f.workspace->ApplyAuxiliaryPlan(plan).Succeeded(), "external new member changes are refused before publication");
}
}
int main() {
    try { FacadeCrudDryRunAndOriginalUndo(); PartialUpdatesPreserveNativeBytes(); StandaloneSequenceExportReseedAndRollback(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Auxiliary MCP authored fixtures passed.\n"; return 0;
}
