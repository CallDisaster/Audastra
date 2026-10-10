#include "WorldPathingHelperPresenter.h"
#include "EditorProject.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "../MapDataFixtures.h"

#include <QApplication>
#include <QTemporaryDir>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Modules;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::shared_ptr<const PlacementPresentationFields> Fields() {
    auto fields = std::make_shared<PlacementPresentationFields>();
    fields->category = {PlacementFieldState::Known, "P", {}};
    fields->modelPath = {PlacementFieldState::Known, "RealSourceModel.mdx", {}};
    fields->pathingTexture = {PlacementFieldState::Known, "PathingTextures\\Fixture.tga", {}};
    fields->useClickHelper = {PlacementFieldState::Known, true, {}};
    for (const auto* name : {"bcat", "buch", "bfil", "bptx"})
        fields->origins.push_back({name, "BASE", "map-effective-native", "war3map.w3b", 42});
    return fields;
}
WorldPlacementPresentationCandidate Candidate(std::uint32_t index, std::shared_ptr<const PlacementPresentationFields> fields) {
    return {index, {ObjectData::ObjectType::Destructable, "Q001"},
        MapData::PlacementId{MapData::PlacementFile::Doodads, index, "Q001", 7}, std::move(fields)};
}
Resources::ResolvedResource Tga(std::string_view path, unsigned width, unsigned height) {
    std::vector<std::byte> bytes(18 + width * height * 3);
    bytes[2] = std::byte{2}; bytes[12] = static_cast<std::byte>(width);
    bytes[14] = static_cast<std::byte>(height); bytes[16] = std::byte{24};
    return {{std::string(path), Resources::ResourceLayer::Game, 300, "fixture-game:War3.mpq", "actual.tga"}, std::move(bytes)};
}
void ActualFieldsAndWorkerCacheShareTheSameHelper() {
    QtProject::EditorProject project;
    const std::string lni = "[BASE]\nbcat = \"P\"\nbuch = 1\nbfil = \"\"\nbptx = \"PathingTextures\\\\Fixture.tga\"\n"
        "[Q001]\n_parent = \"BASE\"\n";
    auto parsed = ObjectData::LniObjectCodec(ObjectData::ObjectType::Destructable).Parse(
        std::span(reinterpret_cast<const std::byte*>(lni.data()), lni.size()));
    Require(parsed.document != nullptr, "original effective-field fixture parses");
    project.objectDocuments_[static_cast<std::size_t>(ObjectData::ObjectType::Destructable)].reset(
        dynamic_cast<ObjectData::LniObjectDocument*>(parsed.document.release()));
    MapData::DoodadPlacement placement; placement.rawcode = "Q001"; placement.position = {100, 200, 9}; placement.scale = {4, 5, 6};
    WorldPlacementPresentationCandidate captured;
    const auto preview = CapturePlacementPreview(project, ObjectData::ObjectType::Destructable, placement, 0, &captured);
    Require(preview && preview->modelPath.empty() && preview->position == placement.position
        && preview->scale == std::array<float, 3>{1, 1, 1} && captured.fields, "blank model helper survives parent-chain capture");
    Require(captured.fields->origins[0].objectRawcode == "BASE" && captured.fields->modelPath.state == PlacementFieldState::Known,
        "real parent and empty bfil provenance retained");

    PlacementPresentationContext context{{{"fixture", 1, 7, std::string(64, 'a')}, 8, 0}, "resource-A", {}};
    unsigned reads = 0;
    context.resourceReader = [&](auto path, auto) { ++reads; return Result<Resources::ResolvedResource>::Success(Tga(path, 2, 3)); };
    std::vector<Render::ModelInstanceRequest> requests(21);
    for (auto& request : requests) { request.modelPath = "RealSourceModel.mdx"; request.position = {100, 200, 9}; request.scale = {4, 5, 6}; }
    std::vector<WorldPlacementPresentationCandidate> candidates;
    for (std::uint32_t i = 0; i < 20; ++i) candidates.push_back(Candidate(i, Fields()));
    const auto prepared = PrepareWorldPathingHelpers(requests, candidates, context);
    Require(prepared.Succeeded() && reads == 1 && prepared.value->helpers.size() == 20
        && prepared.value->helperRequests.size() == requests.size() && !prepared.value->helperRequests.back(), "one real TGA read for twenty helpers");
    const auto& timings = prepared.value->timings;
    Require(timings.candidateCount == 20 && timings.helperCount == 20 && timings.queryCount == 20
        && timings.resourceReads == 1 && timings.resourceCacheHits == 0 && timings.assetsCreated == 1 && timings.assetsReused == 19
        && timings.totalNs > 0 && timings.totalNs >= timings.mappingNs + timings.cacheNs + timings.classifyNs
            + timings.queryNs + timings.assetNs + timings.verifyNs,
        "batch repeated the same source capture or helper phase accounting overlapped");
    for (std::size_t i = 0; i < 20; ++i)
        Require(requests[i].ownedStaticAsset == requests[0].ownedStaticAsset && prepared.value->helperRequests[i]
            && requests[i].scale == std::array<float, 3>{1, 1, 1} && requests[i].modelPath == "RealSourceModel.mdx"
            && requests[i].position == std::array<float, 3>{100, 200, 9}, "asset shares without changing source transform/path");
    const auto& row = *prepared.value->helpers.front().row;
    Require(row.footprint && row.footprint->worldWidth == 64 && row.footprint->worldHeight == 96 && row.textureSha256
        && !row.footprint->nativeObjectScaleVerified && !requests.back().ownedStaticAsset, "typed source dimensions and ordinary model preserved");
    auto reused = PrepareWorldPathingHelpers(requests, candidates, context, {}, prepared.value->cache);
    Require(reused.Succeeded() && reads == 1 && reused.value->helpers.front().row->inputIdentity == row.inputIdentity,
        "same frozen resource identity reuses owned cache");
    Require(reused.value->timings.resourceReads == 0 && reused.value->timings.resourceCacheHits == 1
        && reused.value->timings.assetsCreated == 0 && reused.value->timings.assetsReused == 20,
        "new preparation did not rebuild its batch facts from one actual cached source");
    context.resourceIdentity = "resource-B";
    context.resourceReader = [&](auto path, auto) { ++reads; return Result<Resources::ResolvedResource>::Success(Tga(path, 3, 2)); };
    const auto oldAsset = requests[0].ownedStaticAsset;
    auto changed = PrepareWorldPathingHelpers(requests, candidates, context, {}, prepared.value->cache);
    Require(changed.Succeeded() && reads == 2 && requests[0].ownedStaticAsset != oldAsset
        && changed.value->helpers.front().row->textureSha256 != row.textureSha256, "new actual source identity invalidates texture and geometry");
}
void MissingFootprintDoesNotInventNativeSizeOrRemapRows() {
    PlacementPresentationContext context{{}, "missing-source", [](auto, auto) {
        return Result<Resources::ResolvedResource>::Failure("Fixture", "EXACT_SOURCE_MISSING", "no lower source"); }};
    auto unknown = std::make_shared<PlacementPresentationFields>(*Fields());
    unknown->useClickHelper = {PlacementFieldState::Opaque, {}, "opaque original"};
    std::vector<Render::ModelInstanceRequest> requests(2);
    const std::array candidates{Candidate(0, Fields()), Candidate(1, unknown)};
    auto prepared = PrepareWorldPathingHelpers(requests, candidates, context);
    Require(prepared.Succeeded() && prepared.value->unknownFootprints == 1 && prepared.value->helpers.size() == 1
        && requests[0].ownedStaticAsset && !requests[1].ownedStaticAsset && !prepared.value->helperRequests[1], "opaque classification is not a guessed helper");
    const auto& row = *prepared.value->helpers[0].row;
    Require(!row.footprint && row.footprintState == PlacementFootprintState::Unavailable
        && row.resourceDiagnostics[0].code == "EXACT_SOURCE_MISSING", "failed actual source survives in owned row");
    float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
    for (const auto& mesh : requests[0].ownedStaticAsset->Meshes()) for (const auto& vertex : mesh.vertices) {
        lo = std::min(lo, vertex.x); hi = std::max(hi, vertex.x);
    }
    Require(hi - lo == 8, "unknown marker is not a fabricated 64-unit footprint");
    requests.assign(2, {});
    const std::array duplicate{Candidate(0, Fields()), Candidate(0, Fields())};
    Require(!PrepareWorldPathingHelpers(requests, duplicate, context).Succeeded() && !requests[0].ownedStaticAsset,
        "duplicate request mapping rejects before partial mutation");
}
void DefaultClassificationStillChecksEachOwnerAndHelperRowIdentity() {
    PlacementPresentationContext context{{{"classification-map", 2, 9, "map-sha"}, 3, 4}, "classified-resources", {}};
    unsigned reads = 0;
    context.resourceReader = [&](auto path, auto) { ++reads; return Result<Resources::ResolvedResource>::Success(Tga(path, 2, 3)); };
    std::vector<Render::ModelInstanceRequest> requests(259);
    std::vector<WorldPlacementPresentationCandidate> candidates;
    const auto fields = Fields();
    for (std::uint32_t index = 0; index < 256; ++index) {
        requests[index].modelPath = "ordinary.mdx";
        candidates.push_back({index, {ObjectData::ObjectType::Unit, "u001"},
            MapData::PlacementId{MapData::PlacementFile::Units, index, "u001", static_cast<std::int32_t>(index)}, fields});
    }
    auto opaque = std::make_shared<PlacementPresentationFields>(*fields);
    opaque->useClickHelper = {PlacementFieldState::Opaque, {}, "opaque helper original"};
    candidates.push_back(Candidate(256, opaque));
    auto invalidOwner = candidates.front(); invalidOwner.requestIndex = 257;
    invalidOwner.owner->file = MapData::PlacementFile::Doodads; candidates.push_back(std::move(invalidOwner));
    candidates.push_back(Candidate(258, fields));
    auto prepared = PrepareWorldPathingHelpers(requests, candidates, context);
    Require(prepared.Succeeded() && reads == 1 && prepared.value->helpers.size() == 1
        && prepared.value->helpers.front().requestIndex == 258 && prepared.value->unavailableClassifications == 2,
        "ordinary and unknown rows were guessed as helpers or invalid default owner bypassed validation");
    Require(std::any_of(prepared.value->diagnostics.begin(), prepared.value->diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "PLACEMENT_PRESENTATION_INPUT_INVALID";
    }), "fast classification discarded exact invalid owner diagnostic");
    for (std::size_t index = 0; index < 258; ++index)
        Require(!requests[index].ownedStaticAsset && !prepared.value->helperRequests[index], "ordinary or invalid candidate mutated into a helper");
    const auto& row = *prepared.value->helpers.front().row;
    Require(row.owner == candidates.back().owner && row.version == context.version && !row.inputIdentity.empty()
        && row.textureSha256 && row.fields.origins.front().byteOffset == 42, "actual helper lost complete current owner/source facts");
    ++context.version.document.revision;
    auto rebound = PrepareWorldPathingHelpers(requests, candidates, context, {}, prepared.value->cache);
    Require(rebound.Succeeded() && reads == 1 && rebound.value->helpers.front().row->version == context.version
        && rebound.value->helpers.front().row->inputIdentity != row.inputIdentity,
        "asset reuse relabelled an old helper row instead of producing its current revision identity");
}

