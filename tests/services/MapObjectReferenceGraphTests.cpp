#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"
#include "../MapDataFixtures.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using namespace War3Map::Trigger;
constexpr std::string_view kUnit = "gg_unit_I001_0017";
constexpr std::string_view kOtherUnit = "gg_unit_I002_0019";
constexpr std::string_view kItem = "gg_item_H001_0018";

void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view value) {
    std::vector<std::byte> result;
    for (const unsigned char byte : value) result.push_back(static_cast<std::byte>(byte));
    return result;
}
template<class T> bool HasCode(const Result<T>& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
void Write(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary); output << value;
    Require(output.good(), "write original catalog fixture");
}
GraphRecordRef Ref(GraphRecordKind kind, StableId id) { return {kind, id, {}}; }
GraphArgumentValue Scalar(std::string value, std::int32_t kind = 1) {
    GraphArgumentValue result; result.scalar = {kind, std::move(value)}; return result;
}

// 使用原创真实doo及正向分类表，绝不构造、反序列化或修改密封引用列表。
std::shared_ptr<const MapObjectReferenceSnapshot> Snapshot(float unitX = 0, std::string source = "fixture-standard-a") {
    auto placements = MapData::ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11());
    Require(placements.Succeeded(), "read original empty doo fixture");
    MapData::UnitPlacement unit; unit.rawcode = "I001"; unit.creationId = 17;
    unit.scale = {1, 1, 1}; unit.position[0] = unitX; placements.value->units.push_back(unit);
    unit.rawcode = "H001"; unit.creationId = 18; placements.value->units.push_back(unit);
    unit.rawcode = "I002"; unit.creationId = 19; placements.value->units.push_back(unit);
    const auto bytes = placements.value->Serialize(); Require(bytes.Succeeded(), "encode real doo records");
    const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", *bytes.value}};
    const auto map = MapDocument::FromMembers(members, "fixture-map.w3x");
    Require(map.Member(MapDataKind::Units).state == MapDataState::Ready, "real map document qualifies doo layout");
    const MapObjectReferenceCapture::Reader absentNative = [](std::string_view name) {
        return Result<MapObjectReferenceSource>::Success({"fixture-map/" + std::string(name), {}});
    };
    const MapObjectReferenceCapture::Reader standard = [source = std::move(source)](std::string_view name) {
        std::string text = "ID;PWXL\nC;X1;Y1;K\"alias\"\n";
        if (name == "Units/UnitData.slk") text += "C;X1;Y2;K\"I001\"\nC;X1;Y3;K\"I002\"\n";
        else if (name == "Units/ItemData.slk") text += "C;X1;Y2;K\"H001\"\n";
        else return Result<MapObjectReferenceSource>::Failure("Test", "unexpected-source", std::string(name));
        text += "E\n";
        return Result<MapObjectReferenceSource>::Success({source + '/' + std::string(name), Bytes(text)});
    };
    const auto capture = MapObjectReferenceCapture::Capture(map, absentNative, standard);
    Require(capture.Succeeded() && capture.value && *capture.value, "real positive classification capture");
    Require((*capture.value)->Find(kUnit) && (*capture.value)->Find(kOtherUnit) && (*capture.value)->Find(kItem),
        "unit and item eligibility comes from explicit source roles, not rawcode prefixes");
    return *capture.value;
}

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Forge-MapObjectGraph-"
        + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog;
    TriggerDocument document;
    Fixture() {
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n"
            "unit=0,1,0,Unit,unit\nitem=0,1,0,Item,item\ntrigger=0,1,0,Trigger,trigger\n");
        Write(root / "ui/base/action.txt", "[SetVariable]\n[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n"
            "[UseUnit]\n[[.args]]\ntype=unit\n[UseItem]\n[[.args]]\ntype=item\n"
            "[UseWidget]\n[[.args]]\ntype=widget\n[UseHandle]\n[[.args]]\ntype=handle\n"
            "[UseTrigger]\n[[.args]]\ntype=trigger\n[UseInteger]\n[[.args]]\ntype=integer\n");
        Write(root / "ui/base/call.txt", "[IdentityUnit]\nreturns=unit\n[[.args]]\ntype=unit\n");
        auto loaded = UiCatalog::Load(root / "ui"); Require(loaded.Succeeded(), "load original typed catalog"); catalog = std::move(*loaded.value);
        auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "new sealed document"); document = std::move(*empty.value);
        const auto category = TriggerDirectoryEditor::AddCategory(document, "Original"); Require(category.Succeeded(), "category");
        Require(TriggerDocumentEditor::AddTrigger(document, *category.value, "Dispatch").Succeeded(), "real trigger");
        Require(TriggerDocumentEditor::AddVariable(document, "Target", "unit").Succeeded(), "unit declaration");
        Require(TriggerDocumentEditor::AddVariable(document, "ItemTarget", "item").Succeeded(), "item declaration");
        Require(TriggerDocumentEditor::AddVariable(document, "Array", "unit", true).Succeeded(), "array declaration");
        Require(TriggerDocumentEditor::AddVariable(document, "Counter", "integer").Succeeded(), "integer declaration");
        const auto encoded = WtgWctCodec{}.Serialize(document, catalog); Require(encoded.Succeeded(), "pair before codec seal");
        auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog);
        Require(parsed.Succeeded(), "real pair source seal"); document = std::move(*parsed.value);
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    TriggerGraphEditBatch Batch(const MapObjectReferenceSnapshot* snapshot) const {
        const auto binding = CaptureTriggerGraphBinding(document, catalog, snapshot);
        Require(binding.Succeeded(), "graph binding"); return {*binding.value, {}};
    }
    GraphInsertSubtree Insert(std::string function, std::vector<GraphArgumentValue> arguments) const {
        GraphParentExpectation parent{GraphParentKind::TriggerEcas, Ref(GraphRecordKind::Trigger, document.triggers[0].id), {}, {}};
        for (const auto index : document.triggers[0].ecas) parent.orderedChildren.push_back(Ref(GraphRecordKind::Eca, document.ecaNodes[index].id));
        auto eca = std::make_shared<GraphEcaValue>(); eca->function = std::move(function); eca->arguments = std::move(arguments);
        return {std::move(parent), {}, std::move(eca)};
    }
    Result<TriggerGraphEditReceipt> Append(std::string function, std::vector<GraphArgumentValue> arguments,
        const MapObjectReferenceSnapshot* snapshot) {
        auto batch = Batch(snapshot); batch.operations.emplace_back(Insert(std::move(function), std::move(arguments)));
        return ApplyTriggerGraphEditBatch(document, catalog, batch, {}, snapshot);
    }
    StableId LastArgument(std::size_t position = 0) const {
        const auto& eca = document.ecaNodes[document.triggers[0].ecas.back()];
        return document.argumentNodes[eca.arguments[position]].id;
    }
};

