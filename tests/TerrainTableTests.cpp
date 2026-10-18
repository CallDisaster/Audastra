#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/TerrainMaterials.h"
#include "War3MapCore/Resources/TerrainTable.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::Resources;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool Code(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& item) { return item.code == code; });
}
void Explain(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& item : diagnostics) std::cerr << item.code << ": " << item.message << '\n';
}
template <typename T>
void Rejected(const Result<T>& result, std::string_view code) {
    if (result.Succeeded() || result.value || !Code(result.diagnostics, code)) {
        Explain(result.diagnostics);
        throw std::runtime_error("unexpected capture/replay result: " + std::string(code));
    }
}
void Rejected(const Status& result, std::string_view code) {
    if (result.Succeeded() || !Code(result.diagnostics, code)) {
        Explain(result.diagnostics);
        throw std::runtime_error("unexpected verify result: " + std::string(code));
    }
}
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const auto character : text) bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}
std::string Quote(std::string_view text) {
    std::string quoted = "\"";
    for (const auto character : text) {
        quoted += character;
        if (character == '"') quoted += '"';
    }
    return quoted + '"';
}
std::string Slk(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const auto& row, std::size_t y) {
        for (std::size_t x = 0; x < row.size(); ++x)
            text += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y) + ";K" + Quote(row[x]) + '\n';
    };
    append(headers, 1);
    for (std::size_t y = 0; y < rows.size(); ++y) append(rows[y], y + 2);
    return text + "E\n";
}
std::string OriginalTable() {
    // 原创表保留大小写不同的rawcode、引号/分号以及尚未解释的业务列。
    return Slk({"tileID", "Name", "futureFlag"},
        {{"Q001", "stone;\"quoted\"", "unknown value"}, {"q001", "moss", "_"}});
}
struct Backing {
    std::vector<std::byte> bytes = Bytes(OriginalTable());
    std::string canonicalPath = "TERRAINART\\TERRAIN.SLK";
    std::string actualPath = "Original/Terrain.slk";
    bool present = true, failRead = false, failProbe = false, conflict = false;
    unsigned probes = 0, reads = 0;
};
ResourceSource Source(std::string id, ResourceLayer layer, int priority, const std::shared_ptr<Backing>& backing) {
    ResourceSource source;
    source.id = std::move(id); source.layer = layer; source.priority = priority;
    source.probe = [backing](std::string_view path) {
        ++backing->probes;
        if (backing->failProbe) return Result<ResourceProbe>::Failure("Fixture", "FIXTURE_PROBE_FAILED", "原创查询故障");
        if (!backing->present || path != backing->canonicalPath) return Result<ResourceProbe>::Success({});
        return Result<ResourceProbe>::Success({backing->conflict ? ResourceProbeState::Conflict : ResourceProbeState::Found,
            backing->actualPath});
    };
    source.read = [backing](std::string_view path) {
        ++backing->reads;
        Require(path == backing->actualPath, "only the located table can be read; no BLP or lower-source substitution");
        if (backing->failRead) return Result<std::vector<std::byte>>::Failure("Fixture", "FIXTURE_READ_FAILED", "原创读取故障");
        return Result<std::vector<std::byte>>::Success(backing->bytes);
    };
    return source;
}
struct Fixture {
    std::shared_ptr<Backing> backing = std::make_shared<Backing>();
    ResourceResolver resolver;
    Fixture() {
        Require(resolver.AddSource(Source("original-base", ResourceLayer::Game,
            ResourcePriority::GameBase, backing)).Succeeded(), "register original table source");
    }
    TerrainTableSnapshot Capture(const TerrainTableLimits& limits = {}) const {
        auto captured = CaptureTerrainTable(resolver, "TerrainArt/Terrain.slk", "tileid", limits);
        if (!captured.Succeeded()) Explain(captured.diagnostics);
        Require(captured.Succeeded(), "capture original source through public service");
        return std::move(*captured.value);
    }
};
bool SameLocation(const ResourceLocation& a, const ResourceLocation& b) {
    return a.canonicalPath == b.canonicalPath && a.layer == b.layer && a.priority == b.priority
        && a.sourceId == b.sourceId && a.sourcePath == b.sourcePath
        && ((!a.readProvenance && !b.readProvenance)
            || (a.readProvenance && b.readProvenance && *a.readProvenance == *b.readProvenance));
}
bool SameRows(const std::vector<TerrainTableRow>& a, const std::vector<TerrainTableRow>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& first, const auto& second) {
        return first.id == second.id && first.row == second.row && first.fields == second.fields
            && SameLocation(first.table, second.table);
    });
}

