#include "War3MapCore/Services/MapObjectReferenceCapture.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/GeneratedObjectSymbol.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using SnapshotResult = Result<std::shared_ptr<const Trigger::MapObjectReferenceSnapshot>>;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) { return diagnostic.code == code; });
}

void RequireFailure(const SnapshotResult& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "failure must not expose a partial reference snapshot");
    Require(HasCode(result.diagnostics, code), "expected rejection diagnostic missing");
}

std::vector<std::byte> TextBytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const unsigned char value : text) result.push_back(static_cast<std::byte>(value));
    return result;
}

struct Binary {
    std::vector<std::byte> bytes;
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
    void Id(std::string_view value) {
        Require(value.size() == 4, "synthetic ID must have exactly four bytes");
        for (const unsigned char character : value) bytes.push_back(static_cast<std::byte>(character));
    }
};

// 原创真实native表；允许刻意重复记录，让产品解析器决定是否取得资格。
std::vector<std::byte> Native(std::initializer_list<std::string_view> originals,
    std::initializer_list<std::pair<std::string_view, std::string_view>> custom = {}, std::uint32_t version = 2) {
    Binary writer;
    writer.U32(version);
    writer.U32(static_cast<std::uint32_t>(originals.size()));
    for (const auto original : originals) {
        writer.Id(original); writer.U32(0);
        if (version == 3) writer.U32(0); // 已知边界的v3未知整数段为空。
        writer.U32(0);
    }
    writer.U32(static_cast<std::uint32_t>(custom.size()));
    for (const auto& [child, parent] : custom) {
        writer.Id(parent); writer.Id(child);
        if (version == 3) writer.U32(0);
        writer.U32(0);
    }
    return std::move(writer.bytes);
}

std::vector<std::byte> Slk(std::initializer_list<std::string_view> rawcodes) {
    std::string text = "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"name\"\n";
    unsigned row = 2;
    for (const auto rawcode : rawcodes) {
        text += "C;X1;Y" + std::to_string(row++) + ";K\"" + std::string(rawcode) + "\"\nC;X2;K\"Synthetic\"\n";
    }
    text += "E\n";
    return TextBytes(text);
}

MapData::UnitPlacement Placed(std::string rawcode, std::int32_t creationId) {
    MapData::UnitPlacement result;
    result.rawcode = std::move(rawcode);
    result.scale = {1.f, 1.f, 1.f};
    result.creationId = creationId;
    result.flags = 2;
    // 真实经典固定记录也有mode=0默认载荷，不能只用人工mode=-1测正例。
    result.random = MapData::RandomAnyUnit{1, 0};
    return result;
}

std::vector<std::byte> UnitBytes(std::vector<MapData::UnitPlacement> records) {
    MapData::UnitPlacements placements;
    placements.units = std::move(records);
    auto serialized = placements.Serialize();
    Require(serialized.Succeeded(), "original placement fixture must serialize through the public codec");
    return std::move(*serialized.value);
}

MapDocument Map(std::vector<MapData::UnitPlacement> records, std::string_view source = "synthetic-map.w3x") {
    const std::array<MapArchiveMember, 1> members{{{"war3mapUnits.doo", UnitBytes(std::move(records))}}};
    return MapDocument::FromMembers(members, std::filesystem::path(std::u8string(source.begin(), source.end())));
}

struct Sources {
    std::map<std::string, std::vector<std::byte>> files;
    std::map<std::string, std::string> sourceOverrides;
    std::vector<std::string> requested;
    std::string rejected;
    std::string prefix = "owned/";

    MapObjectReferenceCapture::Reader Reader() {
        return [this](std::string_view name) -> Result<MapObjectReferenceSource> {
            requested.emplace_back(name);
            if (name == rejected)
                return Result<MapObjectReferenceSource>::Failure("Test.Source", "TEST_SOURCE_SHADOW_CONFLICT",
                    "High priority source is ambiguous; lower bytes must not be consumed.");
            const std::string logical(name);
            const auto source = sourceOverrides.find(logical);
            MapObjectReferenceSource result{source == sourceOverrides.end() ? prefix + logical : source->second, {}};
            if (const auto found = files.find(logical); found != files.end()) result.bytes = found->second;
            return Result<MapObjectReferenceSource>::Success(std::move(result));
        };
    }
};

