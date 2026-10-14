#include "WorldPlacementPending.h"
#include "WorldPlacementModels.h"
#include "EditorProject.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "../../src/Render/Animation/AnimationParticles.h"
#include "../MapDataFixtures.h"

#include <QApplication>

#include <iostream>
#include <set>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Modules;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 多变体装饰物：变体文件不存在才改用无编号模型；存在或来源出错都保留原路径。
void VariantFallbackOnlyForActuallyMissingFiles() {
    Resources::ResourceResolver resolver;
    Resources::ResourceSource source;
    source.id = "fixture"; source.layer = Resources::ResourceLayer::MapImport; source.priority = Resources::ResourcePriority::MapArchive;
    const std::set<std::string> files{"GRASS.MDX", "TREE0.MDX", "TREE.MDX", "ROCK.MDX"};
    source.probe = [files](std::string_view path) -> Result<Resources::ResourceProbe> {
        if (path == "ROCK1.MDX") return Result<Resources::ResourceProbe>::Failure("Fixture", "FIXTURE_READ_FAILED", "来源读取失败");
        return Result<Resources::ResourceProbe>::Success(files.contains(std::string(path))
            ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)} : Resources::ResourceProbe{});
    };
    source.read = [](std::string_view) { return Result<std::vector<std::byte>>::Success({}); };
    Require(resolver.AddSource(std::move(source)).Succeeded(), "fixture source");
    WorldPlacementCapture capture;
    for (const char* path : {"grass2.mdx", "tree0.mdx", "rock1.mdx", "bush1.mdx"}) {
        Render::ModelInstanceRequest request; request.modelPath = path; capture.requests.push_back(request);
    }
    capture.variantFallbacks = {{0, "grass2.mdx", "grass.mdx"}, {1, "tree0.mdx", "tree.mdx"},
        {2, "rock1.mdx", "rock.mdx"}, {3, "bush1.mdx", "bush.mdx"}};
    ModelVariantFallbacks decided;
    ResolveModelVariantFallbacks(capture, resolver, decided);
    Require(ApplyModelVariantFallbacks(capture, decided) == 1, "only the missing variant with an existing base switches");
    Require(capture.requests[0].modelPath == "grass.mdx", "missing variant uses the unnumbered model");
    Require(capture.requests[1].modelPath == "tree0.mdx", "existing variant is kept");
    Require(capture.requests[2].modelPath == "rock1.mdx", "source errors are not downgraded to the base model");
    Require(capture.requests[3].modelPath == "bush1.mdx", "missing base keeps the original missing variant");
    // 主线程按已判定的表恢复同一结果；变体后来出现时判定改回保留。
    WorldPlacementCapture again = capture; again.requests[0].modelPath = "grass2.mdx";
    Require(ApplyModelVariantFallbacks(again, decided) == 1 && again.requests[0].modelPath == "grass.mdx", "table reuse");
    decided.insert_or_assign("grass2.mdx", std::string{});
    Require(ApplyModelVariantFallbacks(again, decided) == 1 && again.requests[0].modelPath == "grass2.mdx", "table reverts");
}
void CreationRemapsAndUndoDoesNotLeaveAGhost() {
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()},
        {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()}};
    const auto source = MapDocument::FromMembers(members); MapDataEditSession edits(source);
    WorldInstanceBinding binding{7, 0, edits.SessionIdentity(), 0, "fixture-source"};
    auto initial = WorldInstanceRegistry::Create(source, binding, {}, {}); Require(initial.Succeeded(), "empty registry");
    UnitPlacement unit; unit.rawcode = "hfoo"; unit.scale = {1,1,1}; unit.position = {100,200,12};
    auto added = AddUnitPlacement(edits, unit); Require(added.Succeeded(), "formal addition");
    const auto receipt = edits.LastAppliedEdit(); const auto document = edits.Snapshot();
    binding.mapRevision = binding.editRevision = edits.Revision();
    const std::array<PlacementId,1> rows{*added.value};
    Render::ModelInstanceRequest request; request.modelPath = "Fixture.mdx"; request.position = unit.position;
    const std::array<Render::ModelInstanceRequest,1> requests{request}; WorldPlacementPending pending;
    Require(pending.Remember(binding, receipt, document, rows, requests).Succeeded(), "remember actual lineage");
    const auto timing = pending.TimingCommands(0, binding.editRevision);
    Require(timing.size() == 1 && timing[0].commandIdentity == receipt.CommandIdentity()
        && timing[0].documentRevision == binding.editRevision && timing[0].placements == 1 && timing[0].retainedPlacements == 1
        && timing[0].inputStartedNs == 0, "command timing keeps actual identity and unavailable input is zero");
    Require(pending.TimingCommands(binding.editRevision, binding.editRevision).empty(),
        "already completed prefix cannot be reported as a new placement");
    auto wrongSource = binding; ++wrongSource.editSessionIdentity;
    WorldPlacementPending rejected;
    Require(!rejected.Remember(wrongSource, receipt, document, rows, requests).Succeeded() && rejected.Entries().empty(),
        "foreign edit session cannot create placeholder");
    Require(!pending.Remember(binding, receipt, document, rows, requests).Succeeded() && pending.Entries().size() == 1,
        "duplicate creation cannot add a second marker");
    Render::OverlayLines lines; pending.AppendLines(lines, binding);
    Require(lines.size() == 12 && !pending.Entries()[0].renderIdentity && pending.Entries()[0].memberSha.size() == 64,
        "provisional creation is visible before registry or asset decode");
    auto stale = binding; ++stale.editRevision; lines.clear(); pending.AppendLines(lines, stale);
    Require(lines.empty(), "new revision cannot reuse an unbound creation row");
    auto next = (*initial.value)->Prepare(receipt, (*initial.value)->Binding(), binding.mapRevision, document, requests, rows);
    Require(next.Succeeded() && pending.Reconcile(next.value->registry).Succeeded(), "explicit registry remap");
    const auto renderIdentity = pending.Entries()[0].renderIdentity;
    Require(renderIdentity && renderIdentity == next.value->registry->Find(rows[0]), "render identity comes from registry");
    Render::AnimatedModelScene scene; scene.sceneVersion = 4; scene.generation = 7; scene.documentRevision = binding.mapRevision;
    scene.sourceIdentity = binding.sourceIdentity; scene.specs = next.value->registry->CapturedSpecs();
    Render::AnimationFrame completed; completed.sceneVersion = 4; completed.documentRevision = binding.mapRevision;
    completed.instances.resize(1); Render::AnimationPoseLink pose; pose.identity = *renderIdentity; completed.poses.push_back(pose);
    lines.clear(); pending.AppendLines(lines, binding, &scene, &completed); Require(lines.empty(), "completed exact instance replaces marker");
    completed.poses[0].hidden = true; pending.AppendLines(lines, binding, &scene, &completed);
    Require(lines.size() == 12, "unavailable model retains explicit placeholder");
    Require(edits.Undo(), "undo addition"); const auto undone = edits.Snapshot();
    auto back = next.value->registry->Prepare(edits.LastAppliedEdit(), next.value->registry->Binding(), edits.Revision(), undone, {}, {});
    Require(back.Succeeded() && pending.Reconcile(back.value->registry).Succeeded(), "undo lineage remaps");
    const auto undoneTiming = pending.TimingCommands(0, edits.Revision());
    Require(undoneTiming.size() == 1 && undoneTiming[0].placements == 1 && undoneTiming[0].retainedPlacements == 0,
        "creation undone before display is not counted as a remaining placement");
    lines.clear(); pending.AppendLines(lines, back.value->registry->Binding()); Require(lines.empty(), "undo leaves no ghost");
    Require(edits.Redo(), "redo addition");
    auto redone = back.value->registry->Prepare(edits.LastAppliedEdit(), back.value->registry->Binding(), edits.Revision(), edits.Snapshot(), requests, rows);
    Require(redone.Succeeded() && pending.Reconcile(redone.value->registry).Succeeded(), "redo original identity");
    lines.clear(); pending.AppendLines(lines, redone.value->registry->Binding());
    Require(lines.size() == 12 && pending.Entries()[0].renderIdentity == renderIdentity, "redo same identity reappears");
    pending.Clear(); lines.clear(); pending.AppendLines(lines, binding); Require(lines.empty(), "map switch clears markers");
}
void CachedPreviewOnlyTranslatesOwnedPose() {
    Render::AnimatedModelScene scene; scene.sceneVersion = 10; scene.documentRevision = 2;
    Render::AnimationInstanceSpec spec; spec.identity = {12,7}; scene.specs.push_back(spec);
    Render::AnimationFrame frame; frame.sceneVersion = 10; frame.documentRevision = 2; frame.contentVersion = 20;
    const std::array<float,12> old{1,0,0,0,0,1,0,0,0,0,1,0};
    Render::AnimationInstanceState state; state.identity = spec.identity; state.transform = old; state.elapsedMs = 777; frame.states.push_back(state);
    frame.instances.push_back({{0,{1,0,0,0,0,1,0,0,0,0,1,0},{1,1,1}},0,0});
    Render::AnimationPoseLink pose; pose.identity = spec.identity; pose.poseVersion = 55; frame.poses.push_back(pose);
    frame.groups.push_back({{1,0,0,0,1,0,0,0,1,0,0,0}}); frame.alphas.push_back(1);
    auto moved = old; moved[3] = 100; moved[7] = 200; moved[11] = 300;
    auto translated = TranslatePlacementPreviewFrame(scene, frame, std::span(&moved,1), 21);
    Require(translated.Succeeded(), "cached translation succeeds without service/reader");
    Require(translated.value.value()->instances[0].instance.transform[3] == 100 && frame.instances[0].instance.transform[3] == 0,
        "owned translated frame preserves original");
    Require(translated.value.value()->poses[0].poseVersion == 55 && translated.value.value()->states[0].elapsedMs == 777
        && translated.value.value()->groups[0].values[0] == frame.groups[0].values[0], "pose/time/group evaluation is retained");
    auto rotated = moved; rotated[0] = 0;
    Require(!TranslatePlacementPreviewFrame(scene, frame, std::span(&rotated,1), 22).Succeeded(), "rotation needs real prepare");
    frame.particleVertices.resize(1);
    Require(!TranslatePlacementPreviewFrame(scene, frame, std::span(&moved,1), 22).Succeeded(), "world particle geometry cannot silently move");
    frame.particleVertices.clear();
    frame.particleCheckpoint = std::make_shared<Render::AnimationParticleCheckpoint>();
    Require(!TranslatePlacementPreviewFrame(scene, frame, std::span(&moved,1), 22).Succeeded(),
        "zero current geometry still owns a particle checkpoint whose content identity must not be forged");
}
void TargetedRowsShareFullCaptureRules() {
    QtProject::EditorProject project;
    const std::string fields = "[hfoo]\nfile = \"Fixture.mdl\"\nusca = 2\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Unit).Parse(
        std::span(reinterpret_cast<const std::byte*>(fields.data()), fields.size()));
    Require(parsed.document != nullptr, "original object fields parsed");
    project.objectDocuments_[0].reset(dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    Require(project.objectDocuments_[0] != nullptr, "owned unit object fields");
    auto units = ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    UnitPlacement unit; unit.rawcode = "hfoo"; unit.creationId = 7; unit.position = {1,2,3}; unit.scale = {1,1,1};
    units.value->units.push_back(unit); unit.rawcode = "zzzz"; ++unit.creationId; units.value->units.push_back(unit);
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *units.value->Serialize().value}};
    const auto document = MapDocument::FromMembers(members);
    const auto row = MakePlacementId(PlacementFile::Units, 0, document.Units()->units[0]);
    const auto full = CaptureWorldPlacements(project, document);
    const auto one = CapturePlacementRows(project, document, std::span(&row,1));
    Require(one.Succeeded() && one.value->ids.size() == 1 && full.ids.size() == 1
        && one.value->requests[0].modelPath == full.requests[0].modelPath
        && one.value->requests[0].scale == full.requests[0].scale, "bounded row uses same model/scale rules");
    const auto unknown = MakePlacementId(PlacementFile::Units, 1, document.Units()->units[1]);
    const auto marker = CapturePlacementRows(project, document, std::span(&unknown,1), true);
    Require(marker.Succeeded() && marker.value->unresolved == 1 && marker.value->ids.size() == 1
        && marker.value->requests[0].modelPath.empty() && marker.value->requests[0].position == unit.position,
        "unknown row retains source position without inventing a model");
    auto stale = row; ++stale.creationId;
    Require(!CapturePlacementRows(project, document, std::span(&stale,1)).Succeeded(), "query rejects stale typed row");
}
}
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication application(argc, argv);
    try { VariantFallbackOnlyForActuallyMissingFiles(); CreationRemapsAndUndoDoesNotLeaveAGhost(); CachedPreviewOnlyTranslatesOwnedPose(); TargetedRowsShareFullCaptureRules();
        std::cout << "placement pending checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