void CapturesOriginalBytesAndReplaysWithoutSources() {
    Fixture fixture;
    fixture.backing->bytes = Bytes(std::string("\xEF\xBB\xBF") + OriginalTable());
    const auto original = fixture.backing->bytes;
    const auto snapshot = fixture.Capture();
    Require(snapshot.Rows().size() == 2 && snapshot.Rows()[0].id == "Q001" && snapshot.Rows()[1].id == "q001",
        "rawcode identity is case-sensitive");
    Require(snapshot.Rows()[0].row == 2 && snapshot.Rows()[1].row == 3
        && snapshot.Rows()[0].fields.at("name") == "stone;\"quoted\""
        && snapshot.Rows()[0].fields.at("futureflag") == "unknown value", "row numbers, escaping and unknown values are retained");
    Require(std::equal(snapshot.Bytes().begin(), snapshot.Bytes().end(), original.begin(), original.end()),
        "capture owns the original BOM and every source byte");
    const auto hash = Sha256(original);
    Require(hash.Succeeded() && *hash.value == snapshot.SourceSha256() && snapshot.IdentitySha256().size() == 64,
        "source SHA comes from actual owned original bytes");
    Require(snapshot.IdColumn() == "tileid" && snapshot.Location().canonicalPath == "TERRAINART\\TERRAIN.SLK"
        && snapshot.Location().sourcePath == "Original/Terrain.slk"
        && snapshot.Location().sourceId == "original-base" && snapshot.Location().layer == ResourceLayer::Game
        && snapshot.Location().priority == ResourcePriority::GameBase, "complete resolved source location is retained");
    for (const auto& row : snapshot.Rows()) Require(SameLocation(row.table, snapshot.Location()), "each row binds the captured table source");
    Require(fixture.backing->reads == 1, "capture only reads the requested table once");
    Require(VerifyTerrainTableSnapshot(snapshot, fixture.resolver).Succeeded() && fixture.backing->reads == 2,
        "verify actually reads the current source");

    fixture.backing->bytes.clear();
    fixture.backing->failRead = true; fixture.backing->failProbe = true;
    fixture.resolver.ClearSources();
    const auto probes = fixture.backing->probes, reads = fixture.backing->reads;
    const auto replay = ReplayTerrainTable(snapshot);
    Require(replay.Succeeded() && SameRows(*replay.value, snapshot.Rows())
        && fixture.backing->probes == probes && fixture.backing->reads == reads,
        "memory replay succeeds after the resolver and source are gone without any callbacks");
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "RESOURCE_NOT_FOUND");
}

void KeepsStrictParserAndTableRules() {
    Fixture fixture;
    const auto reject = [&](std::string text, std::string_view code) {
        fixture.backing->bytes = Bytes(text);
        Rejected(CaptureTerrainTable(fixture.resolver, "TerrainArt/Terrain.slk", "tileid"), code);
    };
    reject("invalid\n", "slk-header");
    reject("ID;PWXL\nC;X1;Y1;K\"tileID\"\n", "slk-incomplete");
    reject("ID;PWXL\nC;X0;Y1;K\"tileID\"\nE\n", "slk-coordinate");
    reject(Slk({"wrongID"}, {{"Q001"}}), "TERRAIN_TABLE_HEADER");
    reject(Slk({"tileID", "Name", "NAME"}, {{"Q001", "a", "b"}}), "TERRAIN_TABLE_HEADER");
    reject(Slk({"tileID", ""}, {{"Q001", "a"}}), "TERRAIN_TABLE_HEADER");
    reject(Slk({"tileID", "name"}, {{"Q001", "a", "undeclared"}}), "TERRAIN_TABLE_HEADER");
    reject(Slk({"tileID"}, {{"Q001"}, {"Q001"}}), "TERRAIN_TABLE_DUPLICATE");
    reject(Slk({"tileID"}, {{"Q01"}}), "TERRAIN_TABLE_ID");
    reject(Slk({"tileID"}, {{"Q0001"}}), "TERRAIN_TABLE_ID");
    reject(Slk({"tileID"}, {{"Q 01"}}), "TERRAIN_TABLE_ID");
    reject(Slk({"tileID"}, {{std::string("Q\x80") + "01"}}), "TERRAIN_TABLE_ID");
    reject("ID;PWXL\nC;X1;Y1;K\"tileID\"\nC;X2;K\"name\"\nC;X2;Y2;K\"missing id\"\nE\n",
        "TERRAIN_TABLE_ID");
    fixture.backing->bytes = Bytes(OriginalTable());
    Rejected(CaptureTerrainTable(fixture.resolver, "TerrainArt/Terrain.slk", "cliffid"), "TERRAIN_TABLE_HEADER");
    const auto reads = fixture.backing->reads;
    Rejected(CaptureTerrainTable(fixture.resolver, "../Terrain.slk", "tileid"), "RESOURCE_PATH_UNSAFE");
    Require(fixture.backing->reads == reads, "unsafe virtual path is rejected before reading");
    fixture.backing->bytes = Bytes("ID;PWXL\nC;X1;Y1;K\"tileID\"\nC;X2;K\"Name\"\n"
        "C;X1;Y9;K\"Q001\"\nC;X2;K\"sparse\"\nE\n");
    const auto sparse = fixture.Capture();
    Require(sparse.Rows().size() == 1 && sparse.Rows().front().row == 9
        && sparse.Rows().front().fields.at("name") == "sparse", "sparse SYLK row identity and inherited coordinates are preserved");
}

