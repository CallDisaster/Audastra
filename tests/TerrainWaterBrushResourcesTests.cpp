#include "War3MapCore/MapData/TerrainPathingResources.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBudget = 8u * 1024u * 1024u;

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "operation failed";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Check(Status status) {
    if (!status.Succeeded()) {
        std::string message = "check failed";
        for (const auto& diagnostic : status.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
}
Bytes Text(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

class Directory {
public:
    Directory() {
        static std::atomic<unsigned> sequence{0};
        path = std::filesystem::temp_directory_path() / ("Audastra-water-brush-resources-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
        Require(std::filesystem::create_directory(path), "private directory");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};

Bytes GroundTable(std::string_view secondBuildable = "0") {
    // blightPri 特意与 buildable 相反，避免用显示优先级冒充荒芜资格。
    return Text("ID;PWXL;N;E\nB;X5;Y3;D0\n"
        "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\nC;X5;K\"blightPri\"\n"
        "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nC;X5;K0\n"
        "C;X1;Y3;K\"Tg02\"\nC;X2;K1\nC;X3;K1\nC;X4;K" + std::string(secondBuildable) + "\nC;X5;K1\nE\n");
}
Bytes WaterTable(std::string_view height = "0.25", std::string_view id = "LSha") {
    return Text("ID;PWXL;N;E\nB;X2;Y2;D0\nC;X1;Y1;K\"waterID\"\nC;X2;K\"height\"\n"
        "C;X1;Y2;K\"" + std::string(id) + "\"\nC;X2;K" + std::string(height) + "\nE\n");
}
Bytes CliffTable(bool second = true) {
    return Text("ID;PWXL;N;E\nB;X1;Y" + std::string(second ? "3" : "2") + ";D0\nC;X1;Y1;K\"cliffID\"\n"
        "C;X1;Y2;K\"Cl01\"\n" + (second ? "C;X1;Y3;K\"Cl02\"\n" : "") + "E\n");
}
Bytes Profile(std::string_view slope = "30", std::string_view minimum = "-256",
    std::string_view maximum = "1536", std::string_view deep = "64") {
    return Text("[Terrain]\nMaxSlope=" + std::string(slope) + "\nMinHeight=" + std::string(minimum)
        + "\nMaxHeight=" + std::string(maximum) + "\n[Water]\nDeepLevelPathing=" + std::string(deep) + "\n");
}

struct Fixture {
    TerrainGridSnapshot grid;
    std::vector<MapArchiveMember> members;
    Fixture() {
        grid.header.width = grid.header.height = 9;
        grid.header.mainTileset = 'L';
        grid.header.groundTilesets = {"Tg01", "Tg02"};
        grid.header.cliffTilesets = {"Cl01", "Cl02"};
        grid.vertices.resize(81);
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8192;
            vertex.waterHeightAndFlagsRaw = 8192;
            vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(grid, 0, 0, 9, 9);
        members = {{"war3map.w3e", Take(SerializeMapTerrain(grid))},
            {"TerrainArt/Terrain.slk", GroundTable()}, {"TerrainArt/Water.slk", WaterTable()},
            {"TerrainArt/CliffTypes.slk", CliffTable()}, {"UI/MiscData.txt", Profile()},
            {"UI/WorldEditData.txt", Text("[Unused]\nValue=1\n")}};
    }
    void Set(std::string name, Bytes bytes) {
        const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
        if (found != members.end()) found->data = std::move(bytes);
        else members.push_back({std::move(name), std::move(bytes)});
    }
    void Remove(std::string_view name) {
        std::erase_if(members, [&](const auto& member) { return member.name == name; });
    }
    MapArchiveSession Write(const std::filesystem::path& path) const {
        return Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(path, members)).path));
    }
    void WriteDirectory(const std::filesystem::path& root) const {
        Require(std::filesystem::create_directory(root), "map directory");
        for (const auto& member : members) {
            const auto path = root / member.name;
            std::filesystem::create_directories(path.parent_path());
            Check(IO::WriteFileBytes(path, member.data));
        }
    }
    std::size_t DecodedBytes() const {
        std::size_t total = 0;
        for (const auto& member : members) total += member.data.size();
        return total;
    }
};

std::filesystem::path GameRoot(const Directory& directory, std::vector<MapArchiveMember> members = {}) {
    const auto root = directory.path / "game";
    Require(std::filesystem::create_directory(root), "game directory");
    if (members.empty()) members.push_back({"unrelated.txt", Text("fixed core archive")});
    Take(WriteVerifiedArchive(root / "war3.mpq", members));
    return root;
}

void RulesUseActualTablesAndExplicitStrokeOption() {
    Directory directory; Fixture fixture; const auto game = GameRoot(directory);
    const auto source = fixture.Write(directory.path / "rules.w3x");
    const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget));
    const auto* rules = captured.WaterBrushRules();
    Require(rules != nullptr, "water rule capture must expose const typed rules");
    Require(rules->maxSlopeDegrees == 30.0f && rules->minHeight == -256.0f && rules->maxHeight == 1536.0f,
        "terrain rules must come from the actual profile scalar values");
    Require(rules->maximumCliffLevel == 14 && rules->waterLayerOffset == 32.0f && rules->deepWaterThreshold == 64.0f,
        "derive cliff maximum from binary32 height; scale only Water.height by 128");
    Require(rules->enforceWaterLimits, "explicit true stroke option");
    Require(rules->groundRules.size() == 2 && rules->availableCliffTilesets.size() == 2, "unused W3E palette entries need actual rows too");
    for (const auto& rule : rules->groundRules) {
        Require((rule.rawcode == "Tg01" && rule.permitsBlight) || (rule.rawcode == "Tg02" && !rule.permitsBlight),
            "blight permission uses actual buildable, not blightPri");
    }
    Require(std::find(rules->availableCliffTilesets.begin(), rules->availableCliffTilesets.end(), "Cl02")
        != rules->availableCliffTilesets.end(), "second cliff palette identity must be captured");
    Require(captured.SourceBytes() == fixture.DecodedBytes(), "source accounting includes actual W3E and every captured resource once");
    Check(captured.VerifyReadback(source, kBudget));
    Require(!captured.GenerateNative(fixture.grid, kBudget).Succeeded(), "editing rules cannot grant native pathing generation");
    Require(!captured.VerifyFrozenOutput({}, {}).Succeeded(), "editing rules cannot grant final map save qualification");
    const auto relaxed = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, false, kBudget));
    Require(relaxed.WaterBrushRules() && !relaxed.WaterBrushRules()->enforceWaterLimits, "explicit false option never uses user registry state");
}