MapDocument DeltaDocument() {
    auto units = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    auto doodads = MapData::ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11());
    Require(units.Succeeded() && doodads.Succeeded(), "delta placement fixture parses");
    MapData::UnitPlacement unit; unit.rawcode = "hfoo"; unit.creationId = 5; unit.scale = {1, 1, 1};
    units.value->units.push_back(unit);
    MapData::DoodadPlacement blocker; blocker.rawcode = "Q001"; blocker.creationId = 7; blocker.scale = {1, 1, 1};
    doodads.value->doodads.push_back(blocker); blocker.creationId = 8; doodads.value->doodads.push_back(blocker);
    const auto unitBytes = units.value->Serialize(), doodadBytes = doodads.value->Serialize();
    Require(unitBytes.Succeeded() && doodadBytes.Succeeded(), "delta fixture serializes genuine members");
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *unitBytes.value}, {"war3map.doo", *doodadBytes.value}};
    return MapDocument::FromMembers(members);
}
struct DeltaCapture {
    std::vector<Render::ModelInstanceRequest> requests;
    std::vector<MapData::PlacementId> owners;
    std::vector<WorldPlacementPresentationCandidate> candidates;
    std::vector<War3RenderCore::InstanceIdentity> identities;
};
DeltaCapture CaptureDelta(const MapDocument& document, const std::shared_ptr<const PlacementPresentationFields>& fields,
    bool helperCandidatesOnly = false) {
    DeltaCapture capture;
    const auto add = [&](MapData::PlacementId owner, const MapData::PlacementTransform& transform, ObjectData::ObjectType type) {
        const auto requestIndex = static_cast<std::uint32_t>(capture.requests.size());
        Render::ModelInstanceRequest request; request.modelPath = "RealSourceModel.mdx";
        request.position = transform.position; request.angle = transform.rotation; request.scale = transform.scale;
        capture.requests.push_back(std::move(request)); capture.owners.push_back(owner);
        if (!helperCandidatesOnly || type == ObjectData::ObjectType::Destructable)
            capture.candidates.push_back({requestIndex, {type, owner.rawcode}, owner, fields});
    };
    for (std::uint32_t i = 0; i < document.Units()->units.size(); ++i)
        add(MapData::MakePlacementId(MapData::PlacementFile::Units, i, document.Units()->units[i]),
            document.Units()->units[i], ObjectData::ObjectType::Unit);
    for (std::uint32_t i = 0; i < document.Doodads()->doodads.size(); ++i)
        add(MapData::MakePlacementId(MapData::PlacementFile::Doodads, i, document.Doodads()->doodads[i]),
            document.Doodads()->doodads[i], ObjectData::ObjectType::Destructable);
    return capture;
}
void WriteFixtureBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close(); Require(!stream.fail(), "write isolated source bytes");
}
enum class DeltaSource { Archive, PositiveDisk, MissingDisk };
struct DeltaFixture {
    // 临时目录最后析构，reader持有的真实租约必须先释放。
    QTemporaryDir directory;
    MapDocument original = DeltaDocument();
    MapData::MapDataEditSession edits{original};
    std::shared_ptr<const PlacementPresentationFields> fields = Fields();
    WorldTerrainSources sources;
    std::filesystem::path diskTexture;
    std::shared_ptr<const WorldInstanceRegistry> registry;
    std::shared_ptr<const WorldPathingHelperPrepared> prepared;
    PlacementPresentationContext context;
    bool helperCandidatesOnly = false;
    struct Pending {
        DeltaCapture capture;
        std::shared_ptr<const WorldInstanceRegistry> registry;
        std::shared_ptr<WorldResourceReader> reader;
        PlacementPresentationContext context;
    };
    explicit DeltaFixture(DeltaSource mode = DeltaSource::Archive, bool onlyHelpers = false)
        : helperCandidatesOnly(onlyHelpers) {
        Require(directory.isValid(), "isolated helper delta directory");
        const auto root = std::filesystem::path(directory.path().toStdWString());
        sources.archivePath = root / L"source.w3x";
        std::vector<Mpq::MpqBuildFile> files{{"Fixture.bin", {std::byte{1}}}};
        if (mode == DeltaSource::Archive) {
            files.push_back({"PathingTextures\\Fixture.tga", Tga("fixture", 2, 3).bytes});
            sources.entries.push_back({"PathingTextures\\Fixture.tga", "PathingTextures\\Fixture.tga", {}});
        } else {
            diskTexture = root / L"session.tga";
            sources.entries.push_back({"PathingTextures\\Fixture.tga", {}, diskTexture});
            if (mode == DeltaSource::PositiveDisk) WriteFixtureBytes(diskTexture, Tga("fixture", 2, 3).bytes);
        }
        Require(Mpq::WriteMpqArchive(sources.archivePath, files).Succeeded(), "write isolated helper archive");
        const auto hash = Sha256File(sources.archivePath); Require(hash.Succeeded(), "hash isolated helper archive");
        sources.archiveSha = *hash.value;
        auto reader = OpenReader();
        auto capture = CaptureDelta(original, fields, helperCandidatesOnly);
        auto created = WorldInstanceRegistry::Create(original,
            {7, edits.Revision(), edits.SessionIdentity(), edits.Revision(), sources.archiveSha}, capture.requests, capture.owners);
        Require(created.Succeeded(), "genuine initial instance registry"); registry = *created.value;
        const auto identity = WorldPathingHelperResourceIdentity(sources, 8);
        Require(identity.Succeeded(), "helper source identity");
        context = {{{"delta-fixture", 42, edits.Revision(), sources.archiveSha}, 8, 17}, *identity.value,
            WorldPathingHelperResourceReader(reader)};
        auto result = PrepareWorldPathingHelpers(capture.requests, capture.candidates, context, reader);
        Require(result.Succeeded() && result.value->helpers.size() == 2, "initial helper facts captured through shared service");
        prepared = std::make_shared<const WorldPathingHelperPrepared>(std::move(*result.value));
    }
    std::shared_ptr<WorldResourceReader> OpenReader() const {
        auto opened = WorldResourceReader::Open(sources, prepared ? prepared->cache->sourceOwner.get() : nullptr);
        Require(opened.Succeeded(), "independent verified helper resource reader");
        return std::shared_ptr<WorldResourceReader>(std::move(*opened.value));
    }
    void AppendUnit() {
        auto unit = original.Units()->units.front(); unit.position[0] = 25;
        Require(MapData::AddUnitPlacement(edits, unit).Succeeded(), "append ordinary unit through real edit transaction");
    }
    Pending Next() const {
        const auto document = edits.Snapshot();
        Pending pending; pending.capture = CaptureDelta(document, fields, helperCandidatesOnly);
        const auto changed = registry->Prepare(edits.LastAppliedEdit(), registry->Binding(), edits.Revision(),
            document, pending.capture.requests, pending.capture.owners);
        Require(changed.Succeeded(), "sealed next registry verifies original member bytes and edit lineage");
        pending.registry = changed.value->registry; pending.reader = OpenReader();
        const auto version = BindWorldPathingHelperVersion(context.version, *registry, *pending.registry);
        Require(version.Succeeded(), "old selection capture binds to committed document revision");
        pending.context = {*version.value, context.resourceIdentity, WorldPathingHelperResourceReader(pending.reader)};
        for (const auto& owner : pending.capture.owners) {
            const auto identity = pending.registry->Find(owner); Require(identity.has_value(), "current registry owns captured placement");
            pending.capture.identities.push_back(*identity);
        }
        return pending;
    }
    Result<WorldPathingHelperPrepared> Prepare(Pending& pending) const {
        return PrepareWorldPathingHelperDelta(pending.capture.requests, pending.capture.candidates, pending.context,
            pending.reader, prepared, *registry, *pending.registry, pending.capture.identities);
    }
    void Accept(Pending& pending, Result<WorldPathingHelperPrepared>& result) {
        Require(result.Succeeded(), "accept successful isolated helper delta");
        registry = pending.registry; context = pending.context;
        prepared = std::make_shared<const WorldPathingHelperPrepared>(std::move(*result.value));
    }
};
// 实际世界保持存活渲染实例顺序，新单位附在末尾；文档中的单位行却位于地物之前。
// 先经真实registry准备，再把同一身份的请求按已安装渲染顺序重排。
void KeepResidentRenderOrder(const DeltaFixture& fixture, DeltaFixture::Pending& pending) {
    auto& capture = pending.capture;
    std::vector<std::size_t> order;
    std::vector<bool> used(capture.requests.size(), false);
    for (const auto& spec : fixture.registry->CapturedSpecs()) {
        const auto found = std::find(capture.identities.begin(), capture.identities.end(), spec.identity);
        if (found == capture.identities.end()) continue;
        const auto at = static_cast<std::size_t>(found - capture.identities.begin());
        order.push_back(at); used[at] = true;
    }
    for (std::size_t i = 0; i < used.size(); ++i) if (!used[i]) order.push_back(i);
    auto requests = std::move(capture.requests);
    auto owners = std::move(capture.owners);
    auto identities = std::move(capture.identities);
    capture.requests.clear(); capture.owners.clear(); capture.identities.clear();
    std::vector<std::uint32_t> remap(order.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto at = order[i]; remap[at] = static_cast<std::uint32_t>(i);
        capture.requests.push_back(std::move(requests[at])); capture.owners.push_back(std::move(owners[at]));
        capture.identities.push_back(identities[at]);
    }
    for (auto& candidate : capture.candidates) candidate.requestIndex = remap[candidate.requestIndex];
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}
void StableHelperBindingsKeepFullFieldAndResourceEvidence() {
    DeltaFixture fixture(DeltaSource::Archive, true);
    fixture.AppendUnit(); auto pending = fixture.Next(); KeepResidentRenderOrder(fixture, pending);
    Require(pending.capture.candidates.size() == fixture.prepared->records.size(), "ordinary append leaves helper candidate roster unchanged");
    for (std::size_t i = 0; i < pending.capture.candidates.size(); ++i) {
        auto& candidate = pending.capture.candidates[i];
        Require(candidate.requestIndex == fixture.prepared->records[i].requestIndex
            && candidate.owner == fixture.prepared->records[i].currentOwner, "fixture matches stable real-world helper bindings");
        candidate.fields = std::make_shared<const PlacementPresentationFields>(*candidate.fields);
    }
    auto result = fixture.Prepare(pending);
    Require(result.Succeeded() && result.value->timings.queryCount == 0 && result.value->timings.resourceReads == 1
        && result.value->timings.assetsCreated == 0 && result.value->timings.assetsReused == 2
        && result.value->cache->sourceOwner == pending.reader
        && result.value->version.document.revision == fixture.edits.Revision()
        && result.value->helperRequests.size() == pending.capture.requests.size()
        && !result.value->helperRequests.back(), "stable helper reuse must still transfer actual resource evidence and bind current revision");
    for (std::size_t i = 0; i < result.value->records.size(); ++i)
        Require(result.value->records[i].fact == fixture.prepared->records[i].fact
            && result.value->helpers[i].row == fixture.prepared->helpers[i].row
            && result.value->helpers[i].row->version == fixture.prepared->version
            && pending.capture.requests[result.value->records[i].requestIndex].ownedStaticAsset == fixture.prepared->records[i].fact->asset,
            "unchanged facts keep captured identities while current render requests receive resident checker assets");

    const auto reader = pending.context.resourceReader;
    pending.context.resourceReader = [reader](auto path, auto maximum) {
        auto resource = reader(path, maximum);
        if (resource.Succeeded()) resource.value->location.sourceId += "-different-shadow";
        return resource;
    };
    result = fixture.Prepare(pending);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "WORLD_HELPER_SOURCE_STALE"),
        "identical bytes with changed effective source cannot hide behind stable helper bindings");
    pending.context.resourceReader = reader;
    auto changed = std::make_shared<PlacementPresentationFields>(*pending.capture.candidates[0].fields);
    ++*changed->origins.front().byteOffset;
    pending.capture.candidates[0].fields = changed;
    result = fixture.Prepare(pending);
    Require(result.Succeeded() && result.value->timings.queryCount == 1
        && result.value->helpers[0].row != fixture.prepared->helpers[0].row
        && result.value->helpers[0].row->fields.origins.front().byteOffset == 43
        && result.value->helpers[1].row == fixture.prepared->helpers[1].row,
        "stable owner and visible value cannot suppress a changed field source offset");

    pending.capture.candidates[0].fields = fixture.fields;
    std::swap(pending.capture.candidates[0], pending.capture.candidates[1]);
    result = fixture.Prepare(pending);
    Require(result.Succeeded() && result.value->timings.queryCount == 0
        && result.value->helpers[0].row == fixture.prepared->helpers[1].row,
        "noncanonical candidate order uses verified identity mapping without relabelling rows");
}
void IncrementalOwnersReuseFactsWithoutRelabellingRows() {
    DeltaFixture fixture;
    const auto firstRow = fixture.prepared->helpers[0].row, secondRow = fixture.prepared->helpers[1].row;
    const auto initialVersion = firstRow->version; const auto firstIdentity = firstRow->inputIdentity;
    const auto asset = fixture.prepared->records[1].fact->asset;
    fixture.AppendUnit(); auto added = fixture.Next(); auto result = fixture.Prepare(added);
    Require(result.Succeeded() && result.value->timings.queryCount == 0 && result.value->timings.resourceReads == 1
        && result.value->timings.assetsCreated == 0 && result.value->helpers[0].row == firstRow
        && result.value->helpers[1].row == secondRow && result.value->cache->sourceOwner == added.reader
        && result.value->version.document.revision == fixture.edits.Revision()
        && result.value->version.editorRevision == initialVersion.editorRevision,
        "ordinary append must transfer the one real resource closure without querying old helper rows");
    Require(firstRow->version == initialVersion && firstRow->inputIdentity == firstIdentity
        && added.capture.requests[result.value->helpers[0].requestIndex].ownedStaticAsset == asset,
        "captured owner/version/input identity and resident asset remain immutable");
    fixture.Accept(added, result);
    const auto removed = *firstRow->owner;
    Require(MapData::DeletePlacements(fixture.edits, std::span(&removed, 1)).Succeeded(), "delete helper with genuine index shift");
    auto deleted = fixture.Next(); result = fixture.Prepare(deleted);
    Require(result.Succeeded() && result.value->timings.queryCount == 0 && result.value->helpers.size() == 1
        && result.value->helpers[0].row == secondRow && result.value->helpers[0].currentOwner->index == 0
        && secondRow->owner->index == 1, "survivor current binding shifts without relabelling its captured row");
    fixture.Accept(deleted, result);
    Require(fixture.edits.Undo(), "undo original helper deletion"); auto restored = fixture.Next(); result = fixture.Prepare(restored);
    Require(result.Succeeded() && result.value->timings.queryCount == 1 && result.value->helpers.size() == 2
        && result.value->helpers[0].row != firstRow && result.value->helpers[1].row == secondRow
        && result.value->helpers[0].row->version == restored.context.version,
        "restored helper queries current facts instead of resurrecting an old rawcode row");
    fixture.Accept(restored, result);
    Require(fixture.edits.Redo(), "redo original deletion"); auto redone = fixture.Next(); result = fixture.Prepare(redone);
    Require(result.Succeeded() && result.value->timings.queryCount == 0 && result.value->helpers.size() == 1
        && result.value->helpers[0].row == secondRow && result.value->records.size() == redone.capture.candidates.size(),
        "redo leaves no ghost helper or stale candidate record");
}
void CopyAndFieldChangesQueryOnlyAffectedHelpers() {
    DeltaFixture fixture;
    const auto owner = *fixture.prepared->helpers[0].currentOwner;
    const auto originalRow = fixture.prepared->helpers[0].row;
    const auto copied = MapData::CopyPlacements(fixture.edits, std::span(&owner, 1), {32, 0, 0});
    Require(copied.Succeeded(), "copy helper through common placement transaction");
    auto copy = fixture.Next(); auto result = fixture.Prepare(copy);
    Require(result.Succeeded() && result.value->timings.queryCount == 1 && result.value->helpers.size() == 3
        && result.value->timings.assetsCreated == 0 && result.value->helpers[0].row == originalRow,
        "copy queries only its new owner and reuses the verified checker asset");
    fixture.Accept(copy, result);
    Require(fixture.edits.Undo(), "undo helper copy"); auto undo = fixture.Next(); result = fixture.Prepare(undo);
    Require(result.Succeeded() && result.value->timings.queryCount == 0 && result.value->helpers.size() == 2,
        "copy undo retains only installed original helpers"); fixture.Accept(undo, result);
    Require(fixture.edits.Redo(), "redo helper copy"); auto redo = fixture.Next(); result = fixture.Prepare(redo);
    Require(result.Succeeded() && result.value->timings.queryCount == 1, "copy redo rechecks restored helper facts");
    fixture.Accept(redo, result);
    fixture.AppendUnit(); auto edited = fixture.Next();
    const auto equalFields = std::make_shared<const PlacementPresentationFields>(*fixture.fields);
    for (auto& candidate : edited.capture.candidates) candidate.fields = equalFields;
    result = fixture.Prepare(edited);
    Require(result.Succeeded() && result.value->timings.queryCount == 0, "equal field values from a new capture share prior facts");
    auto changedFields = std::make_shared<PlacementPresentationFields>(*fixture.fields);
    changedFields->pathingTexture.reason = "new exact source explanation";
    auto& target = *std::find_if(edited.capture.candidates.begin(), edited.capture.candidates.end(), [](const auto& candidate) {
        return candidate.owner->file == MapData::PlacementFile::Doodads;
    });
    target.fields = changedFields; result = fixture.Prepare(edited);
    Require(result.Succeeded() && result.value->timings.queryCount == 1 && result.value->helpers[0].row != originalRow
        && result.value->helpers[0].row->fields.pathingTexture.reason == changedFields->pathingTexture.reason
        && result.value->helpers[0].row->version == edited.context.version,
        "a reason-only field change refreshes its full evidence without replacing resident geometry");
    auto changedOrigin = std::make_shared<PlacementPresentationFields>(*changedFields);
    ++*changedOrigin->origins.front().byteOffset;
    target.fields = changedOrigin; result = fixture.Prepare(edited);
    Require(result.Succeeded() && result.value->timings.queryCount == 1
        && result.value->helpers[0].row->fields.origins.front().byteOffset == 43,
        "equal visible values cannot hide a changed source origin or byte offset");
    fixture.Accept(edited, result);
    fixture.AppendUnit(); auto reclassified = fixture.Next();
    auto ordinary = std::make_shared<PlacementPresentationFields>(*changedFields); ordinary->useClickHelper.value = false;
    auto& changed = *std::find_if(reclassified.capture.candidates.begin(), reclassified.capture.candidates.end(), [](const auto& candidate) {
        return candidate.owner->file == MapData::PlacementFile::Doodads;
    });
    changed.fields = ordinary; result = fixture.Prepare(reclassified);
    Require(!result.Succeeded() && HasCode(result.diagnostics, "WORLD_HELPER_INSTANCE_REBUILD_REQUIRED"),
        "normal instance delta must not replace a surviving helper classification or resident asset");
}
void DeltaRejectsForeignMappingsAndKeepsRevisionNamespacesSeparate() {
    DeltaFixture fixture; fixture.AppendUnit(); auto pending = fixture.Next();
    const auto captured = fixture.context.version;
    const auto rebound = BindWorldPathingHelperVersion(captured, *fixture.registry, *pending.registry);
    Require(rebound.Succeeded() && rebound.value->document.revision == fixture.edits.Revision()
        && rebound.value->editorRevision == captured.editorRevision && rebound.value->catalogRevision == captured.catalogRevision,
        "document edit revision must never be guessed from selection editor revision");
    const auto fresh = BindWorldPathingHelperVersion(*rebound.value, *fixture.registry, *pending.registry);
    Require(fresh.Succeeded() && *fresh.value == *rebound.value, "already current selection revision is not advanced again");
    auto future = captured; future.document.revision = fixture.edits.Revision() + 1;
    auto wrongSha = captured; wrongSha.document.sourceSha256 = "foreign";
    Require(!BindWorldPathingHelperVersion(future, *fixture.registry, *pending.registry).Succeeded()
        && !BindWorldPathingHelperVersion(wrongSha, *fixture.registry, *pending.registry).Succeeded(),
        "future selection version and foreign SHA cannot bind current helper facts");
    const auto currentVersion = pending.context.version;
    ++pending.context.version.document.revision;
    Require(!fixture.Prepare(pending).Succeeded(), "future delta version rejected"); pending.context.version = currentVersion;
    const auto identities = pending.capture.identities;
    pending.capture.identities.back() = pending.capture.identities.front();
    Require(!fixture.Prepare(pending).Succeeded(), "wrong render identity rejected"); pending.capture.identities = identities;
    pending.capture.candidates.push_back(pending.capture.candidates.back());
    Require(!fixture.Prepare(pending).Succeeded(), "duplicate current request rejected"); pending.capture.candidates.pop_back();
    auto foreignBinding = pending.registry->Binding(); ++foreignBinding.editSessionIdentity;
    auto foreign = WorldInstanceRegistry::Create(fixture.edits.Snapshot(), foreignBinding,
        pending.capture.requests, pending.capture.owners);
    Require(foreign.Succeeded() && !BindWorldPathingHelperVersion(captured, *fixture.registry, **foreign.value).Succeeded(),
        "independently sealed foreign edit session cannot reuse current helper version");
    const auto genuine = pending.registry; pending.registry = *foreign.value;
    Require(!fixture.Prepare(pending).Succeeded(), "foreign registry delta rejected before publishing helper rows");
    pending.registry = genuine; foreignBinding = genuine->Binding(); ++foreignBinding.sourceGeneration;
    foreign = WorldInstanceRegistry::Create(fixture.edits.Snapshot(), foreignBinding, pending.capture.requests, pending.capture.owners);
    Require(foreign.Succeeded() && !BindWorldPathingHelperVersion(captured, *fixture.registry, **foreign.value).Succeeded(),
        "source generation boundary cannot borrow helper bindings");
    unsigned reads = 0;
    const auto reader = pending.context.resourceReader;
    pending.context.resourceReader = [&](auto path, auto maximum) { ++reads; return reader(path, maximum); };
    ++pending.context.version.catalogRevision;
    const auto identity = WorldPathingHelperResourceIdentity(fixture.sources, pending.context.version.catalogRevision);
    Require(identity.Succeeded(), "changed catalog resource identity"); pending.context.resourceIdentity = *identity.value;
    const auto changedCatalog = fixture.Prepare(pending);
    Require(!changedCatalog.Succeeded() && reads == 1 && HasCode(changedCatalog.diagnostics, "WORLD_HELPER_INSTANCE_REBUILD_REQUIRED"),
        "changed catalog/source identity must recapture facts and cannot silently republish old resident assets");
}
void MissingAndReplacedSourcesCannotHideBehindHelperReuse() {
    for (bool sameReader : {false, true}) {
        DeltaFixture fixture(DeltaSource::MissingDisk, true);
        Require(fixture.prepared->unknownFootprints == 2 && !fixture.prepared->cache->resources.begin()->second.Succeeded(),
            "fixture really captured a missing high-priority disk source");
        fixture.AppendUnit(); auto pending = fixture.Next(); KeepResidentRenderOrder(fixture, pending);
        WriteFixtureBytes(fixture.diskTexture, Tga("fixture", 2, 3).bytes);
        if (sameReader) pending.reader = fixture.prepared->cache->sourceOwner;
        unsigned reads = 0; const auto actualReader = WorldPathingHelperResourceReader(pending.reader);
        pending.context.resourceReader = [&](auto path, auto maximum) { ++reads; return actualReader(path, maximum); };
        const auto result = fixture.Prepare(pending);
        Require(!result.Succeeded() && reads == 1 && HasCode(result.diagnostics, "WORLD_HELPER_INSTANCE_REBUILD_REQUIRED"),
            "missing source becoming available must actually requery and refuse stale unknown-footprint GPU geometry");
    }
    DeltaFixture fixture(DeltaSource::PositiveDisk, true); fixture.AppendUnit(); auto pending = fixture.Next();
    KeepResidentRenderOrder(fixture, pending);
    const auto held = fixture.diskTexture.parent_path() / L"retained-session.tga";
    std::filesystem::rename(fixture.diskTexture, held);
    WriteFixtureBytes(fixture.diskTexture, Tga("fixture", 3, 2).bytes);
    const auto result = fixture.Prepare(pending);
    Require(!result.Succeeded() && !fixture.prepared->cache->sourceOwner->VerifyCurrentSource().Succeeded(),
        "actual positive source path replacement invalidates reuse despite an unchanged archive SHA");
}
} // namespace
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication application(argc, argv);
    try { ActualFieldsAndWorkerCacheShareTheSameHelper(); MissingFootprintDoesNotInventNativeSizeOrRemapRows();
        DefaultClassificationStillChecksEachOwnerAndHelperRowIdentity();
        StableHelperBindingsKeepFullFieldAndResourceEvidence();
        IncrementalOwnersReuseFactsWithoutRelabellingRows(); CopyAndFieldChangesQueryOnlyAffectedHelpers();
        DeltaRejectsForeignMappingsAndKeepsRevisionNamespacesSeparate(); MissingAndReplacedSourcesCannotHideBehindHelperReuse();
        std::cout << "pathing helper capture/owned worker checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