void BudgetsOnlyTightenAndArePartOfIdentity() {
    Fixture fixture;
    const auto baseline = fixture.Capture();
    TerrainTableLimits exact;
    exact.maxTableBytes = fixture.backing->bytes.size(); exact.maxRows = 2; exact.maxCells = 9; exact.maxCellBytes = 14;
    const auto bounded = fixture.Capture(exact);
    Require(SameRows(baseline.Rows(), bounded.Rows()) && baseline.SourceSha256() == bounded.SourceSha256()
        && baseline.IdentitySha256() != bounded.IdentitySha256(), "same content under a different budget has a different capture identity");
    Require(bounded.Limits().maxTableBytes == exact.maxTableBytes && bounded.Limits().maxRows == 2
        && bounded.Limits().maxCells == 9 && bounded.Limits().maxCellBytes == 14, "all requested budgets are frozen");
    using Limit = std::size_t TerrainTableLimits::*;
    const std::array<Limit, 4> members{&TerrainTableLimits::maxTableBytes, &TerrainTableLimits::maxRows,
        &TerrainTableLimits::maxCells, &TerrainTableLimits::maxCellBytes};
    for (const auto member : members) {
        auto shortBudget = exact;
        --(shortBudget.*member);
        Rejected(CaptureTerrainTable(fixture.resolver, "TerrainArt/Terrain.slk", "tileid", shortBudget), "TERRAIN_TABLE_BUDGET");
        TerrainTableLimits tightened;
        --(tightened.*member);
        Require(fixture.Capture(tightened).IdentitySha256() != baseline.IdentitySha256(), "each budget independently contributes to identity");
        TerrainTableLimits expanded;
        ++(expanded.*member);
        const auto reads = fixture.backing->reads;
        Rejected(CaptureTerrainTable(fixture.resolver, "TerrainArt/Terrain.slk", "tileid", expanded), "TERRAIN_TABLE_LIMITS");
        Require(fixture.backing->reads == reads, "expanded hard budget rejects before source access");
    }
    fixture.backing->bytes = Bytes(Slk({"tileID"}, {}));
    TerrainTableLimits noRows; noRows.maxRows = 0;
    Require(fixture.Capture(noRows).Rows().empty(), "a valid header-only table still has zero data rows");
}

void ActualByteChangesInvalidateEvenEquivalentRows() {
    Fixture fixture;
    const auto snapshot = fixture.Capture();
    fixture.backing->bytes.push_back(std::byte{'\n'});
    const auto current = fixture.Capture();
    Require(SameRows(snapshot.Rows(), current.Rows()) && snapshot.SourceSha256() != current.SourceSha256(),
        "semantically ignored newline remains a real source-byte change");
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "TERRAIN_TABLE_SOURCE_CHANGED");
    const auto replay = ReplayTerrainTable(snapshot);
    Require(replay.Succeeded() && SameRows(*replay.value, snapshot.Rows()), "changed disk bytes do not replace captured bytes during replay");
    fixture.backing->bytes = Bytes("present but broken SLK");
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "slk-header");
}