void TypedReferencesAndAssignment() {
    Fixture fixture; const auto snapshot = Snapshot();
    Require(fixture.Append("SetVariable", {Scalar("Target"), Scalar(std::string(kUnit))}, snapshot.get()).Succeeded(),
        "real preplaced unit is accepted on ordinary T assignment RHS");
    Require(fixture.Append("SetVariable", {Scalar("ItemTarget"), Scalar(std::string(kItem))}, snapshot.get()).Succeeded(),
        "real preplaced item is accepted on ordinary T assignment RHS");
    for (const auto* slot : {"UseUnit", "UseWidget", "UseHandle"})
        Require(fixture.Append(slot, {Scalar(std::string(kUnit))}, snapshot.get()).Succeeded(), "unit follows native ancestor typing");
    Require(fixture.Append("UseItem", {Scalar(std::string(kItem))}, snapshot.get()).Succeeded(), "real item read is typed");
    Require(fixture.Append("UseTrigger", {Scalar("gg_trg_Dispatch")}, nullptr).Succeeded(), "actual document generated trigger remains readable");
    Require(fixture.Append("UseUnit", {Scalar("udg_Target")}, nullptr).Succeeded(), "udg alias remains a declared reference");
    auto indexed = Scalar("udg_Array"); indexed.index = std::make_shared<GraphArgumentValue>(Scalar("1", 3));
    Require(fixture.Append("UseUnit", {indexed}, snapshot.get()).Succeeded(), "declared array alias retains valid index support");

    const auto baseline = fixture.Batch(snapshot.get()).expected;
    const auto rejects = [&](std::string function, std::vector<GraphArgumentValue> arguments,
        const MapObjectReferenceSnapshot* context, std::string_view code) {
        const auto failed = fixture.Append(std::move(function), std::move(arguments), context);
        Require(!failed.Succeeded() && HasCode(failed, code), "unqualified reference is rejected with diagnostic");
        const auto after = fixture.Batch(snapshot.get()).expected;
        Require(after.currentWtgSha256 == baseline.currentWtgSha256 && after.currentWctSha256 == baseline.currentWctSha256,
            "rejected reference publishes no T or code change");
    };
    rejects("UseUnit", {Scalar(std::string(kUnit))}, nullptr, "TRIGGER_GRAPH_VARIABLE_UNKNOWN");
    rejects("UseUnit", {Scalar("gg_unit_I001_9999")}, snapshot.get(), "TRIGGER_GRAPH_VARIABLE_UNKNOWN");
    rejects("UseUnit", {Scalar("gg_unit_XXXX_0017")}, snapshot.get(), "TRIGGER_GRAPH_VARIABLE_UNKNOWN");
    rejects("UseUnit", {Scalar("udg_" + std::string(kUnit))}, snapshot.get(), "TRIGGER_GRAPH_VARIABLE_UNKNOWN");
    rejects("UseItem", {Scalar(std::string(kUnit))}, snapshot.get(), "TRIGGER_GRAPH_TYPE_MISMATCH");
    rejects("SetVariable", {Scalar(std::string(kUnit)), Scalar("Target")}, snapshot.get(), "TRIGGER_WRITABLE_VARIABLE_UNKNOWN");
    indexed = Scalar(std::string(kUnit)); indexed.index = std::make_shared<GraphArgumentValue>(Scalar("1", 3));
    rejects("UseUnit", {indexed}, snapshot.get(), "TRIGGER_GRAPH_INDEX_INVALID");

    const TriggerTypeSystem types(fixture.catalog, snapshot.get());
    ArgumentNode reference; reference.type = 1;
    reference.value.text = std::string(kUnit); Require(types.ValueType(fixture.document, reference) == "unit", "value type sees real unit");
    reference.value.text = "gg_trg_Dispatch"; Require(types.ValueType(fixture.document, reference) == "trigger", "value type sees real generated trigger");
    reference.value.text = "udg_Target"; Require(types.ValueType(fixture.document, reference) == "unit", "value type sees exact alias");
    reference.value.text = "gg_unit_missing_0017"; Require(types.ValueType(fixture.document, reference).empty(), "unknown value has no fabricated type");
}