void PositiveOwnedInputsAndOldVersions() {
    auto withDrops = Placed("n00Q", 17);
    withDrops.droppedItemTable = 4; // 资格捕获不以完整Lua初始化/掉落后端为前提。
    auto map = Map({withDrops, Placed("I001", 3), Placed("hfoo", 12345), Placed("zzzz", 9)});
    Sources native, standard;
    native.files["war3map.w3u"] = Native({}, {{"n00Q", "NOPE"}}, 1);
    native.files["war3map.w3t"] = Native({}, {{"I001", "MISS"}}, 1);
    standard.files["Units/UnitData.slk"] = Slk({"hfoo", "n00Q"});
    standard.files["Units/ItemData.slk"] = Slk({"ratf"});
    const auto result = MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader());
    Require(result.Succeeded(), "native v1 read-only warning must not reject positive category evidence");
    const auto& snapshot = **result.value;
    Require(snapshot.References().size() == 3 && snapshot.Inputs().size() == 5, "three qualified references and five owned slots");
    const auto* unit = snapshot.Find("gg_unit_n00Q_0017");
    Require(unit && unit->type == "unit" && unit->rawcode == "n00Q" && unit->creationId == 17
        && unit->sourceIndex == 0 && unit->memberName == "war3mapUnits.doo"
        && unit->memberSha256 == snapshot.Inputs()[0].sha256, "real preplaced unit retains its member and row identity");
    Require(snapshot.Identity().size() == 64, "snapshot has a complete content identity");
    Require(snapshot.Find("gg_item_I001_0003") && snapshot.Find("gg_unit_hfoo_12345"), "positive item and five-digit creation number");
    Require(!snapshot.Find("gg_unit_I001_0003") && !snapshot.Find("gg_unit_zzzz_0009"), "no item-to-unit or unknown-to-unit inference");
    Require(HasCode(snapshot.Diagnostics(), "native-v1-unit-read-only")
        && HasCode(snapshot.Diagnostics(), "MAP_OBJECT_REFERENCE_CLASSIFICATION_UNKNOWN"), "read-only and unknown diagnostics remain separate");
    Require(native.requested == std::vector<std::string>{"war3map.w3u", "war3map.w3t"}
        && standard.requested == std::vector<std::string>{"Units/UnitData.slk", "Units/ItemData.slk"},
        "capture reads only the four contracted inputs, without profiles or strings");
    for (const auto& input : snapshot.Inputs()) {
        const auto hash = Sha256(input.bytes);
        Require(input.present && hash.Succeeded() && input.sha256 == *hash.value, "every present input hash comes from owned actual bytes");
    }
    const auto ownedBytes = snapshot.Inputs()[1].bytes;
    native.files["war3map.w3u"] = Native({"xxxx"});
    Require(snapshot.Inputs()[1].bytes == ownedBytes && snapshot.Find("gg_unit_n00Q_0017"), "later provider mutations cannot alter the frozen snapshot");
    Require(std::is_sorted(snapshot.References().begin(), snapshot.References().end(), [](const auto& a, const auto& b) {
        return a.symbol < b.symbol;
    }), "reference index is sorted by exact generated symbol");

    Sources v3;
    v3.files["war3map.w3u"] = Native({"hfoo"}, {}, 3);
    const auto version3 = MapObjectReferenceCapture::Capture(Map({Placed("hfoo", 1)}), v3.Reader());
    Require(version3.Succeeded() && (*version3.value)->Find("gg_unit_hfoo_0001"), "known native v3 record boundaries provide category facts only");
}

void MissingAndUnknownDoNotBecomeUnit() {
    Sources missing;
    const auto map = Map({Placed("hfoo", 1)});
    const auto result = MapObjectReferenceCapture::Capture(map, missing.Reader());
    Require(result.Succeeded() && (*result.value)->References().empty(), "unknown rawcode has no positive qualification");
    const auto& inputs = (*result.value)->Inputs();
    Require(!inputs[1].present && inputs[1].bytes.empty() && inputs[1].sha256.empty()
        && inputs[3].source.starts_with("unconfigured:"), "missing source and unconfigured standard source are explicit");
    Require(HasCode(result.diagnostics, "MAP_OBJECT_REFERENCE_CLASSIFICATION_MISSING")
        && HasCode(result.diagnostics, "MAP_OBJECT_REFERENCE_CLASSIFICATION_UNKNOWN"), "missing category input is not a positive class");
    const auto emptyMap = MapDocument::FromMembers({});
    const auto absent = MapObjectReferenceCapture::Capture(emptyMap, missing.Reader());
    Require(absent.Succeeded() && (*absent.value)->References().empty() && !(*absent.value)->Inputs()[0].present,
        "WTG-only document may capture an empty external-reference context");
    RequireFailure(MapObjectReferenceCapture::Capture(map, {}), "MAP_OBJECT_REFERENCE_READER_MISSING");
    missing.files["war3map.w3u"] = {};
    RequireFailure(MapObjectReferenceCapture::Capture(map, missing.Reader()), "MAP_OBJECT_REFERENCE_NATIVE_UNQUALIFIED");
}