void EqualBytesRequireTheSameFullLocation() {
    Fixture fixture;
    const auto snapshot = fixture.Capture();
    struct Case { const char* id; ResourceLayer layer; int priority; const char* actualPath; };
    const std::array<Case, 4> changes{{
        {"other-base", ResourceLayer::Game, ResourcePriority::GameBase, "Original/Terrain.slk"},
        {"original-base", ResourceLayer::Other, ResourcePriority::GameBase, "Original/Terrain.slk"},
        {"original-base", ResourceLayer::Game, ResourcePriority::GameBase + 1, "Original/Terrain.slk"},
        {"original-base", ResourceLayer::Game, ResourcePriority::GameBase, "Other/Terrain.slk"}}};
    for (const auto& change : changes) {
        ResourceResolver current;
        auto backing = std::make_shared<Backing>(); backing->actualPath = change.actualPath;
        Require(current.AddSource(Source(change.id, change.layer, change.priority, backing)).Succeeded(), "register changed source identity");
        Rejected(VerifyTerrainTableSnapshot(snapshot, current), "TERRAIN_TABLE_SOURCE_CHANGED");
        const auto captured = CaptureTerrainTable(current, "TerrainArt/Terrain.slk", "tileid");
        Require(captured.Succeeded() && captured.value->SourceSha256() == snapshot.SourceSha256()
            && captured.value->IdentitySha256() != snapshot.IdentitySha256(), "every location field affects capture identity despite equal bytes");
    }
    fixture.backing->canonicalPath = "TERRAINART\\COPY.SLK";
    const auto other = CaptureTerrainTable(fixture.resolver, "TerrainArt/Copy.slk", "tileid");
    Require(other.Succeeded() && other.value->SourceSha256() == snapshot.SourceSha256()
        && other.value->IdentitySha256() != snapshot.IdentitySha256(), "canonical virtual path is also part of identity");
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "RESOURCE_NOT_FOUND");
}

void ActualReadWinnerIsValueIdentityAndSharedAcrossRows() {
    Fixture fixture;
    const auto legacy = fixture.Capture();
    const auto advertised = std::make_shared<ResourceReadProvenance>(ResourceReadProvenance{
        ResourceReadOrigin::GameArchive, "war3patch.mpq", "TerrainArt/Terrain.slk", "fixture-game-root"});
    auto source = Source("original-base", ResourceLayer::Game, ResourcePriority::GameBase, fixture.backing);
    source.readWithProvenance = [backing = fixture.backing, advertised](std::string_view path) {
        Require(path == backing->actualPath, "rich table read retains the probe's actual read key");
        ++backing->reads;
        return Result<ResourceReadResult>::Success({backing->bytes, *advertised});
    };
    fixture.resolver.ClearSources();
    Require(fixture.resolver.AddSource(std::move(source)).Succeeded(), "register actual table read provenance");
    const auto snapshot = fixture.Capture();
    Require(snapshot.Location().readProvenance && !legacy.Location().readProvenance
        && snapshot.SourceSha256() == legacy.SourceSha256() && snapshot.IdentitySha256() != legacy.IdentitySha256(),
        "unknown legacy and known actual winner are distinct despite identical table bytes");
    Rejected(VerifyTerrainTableSnapshot(legacy, fixture.resolver), "TERRAIN_TABLE_SOURCE_CHANGED");
    for (const auto& row : snapshot.Rows())
        Require(row.table.readProvenance == snapshot.Location().readProvenance,
            "all table rows share one immutable provenance instead of copying its strings");
    const auto equivalent = fixture.Capture();
    Require(equivalent.Location().readProvenance != snapshot.Location().readProvenance
        && equivalent.IdentitySha256() == snapshot.IdentitySha256()
        && VerifyTerrainTableSnapshot(snapshot, fixture.resolver).Succeeded(),
        "equal provenance values in different owned objects have the same source identity");

    const auto original = *advertised;
    for (unsigned field = 0; field < 4; ++field) {
        *advertised = original;
        if (field == 0) advertised->origin = ResourceReadOrigin::MapArchive;
        if (field == 1) advertised->containerIdentity = "war3.mpq";
        if (field == 2) advertised->memberPath = "Other/Terrain.slk";
        if (field == 3) advertised->sourceContext = "other-game-root";
        Require(*snapshot.Location().readProvenance == original,
            "callback mutation cannot change the completed immutable winner");
        const auto changed = fixture.Capture();
        Require(changed.SourceSha256() == snapshot.SourceSha256() && changed.IdentitySha256() != snapshot.IdentitySha256(),
            "every actual read provenance field participates in table identity");
        Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "TERRAIN_TABLE_SOURCE_CHANGED");
    }
    const auto replay = ReplayTerrainTable(snapshot);
    Require(replay.Succeeded() && SameRows(*replay.value, snapshot.Rows()),
        "memory replay retains the original read winner after source mutation");
    fixture.resolver.ClearSources();
    Require(fixture.resolver.AddSource(Source("original-base", ResourceLayer::Game,
        ResourcePriority::GameBase, fixture.backing)).Succeeded(), "restore legacy-only source");
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "TERRAIN_TABLE_SOURCE_CHANGED");
}