void BeforeMustMatchActualSourceW3e() {
    Directory directory; Fixture fixture; const auto game = GameRoot(directory);
    const auto source = fixture.Write(directory.path / "source.w3x");
    auto invented = fixture.grid; invented.vertices[20].variationRaw = 1;
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, invented, game, true, kBudget).Succeeded(),
        "caller terrain cannot replace actual source W3E identity");
    const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget));
    const std::vector<MemberReplacement> replacements{{"war3map.w3e", Take(SerializeMapTerrain(invented))}};
    const auto altered = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / "altered.w3x", replacements)).path));
    Require(!captured.VerifyReadback(altered, kBudget).Succeeded(), "rule source recheck binds original W3E, not an arbitrary later edit");
    fixture.Remove("war3map.w3e");
    const auto missing = fixture.Write(directory.path / "missing.w3x");
    Require(!TerrainPathingResources::CaptureWaterBrushRules(missing, fixture.grid, game, true, kBudget).Succeeded(), "missing actual W3E must refuse");
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, {}, true, kBudget).Succeeded(),
        "missing fixed profiles need an actual supported game source to prove absence");
}

void FullProfileChainRejectsConflictsCsvAndGuardDependentValues() {
    Directory directory; const auto game = GameRoot(directory);
    const std::vector<std::pair<std::string, Bytes>> ambiguous{
        {"Units/MiscData.txt", Text("[Terrain]\nMaxSlope=31\n")},
        {"UI/WorldEditData.txt", Text("[Terrain]\nMaxHeight=1408\n")},
        {"UI/WorldEditGameStrings.txt", Text("[Water]\nDeepLevelPathing=48\n")},
        {"Units/NeutralUpgradeStrings.txt", Text("[Terrain]\nMinHeight=-300\n")},
        {"UI/MiscData.txt", Profile("30,30")},
        {"UI/MiscData.txt", Profile("30", "-256", "1536", "64,64")},
        {"UI/WorldEditData.txt", Text("[Terrain]\nMaxSlope=bad\nMaxSlope=30\n")}
    };
    for (std::size_t index = 0; index < ambiguous.size(); ++index) {
        Fixture fixture; fixture.Set(ambiguous[index].first, ambiguous[index].second);
        const auto source = fixture.Write(directory.path / ("conflict-" + std::to_string(index) + ".w3x"));
        Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget).Succeeded(),
            "all fixed-profile occurrences must remain single valid equal binary32 values");
    }
    Fixture same; same.Set("Units/MiscData.txt", Profile()); same.Set("UI/WorldEditData.txt", Profile());
    same.Set("Units/NeutralUpgradeStrings.txt", Profile());
    const auto identical = same.Write(directory.path / "identical.w3x");
    Require(TerrainPathingResources::CaptureWaterBrushRules(identical, same.grid, game, true, kBudget).Succeeded(),
        "same single values are invariant across both possible profile-load guards");
    Fixture guarded; guarded.Set("UI/MiscData.txt", Text("[Unused]\nValue=1\n")); guarded.Set("Units/MiscData.txt", Profile());
    const auto guardedSource = guarded.Write(directory.path / "guarded.w3x");
    Require(!TerrainPathingResources::CaptureWaterBrushRules(guardedSource, guarded.grid, game, true, kBudget).Succeeded(),
        "rules defined only in flag0 files cannot guess the original global guard");
}