void PlacementAmbiguityAndUnsupportedRecords() {
    Sources native;
    native.files["war3map.w3u"] = Native({"n00Q", "hfoo", "sloc", "uDNR", "n!QX"});
    auto random = Placed("uDNR", 22);
    const auto map = Map({Placed("n00Q", 17), Placed("n00Q", 18), Placed("hfoo", 18),
        Placed("sloc", 20), random, Placed("n!QX", 23), Placed("n00Q", -1),
        Placed("n00Q", 24), Placed("n00Q", 25), Placed("n00Q", 19), Placed("n00Q", 19)});
    const auto result = MapObjectReferenceCapture::Capture(map, native.Reader());
    Require(result.Succeeded(), "known duplicate placement identities can be excluded without authorizing either");
    const auto& snapshot = **result.value;
    Require(snapshot.References().size() == 3 && snapshot.Find("gg_unit_n00Q_0017")
        && snapshot.Find("gg_unit_n00Q_0024") && snapshot.Find("gg_unit_n00Q_0025"), "only unique ordinary placements qualify");
    Require(!snapshot.Find("gg_unit_n00Q_0018") && !snapshot.Find("gg_unit_hfoo_0018")
        && !snapshot.Find("gg_unit_n00Q_0019"), "all members of duplicate identity/symbol groups lose qualification");
    Require(HasCode(result.diagnostics, "MAP_OBJECT_REFERENCE_PLACEMENT_UNSUPPORTED")
        && HasCode(result.diagnostics, "MAP_OBJECT_REFERENCE_SYMBOL_INVALID"), "random/start-location and unsafe generated name diagnostics");
    const auto bytes = UnitBytes({Placed("hfoo", 1)});
    const std::array<MapArchiveMember, 2> shadows{{{"war3mapUnits.doo", bytes}, {"WAR3MAPUNITS.DOO", bytes}}};
    RequireFailure(MapObjectReferenceCapture::Capture(MapDocument::FromMembers(shadows), native.Reader()),
        "MAP_OBJECT_REFERENCE_MEMBER_UNQUALIFIED");
    const std::array<MapArchiveMember, 1> broken{{{"war3mapUnits.doo", {std::byte{0}}}}};
    RequireFailure(MapObjectReferenceCapture::Capture(MapDocument::FromMembers(broken), native.Reader()),
        "MAP_OBJECT_REFERENCE_MEMBER_UNQUALIFIED");
}

void FixedDefaultModeAndActualRandomObjects() {
    Sources native;
    native.files["war3map.w3u"] = Native({"n00Q", "hfoo", "Iunt", "uDNR", "bDNR", "sloc"});
    native.files["war3map.w3t"] = Native({"I001", "hItm", "iDNR"});
    auto legacy = Placed("hfoo", 18);
    legacy.random = std::monostate{};
    std::vector<MapData::UnitPlacement> records{
        Placed("n00Q", 17), legacy, Placed("I001", 19), Placed("Iunt", 20), Placed("hItm", 21)};
    std::int32_t creation = 30;
    // 真实随机占位即使目录提供类别、甚至声称mode=-1，也不能生成固定引用。
    for (const auto rawcode : {"uDNR", "bDNR", "iDNR", "sloc"}) {
        for (const auto& mode : std::array<MapData::RandomUnitConfig, 4>{
                 std::monostate{}, MapData::RandomAnyUnit{1, 0},
                 MapData::RandomGlobalTable{0, 0}, MapData::RandomCustomTable{}}) {
            auto value = Placed(rawcode, creation++);
            value.random = mode;
            records.push_back(std::move(value));
        }
    }
    // 普通ID配上全局/自定义表，或未证明为固定默认值的mode=0，不猜其运行语义。
    for (const auto& mode : std::array<MapData::RandomUnitConfig, 5>{
             MapData::RandomAnyUnit{-1, 0}, MapData::RandomAnyUnit{2, 0}, MapData::RandomAnyUnit{1, 1},
             MapData::RandomGlobalTable{0, 0}, MapData::RandomCustomTable{}}) {
        auto value = Placed("n00Q", creation++);
        value.random = mode;
        records.push_back(std::move(value));
    }
    const auto map = Map(records);
    Require(map.Units() && map.Units()->units.size() == records.size(), "fixed mode0 fixture parsed through actual codec");
    const auto* defaultMode = std::get_if<MapData::RandomAnyUnit>(&map.Units()->units.front().random);
    Require(defaultMode && defaultMode->level == 1 && defaultMode->itemClass == 0,
        "the codec keeps the ordinary mode0 payload instead of rewriting it to -1");
    const auto result = MapObjectReferenceCapture::Capture(map, native.Reader());
    Require(result.Succeeded(), "ordinary mode0 capture failed");
    const auto& snapshot = **result.value;
    Require(snapshot.References().size() == 5 && snapshot.Find("gg_unit_n00Q_0017")
        && snapshot.Find("gg_unit_hfoo_0018") && snapshot.Find("gg_item_I001_0019"),
        "ordinary units/items in mode0 and legacy -1 keep their real generated identities");
    Require(snapshot.Find("gg_unit_Iunt_0020") && snapshot.Find("gg_item_hItm_0021")
        && !snapshot.Find("gg_item_Iunt_0020") && !snapshot.Find("gg_unit_hItm_0021"),
        "classification follows native categories, never the first rawcode character");
    const auto unsupported = std::count_if(snapshot.Diagnostics().begin(), snapshot.Diagnostics().end(), [](const auto& diagnostic) {
        return diagnostic.code == "MAP_OBJECT_REFERENCE_PLACEMENT_UNSUPPORTED";
    });
    Require(unsupported == 21, "all random markers and ambiguous configurations remain rejected individually");
    Require(snapshot.Inputs()[0].bytes == UnitBytes(records), "qualification leaves the original mode bytes untouched");
    records.front().random = MapData::RandomGlobalTable{0, 0};
    const auto changed = MapObjectReferenceCapture::Verify(snapshot, Map(records), native.Reader());
    Require(!changed.Succeeded() && HasCode(changed.diagnostics, "MAP_OBJECT_REFERENCE_STALE"),
        "changing placement mode invalidates the frozen qualification");
}

