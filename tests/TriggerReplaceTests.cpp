#include "War3MapCore/Services/TriggerReplace.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "MapDataFixtures.h"
#include "../src/Services/TriggerReplaceWriterInternal.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace TriggerReplaceAllocationWatch {
thread_local std::size_t threshold = 0;
thread_local std::size_t largeRequests = 0;
void Record(std::size_t bytes) noexcept { if (threshold && bytes >= threshold) ++largeRequests; }
class Window {
public:
    explicit Window(std::size_t minimum) { threshold = minimum; largeRequests = 0; }
    ~Window() { threshold = 0; }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};
} // namespace TriggerReplaceAllocationWatch
// 只观察此控制台测试的普通 new 请求，不拒绝分配，不用它冒充总 RSS/对齐分配预算。
void* operator new(std::size_t bytes) {
    TriggerReplaceAllocationWatch::Record(bytes);
    if (void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary); file << text; Require(file.good(), "write original fixture");
}
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes; for (const auto c : text) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c))); return bytes;
}
std::shared_ptr<const MapObjectReferenceSnapshot> PreplacedSnapshot() {
    auto placements = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()); Require(placements.Succeeded(), "original tiny real doo");
    MapData::UnitPlacement unit; unit.rawcode = "I001"; unit.creationId = 17; unit.scale = {1, 1, 1}; placements.value->units.push_back(unit);
    unit.rawcode = "I002"; unit.creationId = 19; placements.value->units.push_back(unit);
    const auto encoded = placements.value->Serialize(); Require(encoded.Succeeded(), "actual doo encode");
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *encoded.value}}; const auto map = MapDocument::FromMembers(members, "u4-fixture.w3x");
    const MapObjectReferenceCapture::Reader absent = [](std::string_view name) { return Result<MapObjectReferenceSource>::Success({"u4-fixture/" + std::string(name), {}}); };
    const MapObjectReferenceCapture::Reader standard = [](std::string_view name) {
        std::string text = "ID;PWXL\nC;X1;Y1;K\"alias\"\n";
        if (name == "Units/UnitData.slk") text += "C;X1;Y2;K\"I001\"\nC;X1;Y3;K\"I002\"\n";
        else Require(name == "Units/ItemData.slk", "only real classification roles requested"); text += "E\n";
        return Result<MapObjectReferenceSource>::Success({"u4-standard/" + std::string(name), Bytes(text)});
    };
    const auto snapshot = MapObjectReferenceCapture::Capture(map, absent, standard); Require(snapshot.Succeeded() && *snapshot.value, "real sealed snapshot positive classification"); return *snapshot.value;
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-TriggerReplace-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog; TriggerDocument doc; std::unique_ptr<TriggerTypeSystem> types; MapData::StringTableDocument strings;
    std::vector<ObjectData::ObjectRecord> objects;
    std::vector<ProjectSearchObjectSource> sources;
    Fixture() {
        Require(std::filesystem::create_directory(root), "fresh owned fixture");
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\nstring=0,1,1,String,string\n"
            "unitcode=0,1,1,UnitId,integer\nabilcode=0,1,1,AbilityId,integer\nunit=0,1,0,Unit,unit\nscriptcode=0,0,0,Script,string\n");
        Write(root / "ui/base/action.txt", "[StoreInt]\n[[.args]]\ntype=integer\n[StoreString]\n[[.args]]\ntype=string\n"
            "[StoreUnit]\n[[.args]]\ntype=unitcode\n[UseUnit]\n[[.args]]\ntype=unit\n[CustomScriptCode]\n[[.args]]\ntype=string\n[ScriptSlot]\n[[.args]]\ntype=scriptcode\n");
        Write(root / "ui/base/call.txt", "[ReadInt]\nreturns=integer\n");
        const auto loaded = UiCatalog::Load(root / "ui"); Require(loaded.Succeeded(), "real catalog load"); catalog = *loaded.value;
        const auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "sealed original fixture"); doc = *empty.value;
        const auto category = TriggerDirectoryEditor::AddCategory(doc, "Original"); Require(category.Succeeded(), "real category");
        Require(TriggerDocumentEditor::AddTrigger(doc, *category.value, "T1").Succeeded(), "real trigger");
        Require(TriggerDocumentEditor::AddVariable(doc, "Old", "integer").Succeeded(), "source variable");
        Require(TriggerDocumentEditor::AddVariable(doc, "New", "integer").Succeeded(), "target variable");
        Require(TriggerDocumentEditor::AddVariable(doc, "Wrong", "string").Succeeded(), "wrong type variable");
        types = std::make_unique<TriggerTypeSystem>(catalog);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    StableId Add(std::string_view function, std::int32_t kind, std::string value, StableId trigger = 0) {
        if (!trigger) trigger = doc.triggers.front().id;
        const auto appended = TriggerDocumentEditor::AppendRootEca(doc, catalog, trigger, UiDefinitionKind::Action, function); Require(appended.Succeeded(), "actual appended ECA");
        const auto* eca = TriggerDocumentEditor::FindEca(doc, appended.value->rootEcaId); Require(eca && eca->arguments.size() == 1, "direct fixture slot");
        const auto id = doc.argumentNodes[eca->arguments.front()].id;
        Require(TriggerDocumentEditor::SetScalarArgument(doc, id, kind, std::move(value)).Succeeded(), "actual scalar editor"); return id;
    }
    TriggerReplaceContext Context() const { return {"generation=7;revision=19;catalog=current", &doc, &catalog, types.get(), nullptr, &strings, sources, nullptr, false}; }
    TriggerReplaceQuery Variables() const { TriggerReplaceQuery q; q.mode = TriggerReplaceMode::Variable; q.sourceVariableId = doc.variables[0].id; return q; }
    TriggerReplacement New() const { TriggerReplacement r; r.variableId = doc.variables[1].id; return r; }
    EncodedTriggerDocument Encoded() const { const auto result = WtgWctCodec{}.Serialize(doc, catalog); Require(result.Succeeded(), "actual encoded document"); return *result.value; }
};
void TestMultiPageAtomicCandidateAndSourceUnchanged() {
    Fixture f;
    for (unsigned i = 0; i < 300; ++i) f.Add("StoreInt", 1, i % 2 ? "udg_Old" : "Old");
    auto c = f.Context(); auto q = f.Variables(); TriggerReplaceLimits l; l.pageLimit = 37;
    const auto before = f.Encoded(); std::vector<std::string> keys; std::optional<TriggerReplaceCursor> cursor; TriggerReplaceQueryProof proof;
    std::set<std::string> unique; std::size_t pages = 0;
    do {
        const auto page = QueryTriggerReplace(c, c.revision, q, l, cursor ? &*cursor : nullptr);
        Require(page.Succeeded() && page.value->coverage.totalMatches == 300 && page.value->coverage.matchedInScannedScope == 300, "complete exact total beyond first128");
        if (pages++) Require(page.value->proof == proof, "all pages same owned source proof"); else proof = page.value->proof;
        for (const auto& hit : page.value->rows) {
            Require(hit.argumentId && hit.triggerId == f.doc.triggers[0].id && hit.ecaId && unique.insert(hit.rowKey).second, "real stable ECA argument owner");
            if (keys.size() < 256) keys.push_back(hit.rowKey);
        }
        cursor = page.value->next;
    } while (cursor);
    Require(pages == 9 && unique.size() == 300 && keys.size() == 256, "nine true pages and cross-page selection");
    const auto preview = PrepareTriggerReplacement(c, c.revision, q, proof, keys, f.New(), l);
    Require(preview.Succeeded() && preview.value->preview && preview.value->rows.size() == 256, "one sealed256 candidate");
    Require(preview.value->preview->Receipt().appliedOperations == 256 && preview.value->preview->Batch().operations.size() == 256, "one shared graph batch");
    Require(VerifyTriggerReplacementPreview(c, c.revision, *preview.value->preview, l).Succeeded(), "actual source binding verifies before publication");
    auto after = f.doc;
    Require(ReplayTriggerGraphScalarEdit(after, f.catalog, preview.value->preview->ScalarReceipt(), true).Succeeded(), "actual compact Core forward publication on independent fixture copy");
    Require(std::count_if(after.argumentNodes.begin(), after.argumentNodes.end(), [](const auto& a) { return a.type == 1 && a.value.text == "New"; }) == 256, "only selected scalar references changed");
    for (std::size_t i = 256; i < after.argumentNodes.size(); ++i) {
        const auto& a = after.argumentNodes[i]; const auto& original = f.doc.argumentNodes[i];
        Require(a.id == original.id && a.type == original.type && a.insertCall == original.insertCall && a.callEca == original.callEca
            && a.insertIndex == original.insertIndex && a.indexArgument == original.indexArgument && a.value.text == original.value.text
            && a.value.encoding == original.value.encoding && a.value.originalBytes == original.value.originalBytes
            && a.origin.offset == original.origin.offset && a.origin.length == original.origin.length && a.origin.fingerprint == original.origin.fingerprint
            && a.origin.source == original.origin.source, "all unselected44 scalar bytes metadata preserved exactly");
    }
    const auto unchanged = f.Encoded(); Require(before.wtg == unchanged.wtg && before.wct == unchanged.wct, "query/options/preview never modify source bytes");
    Require(ReplayTriggerGraphScalarEdit(after, f.catalog, preview.value->preview->ScalarReceipt(), false).Succeeded(), "actual compact Core backward restores complete old graph identity");
    const auto replayed = WtgWctCodec{}.Serialize(after, f.catalog);
    Require(replayed.Succeeded() && replayed.value->wtg == before.wtg && replayed.value->wct == before.wct,
        "compact backward restores entire original WTG WCT bytes");
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, *preview.value->rows[0].hit.argumentId, 3, "9").Succeeded(), "actual source edit");
    Require(!VerifyTriggerReplacementPreview(c, c.revision, *preview.value->preview, l).Succeeded(), "original value drift refused even same context stamp");
}
void TestPreparedLinearMatcherAndCachedWtsWorstPrefix() {
    Fixture f; std::string large(2 * 1024 * 1024, 'a'); large.back() = 'b';
    const auto id = f.Add("StoreString", 3, large); auto c = f.Context(); TriggerReplaceQuery q;
    q.needle = std::string(2048, 'a'); q.needle.back() = 'c';
    auto start = std::chrono::steady_clock::now(); const auto raw = QueryTriggerReplace(c, c.revision, q);
    const auto rawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Require(raw.Succeeded() && raw.value->coverage.totalMatches == 0 && rawMs <= 1000.0, "near duplicate2MiB/2048needle linear query within foreground gate");
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, id, 3, "TRIGSTR_7").Succeeded(), "WTS scalar source");
    for (unsigned i = 0; i < 200; ++i) f.Add("StoreString", 3, "TRIGSTR_0007");
    f.strings.entries.push_back({7, std::move(large), 0});
    start = std::chrono::steady_clock::now(); const auto wts = QueryTriggerReplace(c, c.revision, q);
    const auto wtsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    Require(wts.Succeeded() && wts.value->coverage.totalMatches == 0 && wts.value->coverage.nameRecordsCharged == 1 && wtsMs <= 1000.0,
        "sharedN1 full2MiB WTS display and visible matcher cached once across201 actual refs");
    std::cout << "nearPrefixRawMs=" << rawMs << " repeatedWtsMs=" << wtsMs << '\n';
    f.strings.entries.clear(); Require(TriggerDocumentEditor::SetScalarArgument(f.doc, id, 3, "AbCdEf").Succeeded(), "ASCII source");
    q.needle = "cDe"; q.caseSensitive = false;
    const auto folded = QueryTriggerReplace(c, c.revision, q); Require(folded.Succeeded() && folded.value->rows.size() == 1, "ASCII folding substring preserved");
    q.match = TriggerReplaceMatch::Exact; q.needle = "aBcDeF";
    const auto exact = QueryTriggerReplace(c, c.revision, q); Require(exact.Succeeded() && exact.value->rows.size() == 1, "ASCII folding exact preserved");
    q.caseSensitive = true; const auto strict = QueryTriggerReplace(c, c.revision, q); Require(strict.Succeeded() && strict.value->rows.empty(), "strict ASCII remains case sensitive");
}
void TestTargetAndCodeDriftCannotPublishOldCandidate() {
    Fixture f; f.Add("StoreInt", 1, "Old"); auto c = f.Context(); const auto q = f.Variables();
    const auto page = QueryTriggerReplace(c, c.revision, q); Require(page.Succeeded(), "source page");
    const std::vector<std::string> keys{page.value->rows[0].rowKey}; const auto plan = PrepareTriggerReplacement(c, c.revision, q, page.value->proof, keys, f.New());
    Require(plan.Succeeded() && plan.value->preview, "sealed plan");
    const auto originalComment = f.doc.wct.customComment;
    f.doc.wct.customComment.text = "Changed unsearched WCT comment";
    Require(!VerifyTriggerReplacementPreview(c, c.revision, *plan.value->preview).Succeeded(), "full binding protects unsearched WCT old body");
    f.doc.wct.customComment = originalComment;
    Require(TriggerDocumentEditor::RenameVariable(f.doc, "New", "OtherNew").Succeeded(), "actual target declaration rename");
    Require(!VerifyTriggerReplacementPreview(c, c.revision, *plan.value->preview).Succeeded(), "target declaration identity/name drift rejected");
}
void TestWtsVisibleTextFullDomainAndPartial() {
    Fixture f; f.Add("StoreString", 3, "TRIGSTR_0007"); f.strings.entries.push_back({7, "|cffff0000可见中文|r", 0});
    TriggerReplaceQuery q; q.needle = "可见中文"; auto c = f.Context(); const auto before = f.Encoded();
    const auto page = QueryTriggerReplace(c, c.revision, q);
    Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->rows[0].oldValue == "TRIGSTR_0007"
        && page.value->rows[0].displayValue == "可见中文" && page.value->coverage.totalMatches == 1, "shared N1 WTS visible formatting and original value");
    auto missing = c; missing.strings = nullptr;
    const auto absent = QueryTriggerReplace(missing, missing.revision, q);
    Require(absent.Succeeded() && absent.value->rows.empty() && absent.value->coverage.displayResolutionPartial && !absent.value->coverage.totalMatches, "missing WTS cannot claim complete visible search");
    f.strings.entries.push_back({7, "late duplicate", 0});
    const auto duplicate = QueryTriggerReplace(c, c.revision, q);
    Require(duplicate.Succeeded() && duplicate.value->rows.empty() && duplicate.value->coverage.displayResolutionPartial && !duplicate.value->coverage.totalMatches, "late duplicate does not trust earlier singleton");
    f.strings.entries.pop_back(); TriggerReplaceLimits l; l.maxNameTextBytes = 1;
    const auto bounded = QueryTriggerReplace(c, c.revision, q, l);
    Require(bounded.Succeeded() && bounded.value->coverage.displayResolutionPartial && !bounded.value->coverage.totalMatches, "full WTS budget gate before shared resolver");
    const auto after = f.Encoded(); Require(before.wtg == after.wtg && before.wct == after.wct, "WTS display search readonly");
}
void TestWtsFallbackAliasesNeverShareRawMatches() {
    Fixture f; const auto firstId = f.Add("StoreString", 3, "TRIGSTR_01"); const auto secondId = f.Add("StoreString", 3, "TRIGSTR_1");
    auto c = f.Context(); TriggerReplaceQuery q; q.needle = "TRIGSTR_01"; q.match = TriggerReplaceMatch::Exact;
    const auto one = [&](const Result<TriggerReplacePage>& page) {
        Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->rows[0].argumentId == firstId
            && page.value->rows[0].oldValue == "TRIGSTR_01" && page.value->coverage.matchedInScannedScope == 1
            && page.value->coverage.displayResolutionPartial && !page.value->coverage.totalMatches, "raw fallback aliases never share StringId match");
    };
    c.strings = nullptr; one(QueryTriggerReplace(c, c.revision, q));
    c = f.Context(); f.strings.entries.push_back({99, "another actual ID", 0}); one(QueryTriggerReplace(c, c.revision, q));
    f.strings.entries = {{1, std::string(128, 'x'), 0}};
    const auto prepared = QueryTriggerReplace(c, c.revision, q); Require(prepared.Succeeded() && prepared.value->coverage.totalMatches == 1, "trusted WTS raw exact still one alias");
    TriggerReplaceLimits limited;
    const auto decodeBill = 128 * 6 + std::string_view("TRIGSTR_01").size(); Require(prepared.value->coverage.textBytesCharged > decodeBill, "real source+decode bill");
    limited.maxScanTextBytes = prepared.value->coverage.textBytesCharged - decodeBill;
    one(QueryTriggerReplace(c, c.revision, q, limited));
    f.strings.entries = {{1, "共用展示", 0}}; q.needle = "共用展示";
    const auto resolved = QueryTriggerReplace(c, c.revision, q);
    Require(resolved.Succeeded() && resolved.value->rows.size() == 2 && resolved.value->coverage.totalMatches == 2, "trusted resolved display shares ID match across leading zeros");
    f.strings.entries = {{1, "", 0}}; const auto source = f.Encoded();
    for (const auto& [raw, id] : std::vector<std::pair<std::string, StableId>>{{"TRIGSTR_01", firstId}, {"TRIGSTR_1", secondId}}) {
        q.needle = raw; const auto empty = QueryTriggerReplace(c, c.revision, q);
        Require(empty.Succeeded() && empty.value->rows.size() == 1 && empty.value->rows[0].argumentId == id
            && empty.value->rows[0].oldValue == raw && empty.value->rows[0].displayValue == raw
            && empty.value->coverage.totalMatches == 1 && !empty.value->coverage.displayResolutionPartial,
            "trusted empty WTS preserves each raw alias without false shared display match");
    }
    const auto after = f.Encoded(); Require(source.wtg == after.wtg && source.wct == after.wct && f.strings.entries[0].value.empty(), "empty WTS query leaves source unchanged");
}
void TestRealMixedWtsLocalEncodingAndSourcePreservation() {
    Fixture f; f.Add("StoreString", 3, "TRIGSTR_1"); f.Add("StoreString", 3, "TRIGSTR_2");
    std::string raw = "STRING 1\n{\n";
    const unsigned char gb[]{0xd6, 0xd0, 0xce, 0xc4, 0xd4, 0xad, 0xd7, 0xd6, 0xbd, 0xda};
    for (const auto byte : gb) raw += static_cast<char>(byte);
    raw += "\n}\nSTRING 2\n{\nUTF8中文\n}\n";
    const auto original = Bytes(raw); auto parsed = MapData::ParseStringTable(original); Require(parsed.Succeeded(), "real mixed GB18030/UTF8 WTS parser");
    f.strings = *parsed.value; const auto source = f.Encoded(); auto c = f.Context(); TriggerReplaceQuery q; q.needle = "中文";
    const auto mixed = QueryTriggerReplace(c, c.revision, q);
    Require(mixed.Succeeded() && mixed.value->rows.size() == 2 && mixed.value->coverage.totalMatches == 2
        && !mixed.value->coverage.displayResolutionPartial, "shared N1 decodes both actual local bytes and UTF8 visible parameter text");
    q.needle = "原字节"; const auto local = QueryTriggerReplace(c, c.revision, q);
    Require(local.Succeeded() && local.value->rows.size() == 1 && local.value->rows[0].displayValue == "中文原字节", "known GB18030 Chinese display exact");
    const auto roundtrip = f.strings.Serialize();
    Require(roundtrip.Succeeded() && *roundtrip.value == original && f.strings.originalBytes == original, "mixed WTS raw source bytes never transcoded or written");
    const auto after = f.Encoded(); Require(source.wtg == after.wtg && source.wct == after.wct, "mixed visible query keeps WTG/WCT source bytes");
}
void TestRealSealedPreplacedObjectReferencesAndMissingSource() {
    Fixture f; const auto id = f.Add("UseUnit", 1, "gg_unit_I001_0017"); auto c = f.Context();
    TriggerReplaceQuery q; q.mode = TriggerReplaceMode::ObjectReference; q.sourceObject = ObjectData::ObjectKey{ObjectData::ObjectType::Unit, "I001"};
    const auto missing = QueryTriggerReplace(c, c.revision, q);
    Require(missing.Succeeded() && missing.value->rows.empty() && missing.value->coverage.unresolvedReferenceRows == 1
        && missing.value->coverage.state == TriggerReplaceState::Partial && !missing.value->coverage.totalMatches, "missing source cannot claim no preplaced references");
    const auto snapshot = PreplacedSnapshot(); c.mapObjects = snapshot.get(); f.types = std::make_unique<TriggerTypeSystem>(f.catalog, snapshot.get()); c.types = f.types.get();
    const auto page = QueryTriggerReplace(c, c.revision, q);
    Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->rows[0].argumentId == id && page.value->coverage.totalMatches == 1,
        "actual rawcode and unit role from sealed classification find preplaced scalar");
    TriggerReplacement replacement; replacement.kind = TriggerReplacementKind::PreplacedObject;
    replacement.preplacedSymbol = "gg_unit_I002_0019"; replacement.mapObjectSnapshotIdentity = snapshot->Identity();
    const std::vector<std::string> keys{page.value->rows[0].rowKey}; const auto plan = PrepareTriggerReplacement(c, c.revision, q, page.value->proof, keys, replacement);
    Require(plan.Succeeded() && plan.value->preview && plan.value->rows[0].replacementValue == replacement.preplacedSymbol
        && VerifyTriggerReplacementPreview(c, c.revision, *plan.value->preview).Succeeded(), "true typed preplaced candidate same sealed current identity");
    q.needle = "I002"; q.sourceObject.reset(); Require(TriggerDocumentEditor::SetScalarArgument(f.doc, id, 1, "gg_unit_I002_9999").Succeeded(), "actual unqualified symbol");
    const auto unknown = QueryTriggerReplace(c, c.revision, q);
    Require(unknown.Succeeded() && unknown.value->rows.empty() && unknown.value->coverage.state == TriggerReplaceState::Partial && !unknown.value->coverage.totalMatches,
        "unknown instance not inferred by rawcode or symbol prefix");
}
void TestTypesCodeAndObjectDomainsRemainSealed() {
    Fixture f; const auto intId = f.Add("StoreInt", 1, "Old"); f.Add("ScriptSlot", 3, "Old");
    f.Add("CustomScriptCode", 3, "Old"); f.Add("StoreUnit", 3, "hfoo");
    auto c = f.Context(); auto q = f.Variables(); const auto page = QueryTriggerReplace(c, c.revision, q); Require(page.Succeeded(), "typed variable query");
    const std::vector<std::string> keys{"argument/" + std::to_string(intId)};
    auto wrong = f.New(); wrong.variableId = f.doc.variables[2].id;
    const auto rejected = PrepareTriggerReplacement(c, c.revision, q, page.value->proof, keys, wrong);
    Require(rejected.Succeeded() && !rejected.value->preview && rejected.value->rows.size() == 1
        && rejected.value->rows[0].reasonCode == "TRIGGER_REPLACE_TYPE_MISMATCH", "selected wrong type retained and whole candidate refused");
    TriggerReplaceQuery text; text.needle = "Old";
    const auto raw = QueryTriggerReplace(c, c.revision, text); Require(raw.Succeeded() && raw.value->rows.size() == 3, "raw parameter code text may be found readonly");
    std::vector<std::string> all; for (const auto& row : raw.value->rows) all.push_back(row.rowKey);
    const auto code = PrepareTriggerReplacement(c, c.revision, text, raw.value->proof, all, f.New());
    Require(code.Succeeded() && !code.value->preview && code.value->rows.size() == all.size(), "selected script/mapped source is not silently dropped");
    ObjectData::ObjectRecord oldObject, nextObject, ability;
    oldObject.stableId = 101; oldObject.key = {ObjectData::ObjectType::Unit, "hfoo"};
    nextObject.stableId = 102; nextObject.key = {ObjectData::ObjectType::Unit, "hkni"};
    ability.stableId = 103; ability.key = {ObjectData::ObjectType::Ability, "A000"};
    f.objects = {oldObject, nextObject}; std::vector<ObjectData::ObjectRecord> abilities{ability};
    f.sources = {{ObjectData::ObjectType::Unit, f.objects, ProjectSearchState::Ready}, {ObjectData::ObjectType::Ability, abilities, ProjectSearchState::Ready}}; c = f.Context();
    TriggerReplaceQuery objectQuery; objectQuery.mode = TriggerReplaceMode::ObjectReference; objectQuery.sourceObject = oldObject.key;
    const auto objectPage = QueryTriggerReplace(c, c.revision, objectQuery); Require(objectPage.Succeeded() && objectPage.value->rows.size() == 1, "typed object literal query");
    const std::vector<std::string> objectKeys{objectPage.value->rows[0].rowKey}; TriggerReplacement replacement;
    replacement.kind = TriggerReplacementKind::ObjectLiteral; replacement.object = ability.key;
    const auto wrongDomain = PrepareTriggerReplacement(c, c.revision, objectQuery, objectPage.value->proof, objectKeys, replacement);
    Require(wrongDomain.Succeeded() && !wrongDomain.value->preview, "integer compatibility does not erase unit versus ability object domain");
    replacement.object = nextObject.key;
    const auto objectPlan = PrepareTriggerReplacement(c, c.revision, objectQuery, objectPage.value->proof, objectKeys, replacement);
    Require(objectPlan.Succeeded() && objectPlan.value->preview && objectPlan.value->rows[0].replacementValue == "hkni", "actual present same-domain object candidate");
}
void TestRealPartialObjectDomainNeverGrantsUniqueTarget() {
    Fixture f; f.Add("StoreUnit", 3, "hfoo");
    ObjectData::LniObjectCodec codec(ObjectData::ObjectType::Unit);
    const auto original = Bytes("[hfoo]\n_parent=\"hfoo\"\n[hfoo]\n");
    auto parsed = codec.Parse(original);
    const auto* unsafe = dynamic_cast<const ObjectData::LniObjectDocument*>(parsed.document.get());
    Require(unsafe && !unsafe->IsStructurallySafe() && unsafe->Objects().size() == 1
        && std::any_of(parsed.diagnostics.begin(), parsed.diagnostics.end(), [](const auto& d) { return d.code == "lni-section-duplicate"; }),
        "real duplicate LNI sections collapse second record but retain unsafe source");
    const auto fingerprint = unsafe->DocumentFingerprint();
    ObjectData::CatalogLoadOptions options;
    options.sources = {{ObjectData::ObjectType::Unit, {"owned-units.slk"}, {}}}; options.stringFiles.clear();
    const auto slk = Bytes("ID;PWXL;N;E\nC;X1;Y1;K\"unitID\"\nC;X2;K\"name\"\nC;X1;Y2;K\"hfoo\"\nC;X2;K\"Original\"\nC;X1;Y3;K\"hkni\"\nC;X2;K\"Knight\"\nE\n");
    auto loaded = ObjectData::StandardObjectCatalog::Load([&](const std::string& name) {
        Require(name == "owned-units.slk", "standard metadata reader only original owned fixture");
        return Result<std::vector<std::byte>>::Success(slk);
    }, options);
    Require(loaded.Succeeded() && loaded.value->Entries().size() == 2, "real readonly two-record standard catalog");
    f.sources.clear();
    for (unsigned type = 0; type != 7; ++type) f.sources.push_back({static_cast<ObjectData::ObjectType>(type), {}, ProjectSearchState::Missing});
    f.sources[0].records = unsafe->Objects(); f.sources[0].state = ProjectSearchState::Partial;
    auto c = f.Context(); c.standardObjects = &*loaded.value; c.standardObjectsReady = true;
    TriggerReplaceQuery q; q.mode = TriggerReplaceMode::ObjectReference; q.sourceObject = ObjectData::ObjectKey{ObjectData::ObjectType::Unit, "hfoo"};
    const auto source = f.Encoded(); const auto page = QueryTriggerReplace(c, c.revision, q);
    Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->rows[0].eligibility == TriggerReplaceEligibility::ReadOnly
        && page.value->rows[0].reasonCode == "TRIGGER_REPLACE_OBJECT_SOURCE_UNKNOWN" && page.value->coverage.state == TriggerReplaceState::Partial
        && !page.value->coverage.totalMatches, "original typed argument remains readonly discoverable without granting unsafe target uniqueness");
    const auto targets = QueryTriggerReplacementOptions(c, c.revision, TriggerReplacementKind::ObjectLiteral, "");
    Require(targets.Succeeded() && targets.value->rows.empty() && targets.value->coverage.state == TriggerReplaceState::Partial
        && !targets.value->coverage.totalMatches, "partial collapsed custom domain rejects both its custom record and standard fallback");
    const std::vector<std::string> keys{page.value->rows[0].rowKey}; TriggerReplacement replacement;
    replacement.kind = TriggerReplacementKind::ObjectLiteral;
    for (const auto* rawcode : {"hfoo", "hkni"}) {
        replacement.object = ObjectData::ObjectKey{ObjectData::ObjectType::Unit, rawcode};
        const auto plan = PrepareTriggerReplacement(c, c.revision, q, page.value->proof, keys, replacement);
        Require(!plan.Succeeded() || !plan.value->preview, "direct forged target DTO cannot bypass incomplete actual source domain");
    }
    f.sources[0].state = ProjectSearchState::Missing;
    Require(!QueryTriggerReplacementOptions(c, c.revision, TriggerReplacementKind::ObjectLiteral, "").Succeeded(), "Missing source with records is contradictory and never trusted as absent overlay");
    auto ready = codec.Parse(Bytes("[hfoo]\n_parent=\"hfoo\"\n"));
    const auto* safe = dynamic_cast<const ObjectData::LniObjectDocument*>(ready.document.get());
    Require(safe && safe->IsStructurallySafe() && safe->Objects().size() == 1, "actual complete custom LNI control");
    f.sources[0].records = safe->Objects(); f.sources[0].state = ProjectSearchState::Ready;
    c.objects = f.sources;
    const auto complete = QueryTriggerReplacementOptions(c, c.revision, TriggerReplacementKind::ObjectLiteral, "");
    Require(complete.Succeeded() && complete.value->rows.size() == 2 && complete.value->coverage.totalMatches == 2, "Ready full domain retains actual custom plus nonoverridden standard choices");
    replacement.object = ObjectData::ObjectKey{ObjectData::ObjectType::Unit, "hkni"};
    Require(!PrepareTriggerReplacement(c, c.revision, q, page.value->proof, keys, replacement).Succeeded(), "actual source state change invalidates old query proof even if caller revision unchanged");
    const auto current = QueryTriggerReplace(c, c.revision, q); Require(current.Succeeded() && current.value->coverage.totalMatches == 1, "complete source query control is ready");
    const auto allowed = PrepareTriggerReplacement(c, c.revision, q, current.value->proof, keys, replacement);
    Require(allowed.Succeeded() && allowed.value->preview, "complete typed standard target control remains accepted");
    const auto after = f.Encoded(); Require(source.wtg == after.wtg && source.wct == after.wct && unsafe->DocumentFingerprint() == fingerprint
        && original == Bytes("[hfoo]\n_parent=\"hfoo\"\n[hfoo]\n"), "query/candidates never mutate trigger or incomplete object source");
}
void TestCursorShapeCoverageAndBudgets() {
    Fixture f; for (unsigned i = 0; i < 10; ++i) f.Add("StoreInt", 1, "Old");
    auto c = f.Context(); auto q = f.Variables(); TriggerReplaceLimits l; l.pageLimit = 3;
    const auto first = QueryTriggerReplace(c, c.revision, q, l); Require(first.Succeeded() && first.value->next, "real cursor");
    auto changed = l; changed.pageLimit = 4;
    Require(!QueryTriggerReplace(c, c.revision, q, changed, &*first.value->next).Succeeded(), "cursor binds all options and page scope");
    auto stale = c; stale.revision += ";new"; Require(!QueryTriggerReplace(stale, c.revision, q, l).Succeeded(), "actual revision stale");
    q.needle = std::string("bad\0query", 9); Require(!QueryTriggerReplace(c, c.revision, q, l).Succeeded(), "NUL cannot become query"); q = f.Variables();
    changed = l; changed.maxGraphRecords = 1;
    const auto partial = QueryTriggerReplace(c, c.revision, q, changed);
    Require(partial.Succeeded() && partial.value->coverage.scanTruncated && !partial.value->coverage.totalMatches && partial.value->rows.empty(), "record admission partial never claims first few records fullmap");
    const std::vector<std::string> duplicate{first.value->rows[0].rowKey, first.value->rows[0].rowKey};
    Require(!PrepareTriggerReplacement(c, c.revision, q, first.value->proof, duplicate, f.New(), l).Succeeded(), "duplicate selections fail before copies");
    std::vector<std::string> over(257, first.value->rows[0].rowKey);
    Require(!PrepareTriggerReplacement(c, c.revision, q, first.value->proof, over, f.New(), l).Succeeded(), "explicit cross-page256 cap");
    std::vector<std::string> unknown{"argument/18446744073709551615"};
    Require(!PrepareTriggerReplacement(c, c.revision, q, first.value->proof, unknown, f.New(), l).Succeeded(), "forged rowkey not trusted");
    changed = l; changed.maxPreviewDocumentBytes = 1;
    const auto cappedPage = QueryTriggerReplace(c, c.revision, q, changed); Require(cappedPage.Succeeded(), "query independent of preview copy limit");
    const std::vector<std::string> one{cappedPage.value->rows[0].rowKey};
    Require(!PrepareTriggerReplacement(c, c.revision, q, cappedPage.value->proof, one, f.New(), changed).Succeeded(), "before/after IR charge rejects before full copies");
    f.doc.argumentNodes.back().id = f.doc.argumentNodes.front().id;
    Require(!QueryTriggerReplace(c, c.revision, q, l).Succeeded(), "late duplicate argument does not publish prior pages");
}
void TestScannerAdmittedIdentityCapacityPreservesProofAndRejections() {
    Fixture f; std::vector<StableId> arguments;
    for (unsigned i = 0; i < 40; ++i) arguments.push_back(f.Add("StoreInt", 1, "Old"));
    const auto original = f.Encoded(); auto context = f.Context(); const auto query = f.Variables();
    TriggerReplaceLimits limits; limits.pageLimit = 7;
    const auto records = f.doc.categories.size() + f.doc.variables.size() + f.doc.triggers.size()
        + f.doc.ecaNodes.size() + f.doc.argumentNodes.size();
    constexpr std::size_t largeAllocation = 64 * 1024;
    {
        TriggerReplaceAllocationWatch::Window watch(largeAllocation);
        void* memory = ::operator new(largeAllocation); ::operator delete(memory);
        Require(TriggerReplaceAllocationWatch::largeRequests == 1, "allocation watcher observes a real standard-new request");
    }
    const auto first = [&] {
        TriggerReplaceAllocationWatch::Window watch(largeAllocation);
        auto result = QueryTriggerReplace(context, context.revision, query, limits);
        Require(TriggerReplaceAllocationWatch::largeRequests == 0,
            "small admitted graph does not reserve the default half-million-record budget");
        return result;
    }();
    Require(first.Succeeded() && first.value->coverage.state == TriggerReplaceState::Ready
        && first.value->coverage.graphRecordsCharged == records && first.value->coverage.totalMatches == arguments.size()
        && first.value->rows.size() == limits.pageLimit && first.value->next, "all five admitted ID domains retain complete query coverage");
    auto exactLimits = limits; exactLimits.maxGraphRecords = records;
    const auto exact = QueryTriggerReplace(context, context.revision, query, exactLimits);
    Require(exact.Succeeded() && exact.value->proof.snapshotSha256 == first.value->proof.snapshotSha256
        && exact.value->proof.sourceWtgSha256 == first.value->proof.sourceWtgSha256
        && exact.value->proof.sourceWctSha256 == first.value->proof.sourceWctSha256
        && exact.value->proof.catalogSha256 == first.value->proof.catalogSha256
        && exact.value->proof.requestSha256 != first.value->proof.requestSha256,
        "capacity preparation leaves metadata and source proofs unchanged while public limit changes still bind request proof");
    std::optional<TriggerReplaceCursor> cursor; std::size_t visited = 0;
    do {
        const auto page = QueryTriggerReplace(context, context.revision, query, limits, cursor ? &*cursor : nullptr);
        Require(page.Succeeded() && page.value->proof == first.value->proof && page.value->matchOffset == visited,
            "capacity preparation preserves ordered pagination and exact current proof");
        for (const auto& hit : page.value->rows) {
            Require(visited < arguments.size() && hit.argumentId == arguments[visited]
                && hit.rowKey == "argument/" + std::to_string(arguments[visited])
                && hit.oldKind == 1 && hit.oldValue == "Old" && hit.declaredType == "integer",
                "every original stable row retains insertion order and scalar metadata");
            ++visited;
        }
        cursor = page.value->next;
    } while (cursor);
    Require(visited == arguments.size(), "capacity preparation loses no query rows across pages");
    auto tooSmall = limits; tooSmall.maxGraphRecords = records - 1;
    const auto capped = QueryTriggerReplace(context, context.revision, query, tooSmall);
    Require(capped.Succeeded() && capped.value->coverage.scanTruncated && !capped.value->coverage.totalMatches
        && capped.value->rows.empty() && capped.value->coverage.graphEdgesCharged == 0
        && capped.value->coverage.triggersExamined == 0, "one-record shortfall rejects shape before ID and edge traversal");
    auto oversized = f.doc; oversized.argumentNodes.resize(32768, oversized.argumentNodes.front());
    auto oversizedContext = context; oversizedContext.document = &oversized; tooSmall.maxGraphRecords = 1;
    {
        TriggerReplaceAllocationWatch::Window watch(largeAllocation);
        const auto rejected = QueryTriggerReplace(oversizedContext, context.revision, query, tooSmall);
        Require(rejected.Succeeded() && rejected.value->coverage.scanTruncated && rejected.value->rows.empty()
            && !rejected.value->coverage.totalMatches && TriggerReplaceAllocationWatch::largeRequests == 0,
            "oversized owned ID domain is capped before graph-sized standard-new allocation or malformed-ID traversal");
    }
    const auto savedId = f.doc.argumentNodes.back().id;
    for (const auto collision : {f.doc.argumentNodes.front().id, f.doc.categories.front().id, f.doc.variables.front().id,
        f.doc.triggers.front().id, f.doc.ecaNodes.front().id, StableId{0}}) {
        f.doc.argumentNodes.back().id = collision;
        const auto rejected = QueryTriggerReplace(context, context.revision, query, limits);
        Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
            && rejected.diagnostics.front().code == "TRIGGER_REPLACE_GRAPH_INVALID",
            "reserve never authorizes duplicate, cross-domain-colliding or zero stable IDs");
        f.doc.argumentNodes.back().id = savedId;
    }
    const auto restored = QueryTriggerReplace(context, context.revision, query, limits); const auto after = f.Encoded();
    Require(restored.Succeeded() && restored.value->proof == first.value->proof && after.wtg == original.wtg && after.wct == original.wct,
        "query, bounded refusal and collision validation preserve original WTG WCT and exact proof after fixture restoration");
}
void TestOptionsAndScopeDoNotGuess() {
    Fixture f; f.Add("StoreInt", 1, "Old"); auto c = f.Context(); TriggerReplaceLimits l; l.pageLimit = 1;
    const auto first = QueryTriggerReplacementOptions(c, c.revision, TriggerReplacementKind::Variable, {}, l);
    Require(first.Succeeded() && first.value->rows.size() == 1 && first.value->rows[0].display == "Old" && first.value->rows[0].replacement.variableId == f.doc.variables[0].id && first.value->next, "real variable identity and exact display");
    const auto second = QueryTriggerReplacementOptions(c, c.revision, TriggerReplacementKind::Variable, {}, l, &*first.value->next);
    Require(second.Succeeded() && second.value->rows[0].display == "New" && second.value->proof == first.value->proof, "typed options true next page");
    const auto preplaced = QueryTriggerReplacementOptions(c, c.revision, TriggerReplacementKind::PreplacedObject, {});
    Require(preplaced.Succeeded() && preplaced.value->rows.empty() && preplaced.value->coverage.state == TriggerReplaceState::Partial && !preplaced.value->coverage.totalMatches, "missing real preplaced snapshot not guessed from gg prefix");
    auto q = f.Variables(); q.scopeTriggerId = 99999999; Require(!QueryTriggerReplace(c, c.revision, q).Succeeded(), "missing stable scope rejected");
    q.scopeTriggerId = f.doc.triggers[0].id; const auto current = QueryTriggerReplace(c, c.revision, q);
    Require(current.Succeeded() && current.value->coverage.triggersExamined == 1 && current.value->coverage.totalMatches == 1, "current GUI scope total only");
    TriggerReplaceQuery functions; functions.mode = TriggerReplaceMode::Function; functions.needle = "Store";
    const auto found = QueryTriggerReplace(c, c.revision, functions);
    Require(found.Succeeded() && found.value->rows.size() == 1 && !found.value->rows[0].argumentId && found.value->rows[0].eligibility == TriggerReplaceEligibility::ReadOnly, "function search remains readonly actual ECA");
}
void TestStrictSharedUtf8InputPolicy() {
    Fixture f; f.Add("StoreString", 3, "中文🌟"); const auto source = f.Encoded(); const auto c = f.Context();
    const std::vector<std::string> invalid{
        std::string(1, static_cast<char>(0x80)), std::string("\xc0\xaf", 2), std::string("\xe0\x80\xaf", 3),
        std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xf0\x9f", 2),
        std::string("\xe2x\x80", 3), std::string("A\0B", 3)};
    for (const auto& value : invalid) {
        TriggerReplaceQuery q; q.needle = value;
        const auto query = QueryTriggerReplace(c, c.revision, q);
        Require(!query.Succeeded() && !query.value, "shared scalar validation preserves malformed UTF8/NUL query rejection before graph scan");
        const auto options = QueryTriggerReplacementOptions(c, c.revision, TriggerReplacementKind::Variable, value);
        Require(!options.Succeeded() && !options.value, "typed option filter retains same malformed UTF8/NUL rejection");
    }
    TriggerReplaceQuery valid; valid.needle = "中文🌟"; valid.match = TriggerReplaceMatch::Exact;
    const auto page = QueryTriggerReplace(c, c.revision, valid);
    Require(page.Succeeded() && page.value->rows.size() == 1 && page.value->coverage.totalMatches == 1, "valid3byte Chinese and4byte scalar keep existing exact matching policy");
    valid.needle = std::string("\xf4\x8f\xbf\xbf", 4);
    Require(QueryTriggerReplace(c, c.revision, valid).Succeeded(), "Unicode maximum scalar remains valid input even when no source hit");
    const auto after = f.Encoded(); Require(source.wtg == after.wtg && source.wct == after.wct, "UTF8 query/option guards do not change document source");
}
void TestOwnedPlanCompletionPoliciesAndDeferredWholeGraph() {
    Fixture f; const auto selectedId = f.Add("StoreInt", 1, "Old"); f.Add("StoreInt", 1, "udg_Old");
    const auto original = f.Encoded(); auto c = f.Context(); const auto query = f.Variables();
    TriggerReplaceLimits limits; limits.pageLimit = 1; limits.maxPreviewWorkUnits = 1;
    const TriggerReplacePlan empty;
    Require(empty.Empty() && empty.Rows().empty() && !CompleteTriggerReplacementPlan(empty, f.doc, f.catalog, limits).Succeeded(), "default opaque plan has no completion authority");
    const auto page = QueryTriggerReplace(c, c.revision, query, limits);
    Require(page.Succeeded() && page.value->next, "actual bounded first page with two matches");
    const std::vector<std::string> keys{page.value->rows.front().rowKey};
    const auto plan = PrepareTriggerReplacementPlan(c, c.revision, query, page.value->proof, keys, f.New(), limits);
    Require(plan.Succeeded() && plan.value->plan && !plan.value->plan->Empty() && plan.value->rows.size() == 1,
        "sync qualification yields owned opaque plan without publishing");
    const auto completed = CompleteTriggerReplacementPlan(*plan.value->plan, f.doc, f.catalog, limits);
    Require(completed.Succeeded() && completed.value->preview && completed.value->cost.ownedInputConsumed
        && completed.value->cost.candidateCopies == 0 && completed.value->cost.identityCaptures == 2
        && !completed.value->cost.workCoverageComplete && completed.value->cost.workUnitsCharged > limits.maxPreviewWorkUnits
        && completed.value->cost.workPolicy == TriggerScalarWorkPolicy::BoundedSourcesV2,
        "worker consumes one owned snapshot and explicitly does not claim legacy work-gate acceptance or RSS");
    Require(CheckTriggerReplacementPublicationContext(c, c.revision, *completed.value->preview, limits).Succeeded()
        && VerifyTriggerReplacementPreview(c, c.revision, *completed.value->preview, limits).Succeeded(),
        "context-only and existing strong public verifier keep different contracts");
    auto publication = f.doc;
    Require(ReplayTriggerGraphScalarEdit(publication, f.catalog, completed.value->preview->ScalarReceipt(), true).Succeeded()
        && ReplayTriggerGraphScalarEdit(publication, f.catalog, completed.value->preview->ScalarReceipt(), false).Succeeded(), "actual completion receipt shares atomic forward and inverse engine");
    const auto restored = WtgWctCodec{}.Serialize(publication, f.catalog);
    Require(restored.Succeeded() && restored.value->wtg == original.wtg && restored.value->wct == original.wct, "whole original WTG WCT bytes roundtrip after owned completion");
    auto changedLimits = limits; ++changedLimits.pageLimit;
    Require(!CompleteTriggerReplacementPlan(*plan.value->plan, f.doc, f.catalog, changedLimits).Succeeded(), "every plan limit is sealed across worker completion");
    TriggerReplaceLimits legacy = limits; legacy.scalarWorkPolicy = TriggerScalarWorkPolicy::LegacyKnownWork;
    Require(!CompleteTriggerReplacementPlan(*plan.value->plan, f.doc, f.catalog, legacy).Succeeded(), "policy cannot be changed on an existing plan");
    Require(!QueryTriggerReplace(c, c.revision, query, legacy, &*page.value->next).Succeeded(), "page cursor hash binds resource policy");
    const auto legacyPage = QueryTriggerReplace(c, c.revision, query, legacy);
    Require(legacyPage.Succeeded() && !PrepareTriggerReplacementPlan(c, c.revision, query, legacyPage.value->proof, keys, f.New(), legacy).Succeeded(),
        "legacy actual known-work budget still fails a deliberately low gate");
    auto selectedDrift = f.doc;
    Require(TriggerDocumentEditor::SetScalarArgument(selectedDrift, selectedId, 3, "8").Succeeded(), "actual selected original drift");
    Require(!CompleteTriggerReplacementPlan(*plan.value->plan, std::move(selectedDrift), f.catalog, limits).Succeeded(), "worker rejects different selected owned input");
    auto selectedMetadata = f.doc; ++selectedMetadata.argumentNodes.front().origin.offset;
    Require(!CompleteTriggerReplacementPlan(*plan.value->plan, std::move(selectedMetadata), f.catalog, limits).Succeeded(), "selected same-file-byte metadata drift is included in plan input seal");
    auto wrongSource = WtgWctCodec{}.CreateEmpty(f.catalog); Require(wrongSource.Succeeded(), "actual foreign empty trusted document");
    Require(!CompleteTriggerReplacementPlan(*plan.value->plan, std::move(*wrongSource.value), f.catalog, limits).Succeeded(), "wrong owned structure cannot use borrowed plan authority");
    std::stop_source cancelled; cancelled.request_stop();
    Require(!CompleteTriggerReplacementPlan(*plan.value->plan, f.doc, f.catalog, limits, cancelled.get_token()).Succeeded(), "pre-cancelled worker never creates publishable preview");
    auto unselectedMetadata = f.doc; unselectedMetadata.wct.customComment.text = "另一个私有快照说明";
    const auto anotherOwned = CompleteTriggerReplacementPlan(*plan.value->plan, std::move(unselectedMetadata), f.catalog, limits);
    Require(anotherOwned.Succeeded() && anotherOwned.value->preview, "completion proves its owned input, not unseen live global metadata");
    Require(CheckTriggerReplacementPublicationContext(c, c.revision, *anotherOwned.value->preview, limits).Succeeded()
        && !VerifyTriggerReplacementPreview(c, c.revision, *anotherOwned.value->preview, limits).Succeeded(), "context-only succeeds without claiming whole graph; strong verifier refuses different live WCT");
    Require(!ReplayTriggerGraphScalarEdit(f.doc, f.catalog, anotherOwned.value->preview->ScalarReceipt(), true).Succeeded(), "unique final Core replay rejects different live whole identity without writes");
    auto revision = c; revision.revision = "generation=7;revision=20";
    Require(!CheckTriggerReplacementPublicationContext(revision, revision.revision, *completed.value->preview, limits).Succeeded(), "current publication stamp changed invalidates plan result");
    const auto after = f.Encoded(); Require(after.wtg == original.wtg && after.wct == original.wct, "all async staging failures and successful private preparation preserve original source bytes");
}
void TestForgedProofIsRejectedBeforeOwnedCopy() {
    Fixture f; f.Add("StoreInt", 1, "Old"); const auto original = f.Encoded();
    const auto context = f.Context(); const auto query = f.Variables();
    TriggerReplaceLimits limits; limits.maxPreviewDocumentBytes = 64 * 1024;
    const auto page = QueryTriggerReplace(context, context.revision, query, limits);
    Require(page.Succeeded() && page.value->rows.size() == 1, "small capped actual query control");
    const std::vector<std::string> keys{page.value->rows.front().rowKey};
    const auto genuine = PrepareTriggerReplacementPlan(context, context.revision, query, page.value->proof, keys, f.New(), limits);
    Require(genuine.Succeeded() && genuine.value->plan, "small cap admits actual qualification control");
    for (const auto member : {&TriggerReplaceQueryProof::contextRevision, &TriggerReplaceQueryProof::requestSha256,
        &TriggerReplaceQueryProof::snapshotSha256, &TriggerReplaceQueryProof::sourceWtgSha256,
        &TriggerReplaceQueryProof::sourceWctSha256, &TriggerReplaceQueryProof::catalogSha256}) {
        auto forged = page.value->proof;
        forged.*member = std::string(limits.maxPreviewDocumentBytes + 1, 'x');
        const auto rejected = PrepareTriggerReplacementPlan(context, context.revision, query, forged, keys, f.New(), limits);
        Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
            && rejected.diagnostics.front().code == "TRIGGER_REPLACE_PREVIEW_STALE",
            "each public proof string beyond owned cap fails actual comparison before copying into a result or plan");
        const auto synchronous = PrepareTriggerReplacement(context, context.revision, query, forged, keys, f.New(), limits);
        Require(!synchronous.Succeeded() && !synchronous.value && !synchronous.diagnostics.empty()
            && synchronous.diagnostics.front().code == "TRIGGER_REPLACE_PREVIEW_STALE", "sync adapter shares strict proof admission");
    }
    const auto after = f.Encoded(); Require(after.wtg == original.wtg && after.wct == original.wct,
        "forged proof admission never modifies trigger source bytes");
}
void TestCatalogPrivateOwnedMetadataIsCharged() {
    Fixture f; f.Add("StoreInt", 1, "Old"); const auto original = f.Encoded();
    const auto catalogSha = f.catalog.Sha256();
    const auto configBytes = f.catalog.ConfigDocument().SerializeLossless();
    const auto actualRoot = f.root / "ui";
    std::size_t expected = actualRoot.native().size() * sizeof(std::filesystem::path::value_type);
    constexpr auto pairInline = sizeof(std::pair<const std::string, UiDefinition>) - sizeof(UiDefinition);
    std::size_t keys = 0;
    for (const auto kind : {UiDefinitionKind::Define, UiDefinitionKind::Event, UiDefinitionKind::Condition,
        UiDefinitionKind::Action, UiDefinitionKind::Call}) {
        for (const auto* row : f.catalog.Definitions(kind)) {
            Require(row != nullptr, "actual catalog row for private-key oracle");
            std::string key(ToString(kind)); key.push_back('\0'); key += row->group; key.push_back('\0'); key += row->name;
            expected += pairInline + key.size(); ++keys;
        }
    }
    Require(keys != 0 && expected > actualRoot.native().size() * sizeof(std::filesystem::path::value_type),
        "fixture contains separate actual map keys beyond private root bytes");
    const auto actual = f.catalog.AdditionalOwnedBytes(std::numeric_limits<std::size_t>::max());
    Require(actual && *actual == expected && f.catalog.AdditionalOwnedBytes(expected) == actual,
        "private root native bytes plus real separate key strings and inline pair remainder are exact at boundary");
    Require(!f.catalog.AdditionalOwnedBytes(expected - 1) && !f.catalog.AdditionalOwnedBytes(0),
        "private owned metadata rejects before overflow or cap excess");
    const auto copiedCatalog = f.catalog;
    Require(copiedCatalog.AdditionalOwnedBytes(expected) == actual && copiedCatalog.Sha256() == catalogSha,
        "deep-copied catalog retains actual private bytes without changing source identity");
    auto context = f.Context(); const auto query = f.Variables(); TriggerReplaceLimits limits;
    limits.maxPreviewDocumentBytes = expected - 1;
    const auto page = QueryTriggerReplace(context, context.revision, query, limits);
    Require(page.Succeeded() && page.value->rows.size() == 1, "query does not clone owned catalog");
    const std::vector<std::string> selection{page.value->rows.front().rowKey};
    const auto rejected = PrepareTriggerReplacementPlan(context, context.revision, query, page.value->proof, selection, f.New(), limits);
    Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
        && rejected.diagnostics.front().code == "TRIGGER_REPLACE_PLAN_CATALOG_BUDGET",
        "GUI plan rejects actual private catalog bytes before permitting owned snapshot clone");
    const auto after = f.Encoded(); Require(after.wtg == original.wtg && after.wct == original.wct
        && f.catalog.Sha256() == catalogSha && f.catalog.ConfigDocument().SerializeLossless() == configBytes,
        "catalog accounting and refusal do not modify graph or original catalog source bytes");
}
void TestNonmatchingTypesDoNotAlterPublicationContextQualification() {
    Fixture f;
    for (unsigned i = 0; i < 256; ++i) f.Add("StoreInt", 1, "Old");
    for (unsigned i = 0; i < 24; ++i) {
        f.Add("StoreString", 3, "not matched string");
        f.Add("ScriptSlot", 3, "not matched code");
    }
    const auto objectArgument = f.Add("StoreUnit", 3, "hfoo");
    const auto baseline = f.Encoded(); const auto sourceWtg = std::string(f.doc.SourceWtgSha256());
    const auto sourceWct = std::string(f.doc.SourceWctSha256());
    auto context = f.Context(); TriggerReplaceLimits limits; limits.pageLimit = 37;
    for (const auto mode : {TriggerReplaceMode::ParameterValue, TriggerReplaceMode::Variable}) {
        TriggerReplaceQuery query; query.mode = mode; query.needle = "Old"; query.match = TriggerReplaceMatch::Exact;
        std::optional<TriggerReplaceCursor> cursor; TriggerReplaceQueryProof proof; std::vector<std::string> keys;
        do {
            const auto page = QueryTriggerReplace(context, context.revision, query, limits, cursor ? &*cursor : nullptr);
            Require(page.Succeeded() && page.value->coverage.totalMatches == 256, "mixed nonmatches preserve exact normal query count");
            if (keys.empty()) proof = page.value->proof; else Require(page.value->proof == proof, "same proof across mixed-type pages");
            for (const auto& row : page.value->rows) {
                Require(row.oldValue == "Old" && row.declaredType == "integer"
                    && row.eligibility == TriggerReplaceEligibility::PreviewRequired && row.reasonCode.empty(),
                    "actual matched rows retain declared type and all eligibility checks");
                keys.push_back(row.rowKey);
            }
            cursor = page.value->next;
        } while (cursor);
        Require(keys.size() == 256, "actual cross-page256 selection is retained");
        const auto preview = PrepareTriggerReplacement(context, context.revision, query, proof, keys, f.New(), limits);
        Require(preview.Succeeded() && preview.value->preview && preview.value->rows.size() == 256, "one actual typed256 planner still qualifies");
        Require(CheckTriggerReplacementPublicationContext(context, context.revision, *preview.value->preview, limits).Succeeded(),
            "real Apply context entry also uses the same matching/type path");
        const auto oldSignature = f.doc.ecaNodes[256].uiSignature;
        f.doc.ecaNodes[256].uiSignature += "/nonmatching source drift";
        const auto stale = CheckTriggerReplacementPublicationContext(context, context.revision, *preview.value->preview, limits);
        Require(!stale.Succeeded() && !stale.diagnostics.empty() && stale.diagnostics.front().code == "TRIGGER_REPLACE_PREVIEW_STALE",
            "nonmatching metadata remains in complete snapshot proof and cannot publish stale256");
        f.doc.ecaNodes[256].uiSignature = oldSignature;
    }
    TriggerReplaceQuery objects; objects.mode = TriggerReplaceMode::ObjectReference; objects.needle = "hfoo";
    objects.match = TriggerReplaceMatch::Exact;
    const auto objectPage = QueryTriggerReplace(context, context.revision, objects, limits);
    Require(objectPage.Succeeded() && objectPage.value->rows.size() == 1
        && objectPage.value->rows.front().argumentId == objectArgument
        && objectPage.value->rows.front().declaredType == "unitcode"
        && objectPage.value->rows.front().eligibility == TriggerReplaceEligibility::ReadOnly
        && objectPage.value->rows.front().reasonCode == "TRIGGER_REPLACE_OBJECT_SOURCE_UNKNOWN",
        "object matching keeps early actual declared type and unknown-source refusal");
    const auto after = f.Encoded(); Require(after.wtg == baseline.wtg && after.wct == baseline.wct
        && f.doc.SourceWtgSha256() == sourceWtg && f.doc.SourceWctSha256() == sourceWct,
        "all readonly matching/planning/context refusals retain exact source wire/seal");
}
#include "TriggerReplaceWriterTests.inl"
} // namespace
int main() {
    try {
        TestMultiPageAtomicCandidateAndSourceUnchanged(); TestWtsVisibleTextFullDomainAndPartial();
        TestTypesCodeAndObjectDomainsRemainSealed(); TestCursorShapeCoverageAndBudgets(); TestOptionsAndScopeDoNotGuess();
        TestPreparedLinearMatcherAndCachedWtsWorstPrefix(); TestTargetAndCodeDriftCannotPublishOldCandidate();
        TestWtsFallbackAliasesNeverShareRawMatches(); TestRealMixedWtsLocalEncodingAndSourcePreservation(); TestRealSealedPreplacedObjectReferencesAndMissingSource();
        TestRealPartialObjectDomainNeverGrantsUniqueTarget();
        TestStrictSharedUtf8InputPolicy();
        TestOwnedPlanCompletionPoliciesAndDeferredWholeGraph();
        TestForgedProofIsRejectedBeforeOwnedCopy();
        TestCatalogPrivateOwnedMetadataIsCharged();
        TestScannerAdmittedIdentityCapacityPreservesProofAndRejections();
        TestStreamWriterOriginalFramingAndBoundaries();
        TestStreamWriterDoesNotOwnInputPayload();
        TestPublicQueryAndOptionsKeepOriginalSnapshotProofs();
        TestLazyWriterTinyAllocationAndOriginalFlushBoundary();
        TestNonmatchingTypesDoNotAlterPublicationContextQualification();
        std::cout << "trigger replacement service tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