void InvalidRulesAndUnprovedPaletteRowsRefuse() {
    Directory directory; const auto game = GameRoot(directory);
    const std::vector<std::pair<std::string, Bytes>> invalid{
        {"TerrainArt/Terrain.slk", GroundTable("2")},
        {"TerrainArt/Water.slk", WaterTable("nan")},
        {"TerrainArt/Water.slk", WaterTable("0.25", "ASha")},
        {"TerrainArt/CliffTypes.slk", CliffTable(false)},
        {"UI/MiscData.txt", Profile("91")},
        {"UI/MiscData.txt", Profile("30", "1600")},
        {"UI/MiscData.txt", Profile("30", "-256", "1664")},
        {"UI/MiscData.txt", Profile("30", "-256", "-400")},
        {"UI/MiscData.txt", Profile("30", "-256", "1536", "-1")}
    };
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        Fixture fixture; fixture.Set(invalid[index].first, invalid[index].second);
        const auto source = fixture.Write(directory.path / ("invalid-" + std::to_string(index) + ".w3x"));
        Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget).Succeeded(),
            "missing row, invalid field or out-of-domain rule cannot use guessed defaults");
    }
    Fixture fractional; fractional.Set("UI/MiscData.txt", Profile("30", "-256", "1471.75"));
    const auto source = fractional.Write(directory.path / "fractional-max.w3x");
    const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fractional.grid, game, true, kBudget));
    Require(captured.WaterBrushRules()->maximumCliffLevel == 13, "maximum cliff level is truncation of actual height, not constant 14 or rounding");
    Fixture rounded; rounded.Set("UI/MiscData.txt", Profile("30", "-256", "1535.99999"));
    const auto roundedSource = rounded.Write(directory.path / "binary32-max.w3x");
    const auto roundedRules = Take(TerrainPathingResources::CaptureWaterBrushRules(roundedSource, rounded.grid, game, true, kBudget));
    Require(roundedRules.WaterBrushRules()->maxHeight == 1536.0f && roundedRules.WaterBrushRules()->maximumCliffLevel == 14,
        "derive from the loaded binary32 scalar before double arithmetic, not the source decimal or direct double parse");
}

void MissingProfilesAndGameWinnersStayBound() {
    {
        Directory directory; Fixture fixture; const auto game = GameRoot(directory);
        const auto source = fixture.Write(directory.path / "source.w3x");
        const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget));
        const std::vector<MemberReplacement> override{{"Units/NeutralUpgradeStrings.txt", Text("[Unused]\nValue=1\n")}};
        const auto added = Take(MapArchiveSession::Open(Take(source.SaveCandidate(directory.path / "added.w3x", override)).path));
        Require(!captured.VerifyReadback(added, kBudget).Succeeded(), "new map override invalidates previously captured absence");
        Take(WriteVerifiedArchive(game / "war3patch.mpq", override));
        Require(!captured.VerifyReadback(source, kBudget).Succeeded(), "gameAbsent must recheck the complete fixed-profile set");
    }
    {
        Directory directory; Fixture fixture; fixture.Remove("TerrainArt/CliffTypes.slk");
        const std::vector<MapArchiveMember> resource{{"TerrainArt/CliffTypes.slk", CliffTable()}};
        const auto game = GameRoot(directory, resource);
        const auto source = fixture.Write(directory.path / "source.w3x");
        const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget));
        Require(captured.SourceBytes() == fixture.DecodedBytes() + resource.front().data.size(),
            "actual game fallback bytes share metadata and map resource source accounting");
        Check(captured.VerifyReadback(source, kBudget));
        Take(WriteVerifiedArchive(game / "war3patch.mpq", resource));
        Require(!captured.VerifyReadback(source, kBudget).Succeeded(), "identical bytes from a different winning archive are a changed source");
    }
}