void AmbiguousReferences() {
    Fixture fixture; const auto snapshot = Snapshot();
    const auto ambiguous = [&](const TriggerDocument& document, std::string_view name) {
        const auto result = ResolveVariableReference(document, name, snapshot.get());
        Require(!result.Succeeded() && HasCode(result, "TRIGGER_GRAPH_VARIABLE_AMBIGUOUS"), "all multiple candidates reject");
    };
    auto duplicate = fixture.document; duplicate.variables.push_back(duplicate.variables[0]); ambiguous(duplicate, "Target");
    duplicate = fixture.document; duplicate.variables.push_back(duplicate.variables[0]); duplicate.variables.back().name.text = "udg_Target";
    ambiguous(duplicate, "udg_Target");
    duplicate = fixture.document; duplicate.variables[0].name.text = std::string(kUnit); ambiguous(duplicate, kUnit);
    duplicate = fixture.document; duplicate.variables[0].name.text = "gg_trg_Dispatch"; ambiguous(duplicate, "gg_trg_Dispatch");
    duplicate = fixture.document; duplicate.triggers.push_back(duplicate.triggers[0]); ambiguous(duplicate, "gg_trg_Dispatch");
    duplicate = fixture.document; duplicate.triggers[0].name.text = "Dispatch-Logic";
    duplicate.triggers.push_back(duplicate.triggers[0]); duplicate.triggers.back().name.text = "Dispatch_Logic";
    ambiguous(duplicate, "gg_trg_Dispatch_Logic");
    duplicate = fixture.document; duplicate.variables[0].name.text = "udg_" + std::string(kUnit);
    const auto distinct = ResolveVariableReference(duplicate, duplicate.variables[0].name.text, snapshot.get());
    Require(distinct.Succeeded() && distinct.value->kind == VariableReferenceKind::Declared,
        "udg-prefixed declaration is not confused with a bare preplaced global");
    duplicate = fixture.document; duplicate.triggers[0].type = 1;
    Require(!ResolveVariableReference(duplicate, "gg_trg_Dispatch", snapshot.get()).Succeeded(), "comment does not generate trigger global");
    duplicate = fixture.document; duplicate.triggers[0].enabled = 0;
    Require(!ResolveVariableReference(duplicate, "gg_trg_Dispatch", snapshot.get()).Succeeded(), "disabled T does not generate trigger global");
    duplicate.triggers[0].enabled = 1; duplicate.triggers[0].initiallyClosed = 1;
    Require(ResolveVariableReference(duplicate, "gg_trg_Dispatch", snapshot.get()).Succeeded(), "initially closed T still generates a global");
}