void ClassificationDuplicatesAndCrossKindShadows() {
    Sources native, standard;
    native.files["war3map.w3u"] = Native({"n001", "n002", "n002", "n003", "hfoo"});
    native.files["war3map.w3t"] = Native({"n001"});
    standard.files["Units/UnitData.slk"] = Slk({"n002", "n003", "hfoo", "hfoo"});
    standard.files["Units/ItemData.slk"] = Slk({"ratf"});
    const auto map = Map({Placed("n001", 1), Placed("n002", 2), Placed("n003", 3), Placed("hfoo", 4)});
    const auto result = MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader());
    Require(result.Succeeded() && (*result.value)->References().size() == 1
        && (*result.value)->Find("gg_unit_n003_0003"), "cross-kind and same-file duplicate facts never pick the first or lower record");
    Require(HasCode(result.diagnostics, "MAP_OBJECT_REFERENCE_CLASSIFICATION_AMBIGUOUS"), "ambiguous source is reported");
    standard.files["Units/ItemData.slk"] = Slk({"n003"});
    const auto crossed = MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader());
    Require(crossed.Succeeded() && (*crossed.value)->References().empty(), "native unit cannot hide a positive standard item class");

    Sources justStandard, noNative;
    justStandard.files["Units/UnitData.slk"] = Slk({"hfoo"});
    const auto oneTable = MapObjectReferenceCapture::Capture(Map({Placed("hfoo", 1)}), noNative.Reader(), justStandard.Reader());
    Require(oneTable.Succeeded() && (*oneTable.value)->Find("gg_unit_hfoo_0001"), "one positive standard table works with explicitly missing sibling");
}

void BadHighPrioritySourcesCannotFallBack() {
    const auto map = Map({Placed("hfoo", 1)});
    Sources native, standard;
    native.files["war3map.w3u"] = Native({"hfoo"});
    standard.files["Units/UnitData.slk"] = Slk({"hfoo"});
    standard.files["Units/ItemData.slk"] = Slk({"ratf"});
    native.rejected = "war3map.w3u";
    const auto readFailure = MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader());
    RequireFailure(readFailure, "MAP_OBJECT_REFERENCE_SOURCE_READ_FAILED");
    Require(HasCode(readFailure.diagnostics, "TEST_SOURCE_SHADOW_CONFLICT"), "original provider conflict survives capture failure");
    native.rejected.clear();
    native.files["war3map.w3u"].push_back(std::byte{0});
    RequireFailure(MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader()), "MAP_OBJECT_REFERENCE_NATIVE_UNQUALIFIED");
    native.files["war3map.w3u"] = Native({"hfoo"});
    native.files["war3map.w3u"][12] = std::byte{'X'}; // Original表禁止非零newId。
    RequireFailure(MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader()), "MAP_OBJECT_REFERENCE_NATIVE_UNQUALIFIED");
    native.files["war3map.w3u"] = Native({"hfoo"});
    const MapObjectReferenceCapture::Reader throwing = [](std::string_view) -> Result<MapObjectReferenceSource> {
        throw std::runtime_error("synthetic unreadable high source");
    };
    RequireFailure(MapObjectReferenceCapture::Capture(map, throwing, standard.Reader()), "MAP_OBJECT_REFERENCE_SOURCE_READ_FAILED");
    standard.files["Units/ItemData.slk"] = TextBytes("ID;PWXL\nC;X1;Y1;K\"alias\"\n");
    RequireFailure(MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader()), "MAP_OBJECT_REFERENCE_STANDARD_UNQUALIFIED");
    native.sourceOverrides["war3map.w3u"] = "";
    RequireFailure(MapObjectReferenceCapture::Capture(map, native.Reader()), "MAP_OBJECT_REFERENCE_SOURCE_IDENTITY_MISSING");
}