void ShadowsAndBadHigherSourcesNeverFallBack() {
    Fixture fixture;
    const auto snapshot = fixture.Capture();
    const auto lowReads = fixture.backing->reads;
    auto upper = std::make_shared<Backing>();
    Require(fixture.resolver.AddSource(Source("map-override", ResourceLayer::MapImport,
        ResourcePriority::MapArchive, upper)).Succeeded(), "register same-byte upper table");
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "TERRAIN_TABLE_SOURCE_CHANGED");
    Require(upper->reads == 1 && fixture.backing->reads == lowReads, "same-byte shadowing uses the real upper source");
    upper->bytes = Bytes("broken higher table");
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "slk-header");
    upper->failRead = true;
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "RESOURCE_READ_FAILED");
    upper->failRead = false; upper->failProbe = true;
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "RESOURCE_PROBE_FAILED");
    upper->failProbe = false; upper->conflict = true;
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "RESOURCE_CONFLICT");
    Require(fixture.backing->reads == lowReads, "bad, unreadable and conflicting higher sources never fall back");
    upper->conflict = false; upper->bytes = Bytes(OriginalTable());
    auto sibling = std::make_shared<Backing>();
    Require(fixture.resolver.AddSource(Source("map-sibling", ResourceLayer::MapImport,
        ResourcePriority::MapArchive, sibling)).Succeeded(), "register conflicting same-priority source");
    const auto reads = upper->reads;
    Rejected(VerifyTerrainTableSnapshot(snapshot, fixture.resolver), "RESOURCE_CONFLICT");
    Require(upper->reads == reads && sibling->reads == 0 && fixture.backing->reads == lowReads,
        "same-priority ambiguity rejects before reading any candidate");
}

void RenderAdapterKeepsItsExistingDiagnostics() {
    Fixture fixture;
    fixture.backing->bytes = Bytes(Slk({"tileID"}, {{"Q001"}, {"Q001"}}));
    Rejected(Render::LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_DUPLICATE");
    fixture.backing->bytes = Bytes(Slk({"tileID", "NAME", "name"}, {{"Q001", "a", "b"}}));
    Rejected(Render::LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_HEADER");
    fixture.backing->bytes = Bytes(Slk({"tileID"}, {{"wrong"}}));
    Rejected(Render::LoadTerrainMaterialCatalog(fixture.resolver), "TERRAIN_MATERIAL_ID");
    fixture.backing->bytes = Bytes(OriginalTable());
    Render::TerrainMaterialLimits limits; limits.maxTableBytes = 1;
    Rejected(Render::LoadTerrainMaterialCatalog(fixture.resolver, limits), "TERRAIN_MATERIAL_TABLE_BUDGET");
    fixture.backing->bytes = Bytes("invalid table");
    Rejected(Render::LoadTerrainMaterialCatalog(fixture.resolver), "slk-header");
    fixture.backing->failRead = true;
    Rejected(Render::LoadTerrainMaterialCatalog(fixture.resolver), "RESOURCE_READ_FAILED");
}
} // namespace

int main() {
    try {
        CapturesOriginalBytesAndReplaysWithoutSources();
        KeepsStrictParserAndTableRules();
        BudgetsOnlyTightenAndArePartOfIdentity();
        ActualByteChangesInvalidateEvenEquivalentRows();
        EqualBytesRequireTheSameFullLocation();
        ActualReadWinnerIsValueIdentityAndSharedAcrossRows();
        ShadowsAndBadHigherSourcesNeverFallBack();
        RenderAdapterKeepsItsExistingDiagnostics();
        std::cout << "terrain-table: 8 scenario groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "terrain-table: " << error.what() << '\n';
        return 1;
    }
}