void ScalarEditAndGuardDependencies() {
    Fixture fixture; const auto first = Snapshot(), moved = Snapshot(128), otherSource = Snapshot(0, "fixture-standard-b");
    Require(first->Identity() != moved->Identity() && first->Identity() != otherSource->Identity(), "member bytes and classification source bind identity");
    Require(fixture.Append("UseUnit", {Scalar("Target")}, first.get()).Succeeded(), "ordinary old scalar");
    const auto argumentId = fixture.LastArgument();
    const auto ordinary = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId);
    const auto ordinaryWithSources = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId, {}, first.get());
    const auto ordinaryMoved = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId, {}, moved.get());
    Require(ordinary.Succeeded() && ordinaryWithSources.Succeeded() && ordinaryMoved.Succeeded()
        && *ordinary.value == *ordinaryWithSources.value && *ordinary.value == *ordinaryMoved.value,
        "unused map snapshot leaves legacy nonpreplaced guard unchanged");

    auto scalar = fixture.Batch(first.get());
    scalar.operations.emplace_back(GraphSet{Ref(GraphRecordKind::Argument, argumentId), GraphField::ArgumentScalar,
        GraphScalarValue{1, "Target"}, GraphScalarValue{1, std::string(kUnit)}});
    Require(ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, scalar, {}, first.get()).Succeeded(),
        "scalar edit consumes the same captured reference qualification");
    const auto guard = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId, {}, first.get());
    const auto movedGuard = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId, {}, moved.get());
    const auto sourceGuard = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId, {}, otherSource.get());
    Require(guard.Succeeded() && movedGuard.Succeeded() && sourceGuard.Succeeded()
        && guard.value->subtreeSha256 != movedGuard.value->subtreeSha256
        && guard.value->subtreeSha256 != sourceGuard.value->subtreeSha256, "old preplaced closure includes actual captured dependencies");
    Require(!CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId).Succeeded(), "existing preplaced closure cannot be guarded without sources");

    auto batch = fixture.Batch(first.get());
    batch.operations.emplace_back(GraphReplaceArgument{Ref(GraphRecordKind::Argument, argumentId), *guard.value, Scalar(std::string(kOtherUnit))});
    const auto before = fixture.Batch(first.get()).expected;
    const auto staleBinding = ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, batch, {}, moved.get());
    Require(!staleBinding.Succeeded() && HasCode(staleBinding, "TRIGGER_GRAPH_SOURCE_STALE"), "new snapshot rejects old graph binding");
    batch.expected = fixture.Batch(moved.get()).expected;
    const auto staleGuard = ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, batch, {}, moved.get());
    Require(!staleGuard.Succeeded() && HasCode(staleGuard, "TRIGGER_GRAPH_ARGUMENT_STALE"), "new graph binding cannot hide a stale closure guard");
    const auto after = fixture.Batch(first.get()).expected;
    Require(before.currentWtgSha256 == after.currentWtgSha256 && before.currentWctSha256 == after.currentWctSha256,
        "both stale paths keep complete document unchanged");
    batch.expected = fixture.Batch(first.get()).expected;
    const auto replaced = ApplyTriggerGraphEditBatch(fixture.document, fixture.catalog, batch, {}, first.get());
    Require(replaced.Succeeded() && replaced.value->argumentReplacements.size() == 1, "valid preplaced old-closure replacement succeeds");
    const auto finalGuard = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, argumentId, {}, first.get());
    Require(finalGuard.Succeeded() && finalGuard.value->subtreeSha256 == replaced.value->argumentReplacements[0].afterSubtreeSha256,
        "postreplacement fingerprint uses identical snapshot and resolver");

    // 闭包中深层函数的对象引用也必须绑定来源，不能只检查根参数值。
    GraphArgumentValue nested; auto call = std::make_shared<GraphEcaValue>(); call->function = "IdentityUnit";
    call->kind = UiDefinitionKind::Call; call->arguments = {Scalar(std::string(kUnit))}; nested.call = call;
    Require(fixture.Append("UseUnit", {nested}, first.get()).Succeeded(), "nested typed function retains real object reference");
    const auto nestedId = fixture.LastArgument();
    const auto nestedGuard = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, nestedId, {}, first.get());
    const auto nestedMoved = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, nestedId, {}, moved.get());
    Require(nestedGuard.Succeeded() && nestedMoved.Succeeded() && nestedGuard.value->subtreeSha256 != nestedMoved.value->subtreeSha256,
        "nested preplaced dependencies participate in the closure guard");
    Require(fixture.Append("UseInteger", {Scalar("7", 3)}, nullptr).Succeeded(), "ordinary literal fixture");
    const auto literalId = fixture.LastArgument();
    const auto literal = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, literalId);
    const auto literalWithSources = CaptureArgumentReplacementGuard(fixture.document, fixture.catalog, literalId, {}, first.get());
    Require(literal.Succeeded() && literalWithSources.Succeeded() && *literal.value == *literalWithSources.value,
        "literal closure hash is not revised by optional captured sources");
}
} // namespace

int main() {
    try {
        TypedReferencesAndAssignment();
        AmbiguousReferences();
        ScalarEditAndGuardDependencies();
        std::cout << "real map object graph references passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