void VerifyTracksEveryCapturedSource() {
    auto placed = Placed("hfoo", 1);
    const auto map = Map({placed});
    Sources native, standard;
    native.files["war3map.w3u"] = Native({"hfoo"});
    standard.files["Units/UnitData.slk"] = Slk({"hfoo"});
    const auto base = MapObjectReferenceCapture::Capture(map, native.Reader(), standard.Reader());
    Require(base.Succeeded(), "identity fixture capture");
    const auto& snapshot = **base.value;
    Require(MapObjectReferenceCapture::Verify(snapshot, map, native.Reader(), standard.Reader()).Succeeded(), "same owned inputs verify");
    const auto requireStale = [&](const Status& status) {
        Require(!status.Succeeded() && HasCode(status.diagnostics, "MAP_OBJECT_REFERENCE_STALE"), "changed captured identity must be stale");
    };
    placed.position[2] = 99.f;
    requireStale(MapObjectReferenceCapture::Verify(snapshot, Map({placed}), native.Reader(), standard.Reader()));
    requireStale(MapObjectReferenceCapture::Verify(snapshot, Map({Placed("hfoo", 1)}, "another-source.w3x"), native.Reader(), standard.Reader()));
    native.files["war3map.w3u"] = Native({"hfoo", "unus"});
    requireStale(MapObjectReferenceCapture::Verify(snapshot, map, native.Reader(), standard.Reader()));
    native.files["war3map.w3u"] = Native({"hfoo"});
    native.sourceOverrides["war3map.w3u"] = "shadowed/war3map.w3u";
    requireStale(MapObjectReferenceCapture::Verify(snapshot, map, native.Reader(), standard.Reader()));
    native.sourceOverrides.clear();
    standard.files["Units/ItemData.slk"] = Slk({});
    requireStale(MapObjectReferenceCapture::Verify(snapshot, map, native.Reader(), standard.Reader()));
    standard.files.erase("Units/ItemData.slk");
    standard.sourceOverrides["Units/ItemData.slk"] = "other-missing-origin/Units/ItemData.slk";
    requireStale(MapObjectReferenceCapture::Verify(snapshot, map, native.Reader(), standard.Reader()));
    standard.sourceOverrides.clear();
    requireStale(MapObjectReferenceCapture::Verify(snapshot, map, native.Reader()));
    Require(MapObjectReferenceCapture::Verify(snapshot, map, native.Reader(), standard.Reader()).Succeeded(), "restoring exact inputs restores verification");
}

void DoesNotIntroduceAnInstanceCap() {
    std::vector<MapData::UnitPlacement> records;
    for (std::int32_t id = 0; id < 8201; ++id) records.push_back(Placed("hfoo", id));
    Sources native;
    native.files["war3map.w3u"] = Native({"hfoo"});
    const auto result = MapObjectReferenceCapture::Capture(Map(std::move(records)), native.Reader());
    Require(result.Succeeded() && (*result.value)->References().size() == 8201
        && (*result.value)->Find("gg_unit_hfoo_8200"), "existing parser budgets are retained without an 8192 instance cap");
}
} // namespace

int main() {
    try {
        PositiveOwnedInputsAndOldVersions();
        MissingAndUnknownDoNotBecomeUnit();
        PlacementAmbiguityAndUnsupportedRecords();
        FixedDefaultModeAndActualRandomObjects();
        ClassificationDuplicatesAndCrossKindShadows();
        BadHighPrioritySourcesCannotFallBack();
        VerifyTracksEveryCapturedSource();
        DoesNotIntroduceAnInstanceCap();
        std::cout << "map-object-reference-capture: all checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "map-object-reference-capture: " << error.what() << '\n';
        return 1;
    }
}