void ExtraOverridesAndLooseSourcesRefuse() {
    Directory directory; const auto game = GameRoot(directory);
    Fixture fixture; fixture.Set("war3mapMisc.txt", Profile());
    const auto custom = fixture.Write(directory.path / "custom.w3x");
    Require(!TerrainPathingResources::CaptureWaterBrushRules(custom, fixture.grid, game, true, kBudget).Succeeded(),
        "unproved custom profile merge cannot be silently ignored");
    fixture.Remove("war3mapMisc.txt");
    const auto source = fixture.Write(directory.path / "source.w3x");
    const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget));
    const auto loose = game / "UI/MiscData.txt";
    std::filesystem::create_directories(loose.parent_path()); Check(IO::WriteFileBytes(loose, Profile()));
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget).Succeeded(),
        "same-name loose profile cannot be treated as a proved MPQ-only winner");
    Require(!captured.VerifyReadback(source, kBudget).Succeeded(), "new loose profile also invalidates a prior rule capture");
}

void DirectoryCaptureUsesVerifiedSourceAndRejectsUnknownLayout() {
    Directory directory; Fixture fixture; const auto game = GameRoot(directory);
    const auto root = directory.path / "map"; fixture.WriteDirectory(root);
    const auto workspace = Take(DirectoryWorkspace::Open(root));
    const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(workspace.Snapshot(), fixture.grid, game, true, kBudget));
    Check(captured.VerifyReadback(workspace.Snapshot(), kBudget));
    Check(IO::WriteFileBytes(root / "TerrainArt/Water.slk", WaterTable("0.5")));
    Require(!captured.VerifyReadback(workspace.Snapshot(), kBudget).Succeeded(), "old directory snapshot cannot authorize changed resource bytes");
    auto actual = Take(DirectoryWorkspace::Open(root));
    auto invented = fixture.grid; invented.vertices[10].variationRaw = 1;
    Require(!TerrainPathingResources::CaptureWaterBrushRules(actual.Snapshot(), invented, game, true, kBudget).Succeeded(),
        "directory capture also binds actual source W3E");
    const auto duplicate = root / "map/TerrainArt/Water.slk";
    std::filesystem::create_directories(duplicate.parent_path()); Check(IO::WriteFileBytes(duplicate, WaterTable("0.5")));
    actual = Take(DirectoryWorkspace::Open(root));
    Require(!TerrainPathingResources::CaptureWaterBrushRules(actual.Snapshot(), fixture.grid, game, true, kBudget).Succeeded(),
        "root and map/ copies cannot compete as unproved directory winners");
    Require(std::filesystem::remove(duplicate), "remove private duplicate fixture");
    const auto lni = root / "table/misc.ini"; std::filesystem::create_directories(lni.parent_path());
    Check(IO::WriteFileBytes(lni, Profile())); actual = Take(DirectoryWorkspace::Open(root));
    Require(!TerrainPathingResources::CaptureWaterBrushRules(actual.Snapshot(), fixture.grid, game, true, kBudget).Succeeded(),
        "unproved LNI configuration source cannot be skipped");
}

void RuleAndMetadataReadsShareCallerBudget() {
    Directory directory; Fixture fixture; const auto game = GameRoot(directory);
    const auto source = fixture.Write(directory.path / "source.w3x");
    const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, kBudget));
    Require(captured.SourceBytes() == fixture.DecodedBytes() && captured.SourceBytes() > 1, "all actual decoded inputs consume source budget");
    const auto insufficient = captured.SourceBytes() - 1;
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, insufficient).Succeeded(),
        "cumulative budget cannot separately admit W3E, tables and profile bytes");
    Require(!captured.VerifyReadback(source, insufficient).Succeeded(), "source recheck obeys the same cumulative caller budget");
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, game, true, 16).Succeeded(),
        "source W3E capture and identity serialization are bounded before returning any rule qualification");
}

void CurrentDraftHasIndependentInputIdentityAndActualPaletteRules() {
    Directory directory; Fixture fixture; const auto game = GameRoot(directory);
    // 磁盘palette不使用Tg03，但实际表已提供该行；当前草稿才能据此追加。
    fixture.Set("TerrainArt/Terrain.slk", Text("ID;PWXL;N;E\nB;X4;Y4;D0\n"
        "C;X1;Y1;K\"tileID\"\nC;X2;K\"walkable\"\nC;X3;K\"flyable\"\nC;X4;K\"buildable\"\n"
        "C;X1;Y2;K\"Tg01\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\n"
        "C;X1;Y3;K\"Tg02\"\nC;X2;K1\nC;X3;K1\nC;X4;K0\n"
        "C;X1;Y4;K\"Tg03\"\nC;X2;K1\nC;X3;K1\nC;X4;K1\nE\n"));
    const auto source = fixture.Write(directory.path / "source.w3x");
    auto current = fixture.grid;
    current.vertices[40].groundHeightRaw += 16;
    RefreshTerrainDerived(current, 0, 0, 9, 9);
    const auto captured = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, current, game, true, kBudget));
    const auto wire = Take(SerializeMapTerrain(current));
    Require(captured.WaterBrushInputSha256() == Take(Sha256(wire)) && captured.WaterBrushInputBytes() == wire.size(),
        "current draft identity comes from its canonical W3E, not the unchanged disk source");
    Require(captured.WaterBrushInputSha256() != Take(Sha256(fixture.members.front().data)), "prior height edit must change this stroke input identity");
    Check(captured.VerifyReadback(source, kBudget));
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, current, game, true, kBudget).Succeeded(),
        "single-snapshot overload still requires that snapshot to match the actual source");

    current.header.groundTilesets.push_back("Tg03");
    const auto appended = Take(TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, current, game, true, kBudget));
    const auto* rules = appended.WaterBrushRules();
    Require(rules && rules->groundRules.size() == 3 && std::any_of(rules->groundRules.begin(), rules->groundRules.end(),
        [](const auto& rule) { return rule.rawcode == "Tg03" && rule.permitsBlight; }),
        "actual rows must be captured for the current palette, including a slot absent from the disk W3E");
    const auto appendedWire = Take(SerializeMapTerrain(current));
    Require(appended.WaterBrushInputSha256() == Take(Sha256(appendedWire)) && appended.WaterBrushInputBytes() == appendedWire.size(),
        "palette edit participates in the frozen current input identity");
    const auto mapRoot = directory.path / "map"; fixture.WriteDirectory(mapRoot);
    const auto workspace = Take(DirectoryWorkspace::Open(mapRoot));
    const auto directoryRules = Take(TerrainPathingResources::CaptureWaterBrushRules(workspace.Snapshot(), fixture.grid, current, game, true, kBudget));
    Require(directoryRules.WaterBrushInputSha256() == appended.WaterBrushInputSha256()
        && directoryRules.WaterBrushInputBytes() == appended.WaterBrushInputBytes(),
        "directory adapter captures the same distinct source and current revisions");
    auto unavailable = current; unavailable.header.groundTilesets.push_back("Tg04");
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, unavailable, game, true, kBudget).Succeeded(),
        "current palette cannot add a rawcode whose actual resource row is missing");
    auto shifted = current; shifted.header.offset[0] += 128.0f;
    Require(!TerrainPathingResources::CaptureWaterBrushRules(source, fixture.grid, shifted, game, true, kBudget).Succeeded(),
        "current draft cannot change the source coordinate shape through rule capture");
}
} // namespace

int main() {
    try {
        RulesUseActualTablesAndExplicitStrokeOption();
        BeforeMustMatchActualSourceW3e();
        FullProfileChainRejectsConflictsCsvAndGuardDependentValues();
        InvalidRulesAndUnprovedPaletteRowsRefuse();
        MissingProfilesAndGameWinnersStayBound();
        ExtraOverridesAndLooseSourcesRefuse();
        DirectoryCaptureUsesVerifiedSourceAndRejectsUnknownLayout();
        RuleAndMetadataReadsShareCallerBudget();
        CurrentDraftHasIndependentInputIdentityAndActualPaletteRules();
        std::cout << "Terrain water brush resource tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
