#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& file, std::string_view text) {
    std::filesystem::create_directories(file.parent_path()); std::ofstream stream(file, std::ios::binary);
    stream << text; Require(stream.good(), "write synthetic catalog");
}
GraphRecordRef Id(GraphRecordKind kind, StableId id) { return {kind, id, {}}; }
GraphRecordRef Local(GraphRecordKind kind, std::string key) { return {kind, 0, std::move(key)}; }
GraphParentExpectation Parent(GraphParentKind kind, GraphRecordRef owner = {}, std::vector<GraphRecordRef> children = {}) {
    return {kind, std::move(owner), std::move(children), {}};
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Forge-GraphEdit-"
        + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog; TriggerDocument doc;
    Fixture() {
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\nstring=0,1,1,String,string\ntrigger=0,1,0,Trigger,trigger\n"
            "abilcode=0,1,1,Ability,integer\nabilityalias=0,1,1,Alias,abilcode\nabilityalias2=0,1,1,Alias2,abilityalias\n"
            "unit=0,1,1,Unit,unit\ncustomcode=0,1,1,PlainInteger,integer\nComparisonOperator=0,0,0,Operator\n"
            "integervar=1,0,0,IntegerVariable,integer\n"
            "scriptcode=0,0,0,Script,string\ntypename=0,0,0,Type\n"
            "[TriggerParams]\nOperatorGreater=0,ComparisonOperator,>,Greater\nAbilityPreset=0,abilcode,'A0E0',Ability\n"
            "typename_unit=0,typename,unit,Unit\ntypename_integer=0,typename,integer,Integer\nLoopPreset=0,integer,1,One\n");
        Write(root / "ui/base/action.txt", "[Block]\n[StoreInteger]\n[[.args]]\ntype=integer\n"
            "[UseAbilityAlias]\n[[.args]]\ntype=abilityalias2\n[UseCustomCode]\n[[.args]]\ntype=customcode\n"
            "[WithCodeSibling]\n[[.args]]\ntype=unit\n[[.args]]\ntype=scriptcode\n"
            "[CustomScriptCode]\n[[.args]]\ntype=string\n[StoreString]\n[[.args]]\ntype=string\n"
            "[DynamicStore]\n[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n"
            "[SelectedStore]\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n"
            "[ForLoopVarMultiple]\n[[.args]]\ntype=integervar\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
            "[ForLoopVar]\n[[.args]]\ntype=integervar\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=code\n"
            "[AddTriggerEvent]\n[[.args]]\ntype=trigger\n[[.args]]\ntype=eventcall\n[UseFilter]\n[[.args]]\ntype=boolexpr\n");
        Write(root / "ui/base/event.txt", "[TriggerRegisterUnitEvent]\n[[.args]]\ntype=unit\n");
        Write(root / "ui/base/call.txt", "[ReadInteger]\nreturns=integer\n[AddInteger]\nreturns=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n[ReadString]\nreturns=string\n"
            "[GetTriggerUnit]\nreturns=unit\n[GetUnitAbilityLevel]\nreturns=integer\n[[.args]]\ntype=unit\n[[.args]]\ntype=abilcode\n"
            "[IdentityUnit]\nreturns=unit\n[[.args]]\ntype=unit\n[ReadCode]\nreturns=integer\n[[.args]]\ntype=scriptcode\n"
            "[DispatchAlias]\nreturns=string\nscript_name=ExecuteFunc\n[[.args]]\ntype=string\n");
        Write(root / "ui/base/condition.txt", "[OperatorCompareInteger]\nreturns=boolean\n[[.args]]\ntype=integer\n[[.args]]\ntype=ComparisonOperator\n[[.args]]\ntype=integer\n");
        auto loaded = UiCatalog::Load(root / "ui"); Require(loaded.Succeeded(), "load catalog"); catalog = std::move(*loaded.value);
        auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "sealed empty doc"); doc = std::move(*empty.value);
        auto category = TriggerDirectoryEditor::AddCategory(doc, "Original"); Require(category.Succeeded(), "original category");
        auto trigger = TriggerDocumentEditor::AddTrigger(doc, *category.value, "OriginalTrigger"); Require(trigger.Succeeded(), "original trigger");
        const auto encoded = WtgWctCodec{}.Serialize(doc, catalog); Require(encoded.Succeeded(), "source encoded");
        const auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog); Require(parsed.Succeeded(), "actual pair seal"); doc = *parsed.value;
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    TriggerGraphEditBatch Batch() const { const auto binding = CaptureTriggerGraphBinding(doc, catalog); Require(binding.Succeeded(), "capture binding"); return {*binding.value, {}}; }
};
GraphArgumentValue Scalar(std::int32_t kind, std::string value) { GraphArgumentValue result; result.scalar = {kind, std::move(value)}; return result; }
std::shared_ptr<GraphEcaValue> Eca(std::string key, std::string function, UiDefinitionKind kind = UiDefinitionKind::Action) {
    auto result = std::make_shared<GraphEcaValue>(); result->clientKey = std::move(key); result->function = std::move(function); result->kind = kind; return result;
}
void TestRecursiveBatchAndAtomicNames() {
    Fixture f; auto batch = f.Batch(); const auto original = Id(GraphRecordKind::Category, f.doc.categories[0].id);
    batch.operations.emplace_back(GraphCreate{"folder", GraphCategoryValue{"System"}, Parent(GraphParentKind::Categories, {}, {original}), {}});
    batch.operations.emplace_back(GraphCreate{"array", GraphVariableValue{"Cache", "integer", true, 32, true, "0"}, Parent(GraphParentKind::Variables), {}});
    batch.operations.emplace_back(GraphCreate{"trigger", GraphTriggerValue{"Dispatcher", "Editable T", true, false, true}, Parent(GraphParentKind::CategoryTriggers, Local(GraphRecordKind::Category, "folder")), {}});
    auto block = Eca("block", "Block"); auto store = Eca("store", "StoreInteger"); store->childBranch = 0;
    auto array = Scalar(1, "Cache"); array.clientKey = "arrayArg";
    auto index = std::make_shared<GraphArgumentValue>(); index->clientKey = "index";
    index->call = Eca("indexCall", "ReadInteger", UiDefinitionKind::Call); array.index = index; store->arguments.push_back(array);
    auto storeCall = Eca("second", "StoreInteger"); storeCall->childBranch = 1;
    GraphArgumentValue sum; auto add = Eca("sum", "AddInteger", UiDefinitionKind::Call);
    add->arguments = {Scalar(3, "2"), Scalar(3, "5")}; sum.call = add; storeCall->arguments.push_back(sum);
    block->children = {store, storeCall};
    batch.operations.emplace_back(GraphInsertSubtree{Parent(GraphParentKind::TriggerEcas, Local(GraphRecordKind::Trigger, "trigger")), {}, block});
    const auto created = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch); Require(created.Succeeded(), "complete recursive batch");
    Require(created.value->created.size() == 10 && f.doc.ecaNodes.size() == 5 && f.doc.argumentNodes.size() == 5, "recursive identities/counts");
    auto* arrayArg = TriggerDocumentEditor::FindArgument(f.doc, created.value->created.at("arrayArg").id);
    Require(arrayArg && arrayArg->indexArgument && f.doc.argumentNodes[*arrayArg->indexArgument].callEca, "real index call graph");
    const auto arrayArgId = arrayArg->id;
    const auto variableId = created.value->created.at("array").id, triggerId = created.value->created.at("trigger").id;
    auto second = f.Batch(); second.operations.emplace_back(GraphRename{Id(GraphRecordKind::Variable, variableId), f.doc.variables[0].name, "RenamedCache", {}});
    second.operations.emplace_back(GraphRename{Id(GraphRecordKind::Trigger, triggerId), TriggerDocumentEditor::FindTrigger(f.doc, triggerId)->name, "NewDispatcher", {}});
    second.operations.emplace_back(GraphSet{Id(GraphRecordKind::Variable, variableId), GraphField::VariableArraySize, std::int32_t{32}, std::int32_t{64}});
    second.operations.emplace_back(GraphMove{Id(GraphRecordKind::Trigger, triggerId),
        Parent(GraphParentKind::CategoryTriggers, created.value->created.at("folder"), {Id(GraphRecordKind::Trigger, triggerId)}),
        Parent(GraphParentKind::CategoryTriggers, original, {Id(GraphRecordKind::Trigger, f.doc.triggers[0].id)}), Id(GraphRecordKind::Trigger, f.doc.triggers[0].id)});
    const auto changed = ApplyTriggerGraphEditBatch(f.doc, f.catalog, second); Require(changed.Succeeded(), "rename metadata and move");
    Require(changed.value->renames.size() == 2 && changed.value->renames[0].changedArgumentIds.size() == 1, "atomic reference receipt");
    Require(f.doc.triggers[0].id == triggerId && f.doc.variables[0].size == 64
        && TriggerDocumentEditor::FindArgument(f.doc, arrayArgId)->value.text == "RenamedCache", "reference/order metadata preserved");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog); Require(encoded.Succeeded(), "serialize recursive pair");
    const auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog); Require(parsed.Succeeded(), "recursive pair readback");
    Require(parsed.value->ecaNodes.size() == 5 && parsed.value->variables[0].name.text == "RenamedCache", "recursive semantics survive codec");
    const auto stale = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch); Require(!stale.Succeeded(), "stale overlay binding refuses replay");
    auto remove = f.Batch();
    remove.operations.emplace_back(GraphRemove{created.value->created.at("block"), "Block", Parent(GraphParentKind::TriggerEcas,
        Id(GraphRecordKind::Trigger, triggerId), {created.value->created.at("block")})});
    remove.operations.emplace_back(GraphRemove{Id(GraphRecordKind::Variable, variableId), "RenamedCache", Parent(GraphParentKind::Variables,
        {}, {Id(GraphRecordKind::Variable, variableId)})});
    remove.operations.emplace_back(GraphRemove{Id(GraphRecordKind::Trigger, triggerId), "NewDispatcher", Parent(GraphParentKind::CategoryTriggers,
        original, {Id(GraphRecordKind::Trigger, triggerId), Id(GraphRecordKind::Trigger, f.doc.triggers[1].id)})});
    remove.operations.emplace_back(GraphRemove{created.value->created.at("folder"), "System", Parent(GraphParentKind::Categories,
        {}, {original, created.value->created.at("folder")})});
    Require(ApplyTriggerGraphEditBatch(f.doc, f.catalog, remove).Succeeded(), "common remove reuses complete call/index compaction");
    Require(f.doc.ecaNodes.empty() && f.doc.argumentNodes.empty() && f.doc.variables.empty() && f.doc.triggers.size() == 1
        && f.doc.categories.size() == 1, "no recursive orphan after remove");
}
void TestFailurePublishesNothing() {
    Fixture f; const auto unchanged = f.Batch().expected; auto batch = f.Batch(); const auto category = Id(GraphRecordKind::Category, f.doc.categories[0].id);
    batch.operations.emplace_back(GraphCreate{"new", GraphTriggerValue{"NeverPublished"}, Parent(GraphParentKind::CategoryTriggers, category,
        {Id(GraphRecordKind::Trigger, f.doc.triggers[0].id)}), {}});
    batch.operations.emplace_back(GraphSet{Local(GraphRecordKind::Trigger, "new"), GraphField::Enabled, false, true});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded() && f.doc.triggers.size() == 1, "later stale field rolls whole batch back");
    Require(f.Batch().expected.currentWtgSha256 == unchanged.currentWtgSha256, "failure pair unchanged");
    batch = f.Batch(); auto store = Eca("bad", "StoreInteger"); GraphArgumentValue value; value.call = Eca("string", "ReadString", UiDefinitionKind::Call); store->arguments.push_back(value);
    batch.operations.emplace_back(GraphInsertSubtree{Parent(GraphParentKind::TriggerEcas, Id(GraphRecordKind::Trigger, f.doc.triggers[0].id)), {}, store});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded() && f.doc.ecaNodes.empty(), "wrong recursive return type refuses whole graph");
    auto cyclic = Eca("cycle", "Block"); cyclic->childBranch = 0; cyclic->children.push_back(cyclic);
    batch = f.Batch(); batch.operations.emplace_back(GraphInsertSubtree{Parent(GraphParentKind::TriggerEcas, Id(GraphRecordKind::Trigger, f.doc.triggers[0].id)), {}, cyclic});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "bounded recursive DTO rejects cycle"); cyclic->children.clear();
    batch = f.Batch(); batch.operations.emplace_back(GraphCreate{"x", GraphCategoryValue{"Budget"}, Parent(GraphParentKind::Categories, {}, {category}), {}});
    TriggerGraphEditLimits tiny; tiny.maxAddedRecords = 0;
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch, tiny).Succeeded(), "work budget enforced before candidate publish");
    batch = f.Batch(); batch.operations.emplace_back(GraphMove{Id(GraphRecordKind::Trigger, f.doc.triggers[0].id),
        Parent(GraphParentKind::CategoryTriggers, category), Parent(GraphParentKind::CategoryTriggers, category), {}});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "incomplete ordered parent rejected");
    {
        // 把分类行的 WTG 分类号 category_id 当成引用编号：报错写出种类、编号，并给出正确的 ref.id。
        auto numbered = f.doc;
        const auto added = TriggerDirectoryEditor::AddCategory(numbered, "Numbered"); Require(added.Succeeded(), "second category");
        const auto& row = *std::find_if(numbered.categories.begin(), numbered.categories.end(), [&](const auto& c) { return c.categoryId == *added.value; });
        const auto wrong = static_cast<StableId>(row.categoryId);
        Require(wrong != 0 && wrong != row.id && std::none_of(numbered.categories.begin(), numbered.categories.end(),
            [&](const auto& other) { return other.id == wrong; }), "fixture category number differs from its stable id");
        const auto binding = CaptureTriggerGraphBinding(numbered, f.catalog); Require(binding.Succeeded(), "numbered binding");
        batch = {*binding.value, {}}; batch.operations.emplace_back(GraphMove{Id(GraphRecordKind::Trigger, f.doc.triggers[0].id),
            Parent(GraphParentKind::CategoryTriggers, Id(GraphRecordKind::Category, wrong)),
            Parent(GraphParentKind::CategoryTriggers, Id(GraphRecordKind::Category, wrong)), {}});
        const auto misused = ApplyTriggerGraphEditBatch(numbered, f.catalog, batch);
        Require(!misused.Succeeded() && misused.diagnostics.front().code == "TRIGGER_GRAPH_RECORD_MISSING" &&
            misused.diagnostics.front().message.find("category_id") != std::string::npos &&
            misused.diagnostics.front().message.find(std::to_string(row.id)) != std::string::npos,
            ("a category number used as a ref names the right ref id: " + (misused.diagnostics.empty() ? std::string() :
            misused.diagnostics.front().code + " " + misused.diagnostics.front().message)).c_str());
    }
    {
        // 同一分类内调整顺序：from 与 to 相同，ordered_children 为移动前的完整顺序，before 指定落点。
        auto order = f.doc;
        const auto folder = TriggerDirectoryEditor::AddCategory(order, "Order"); Require(folder.Succeeded(), "order category");
        std::vector<StableId> ids;
        for (const auto* name : {"First", "Second", "Third"}) {
            const auto added = TriggerDocumentEditor::AddTrigger(order, *folder.value, name); Require(added.Succeeded(), "order trigger");
            ids.push_back(*added.value);
        }
        const auto owner = std::find_if(order.categories.begin(), order.categories.end(), [&](const auto& c) { return c.categoryId == *folder.value; })->id;
        const auto current = Parent(GraphParentKind::CategoryTriggers, Id(GraphRecordKind::Category, owner),
            {Id(GraphRecordKind::Trigger, ids[0]), Id(GraphRecordKind::Trigger, ids[1]), Id(GraphRecordKind::Trigger, ids[2])});
        const auto binding = CaptureTriggerGraphBinding(order, f.catalog); Require(binding.Succeeded(), "order binding");
        TriggerGraphEditBatch reorder{*binding.value, {}};
        reorder.operations.emplace_back(GraphMove{Id(GraphRecordKind::Trigger, ids[2]), current, current, Id(GraphRecordKind::Trigger, ids[1])});
        Require(ApplyTriggerGraphEditBatch(order, f.catalog, reorder).Succeeded(), "same-category reorder applies");
        std::vector<StableId> after;
        for (const auto& row : order.triggers) if (row.categoryId == *folder.value) after.push_back(row.id);
        Require(after == std::vector<StableId>{ids[0], ids[2], ids[1]}, "the moved trigger lands before the named sibling");
    }
    Require(f.Batch().expected.currentWtgSha256 == unchanged.currentWtgSha256 && f.Batch().expected.currentWctSha256 == unchanged.currentWctSha256, "all failed mutations preserve bytes");
    batch = f.Batch();
    batch.operations.emplace_back(GraphCreate{"removed", GraphCategoryValue{"Temporary"}, Parent(GraphParentKind::Categories, {}, {category}), {}});
    batch.operations.emplace_back(GraphRemove{Local(GraphRecordKind::Category, "removed"), "Temporary", Parent(GraphParentKind::Categories,
        {}, {category, Local(GraphRecordKind::Category, "removed")})});
    batch.operations.emplace_back(GraphCreate{"survivor", GraphCategoryValue{"Temporary"}, Parent(GraphParentKind::Categories, {}, {category}), {}});
    const auto identities = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch); Require(identities.Succeeded(), "delete then recreate batch");
    Require(identities.value->created.at("removed").id != identities.value->created.at("survivor").id, "removed local identity cannot alias recreated record");
    f.doc.wct.customCode.text = "// untouched existing author code\r\n";
    const auto withCode = WtgWctCodec{}.Serialize(f.doc, f.catalog); Require(withCode.Succeeded(), "existing opaque WCT fixture");
    const auto reparsed = WtgWctCodec{}.Parse(withCode.value->wtg, withCode.value->wct, f.catalog); Require(reparsed.Succeeded(), "seal opaque WCT fixture");
    f.doc = *reparsed.value; const auto originalCode = f.doc.wct.customCode.ToBytes();
    batch = f.Batch(); batch.operations.emplace_back(GraphSet{Id(GraphRecordKind::Trigger, f.doc.triggers[0].id), GraphField::Enabled, true, false});
    Require(ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded() && f.doc.wct.customCode.ToBytes().value == originalCode.value, "ordinary T metadata keeps existing code exact");
}

void TestWritableLoopTargets() {
    Fixture f;
    for (const auto* name : {"Counter", "Slots"})
        Require(TriggerDocumentEditor::AddVariable(f.doc, name, "integer", std::string_view(name) == "Slots").Succeeded(), "loop variable declaration");
    Require(TriggerDocumentEditor::AddVariable(f.doc, "Fraction", "real").Succeeded(), "noninteger loop test variable");
    const auto parent = [&] {
        auto result = Parent(GraphParentKind::TriggerEcas, Id(GraphRecordKind::Trigger, f.doc.triggers[0].id));
        for (const auto index : f.doc.triggers[0].ecas) result.orderedChildren.push_back(Id(GraphRecordKind::Eca, f.doc.ecaNodes[index].id));
        return result;
    };
    const auto append = [&](GraphArgumentValue target, bool rollback) {
        auto batch = f.Batch();
        if (rollback) batch.operations.emplace_back(GraphCreate{"rollback", GraphCategoryValue{"MustNotPublish"},
            Parent(GraphParentKind::Categories, {}, {Id(GraphRecordKind::Category, f.doc.categories[0].id)}), {}});
        auto loop = Eca("loop", "ForLoopVarMultiple"); loop->arguments = {std::move(target), Scalar(3, "1"), Scalar(3, "16")};
        batch.operations.emplace_back(GraphInsertSubtree{parent(), {}, loop});
        return ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch);
    };
    Require(append(Scalar(1, "Counter"), false).Succeeded() && append(Scalar(1, "udg_Counter"), false).Succeeded(),
        "scalar declaration name and udg reference are writable loop targets");
    auto array = Scalar(1, "Slots"); auto index = std::make_shared<GraphArgumentValue>();
    index->call = Eca("readIndex", "ReadInteger", UiDefinitionKind::Call); array.index = index;
    Require(append(array, false).Succeeded(), "array loop retains a typed dynamic index");
    const auto unchanged = f.Batch().expected;
    const auto rejects = [&](GraphArgumentValue target, const char* code) {
        const auto result = append(std::move(target), true);
        Require(!result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; }), "invalid loop target has a precise diagnostic");
        const auto after = f.Batch().expected;
        Require(after.currentWtgSha256 == unchanged.currentWtgSha256 && after.currentWctSha256 == unchanged.currentWctSha256
            && f.doc.categories.size() == 1, "loop target rejection rolls back the entire transaction");
    };
    rejects(Scalar(3, "1"), "TRIGGER_WRITABLE_VARIABLE_REQUIRED");
    rejects(Scalar(0, "LoopPreset"), "TRIGGER_WRITABLE_VARIABLE_REQUIRED");
    GraphArgumentValue call; call.call = Eca("nonLvalue", "ReadInteger", UiDefinitionKind::Call);
    rejects(call, "TRIGGER_WRITABLE_VARIABLE_REQUIRED");
    rejects(Scalar(1, "Missing"), "TRIGGER_WRITABLE_VARIABLE_UNKNOWN");
    rejects(Scalar(1, "Fraction"), "TRIGGER_WRITABLE_VARIABLE_TYPE");
    rejects(Scalar(1, "Slots"), "TRIGGER_WRITABLE_VARIABLE_SHAPE");
    auto badIndex = std::make_shared<GraphArgumentValue>(); badIndex->call = Eca("badIndex", "ReadString", UiDefinitionKind::Call);
    array.index = badIndex; rejects(array, "TRIGGER_CALL_TYPE_MISMATCH");

    const TriggerTypeSystem types(f.catalog);
    const auto& loop = f.doc.ecaNodes[f.doc.triggers[0].ecas.back()];
    auto target = f.doc.argumentNodes[loop.arguments[0]]; target.value.text = "udg_Slots";
    std::string resolved = "unchanged";
    Require(types.ValidateWritableVariable(f.doc, target, "integer", &resolved).Succeeded() && resolved == "Slots", "indexed udg target resolves centrally");
    auto ambiguous = f.doc; auto duplicate = ambiguous.variables[1]; duplicate.name.text = "udg_Slots"; ambiguous.variables.push_back(duplicate);
    resolved = "unchanged";
    const auto rejected = types.ValidateWritableVariable(ambiguous, target, "integer", &resolved);
    Require(!rejected.Succeeded() && rejected.diagnostics.front().code == "TRIGGER_WRITABLE_VARIABLE_AMBIGUOUS"
        && resolved == "unchanged", "ambiguous target does not publish a resolved declaration name");
    target.insertIndex = 0;
    Require(!types.ValidateWritableVariable(f.doc, target, "integer").Succeeded(), "hidden array index is not writable");

    // 未触碰的旧循环即使不合本轮新资格，也不能阻塞无关 T 的编辑；生成时仍会拒绝它。
    f.doc.argumentNodes[f.doc.ecaNodes[f.doc.triggers[0].ecas.front()].arguments[0]].type = 3;
    f.doc.argumentNodes[f.doc.ecaNodes[f.doc.triggers[0].ecas.front()].arguments[0]].value.text = "0";
    auto batch = f.Batch(); auto unrelated = Eca("unrelated", "StoreInteger"); unrelated->arguments = {Scalar(3, "2")};
    batch.operations.emplace_back(GraphInsertSubtree{parent(), {}, unrelated});
    Require(ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "unrelated edits do not revalidate old loop siblings");
}
void TestObjectCodeGraphAndAtomicRename() {
    Fixture f;
    auto batch = f.Batch();
    batch.operations.emplace_back(GraphCreate{"ability", GraphVariableValue{"Ability", "abilityalias2", false, 1, true, "1234"}, Parent(GraphParentKind::Variables), {}});
    batch.operations.emplace_back(GraphCreate{"number", GraphVariableValue{"Count", "integer", false, 1, true, "1234"},
        Parent(GraphParentKind::Variables, {}, {Local(GraphRecordKind::Variable, "ability")}), {}});
    auto comparison = Eca("compare", "OperatorCompareInteger", UiDefinitionKind::Condition);
    GraphArgumentValue level, unit;
    auto levelCall = Eca("level", "GetUnitAbilityLevel", UiDefinitionKind::Call);
    unit.call = Eca("unit", "GetTriggerUnit", UiDefinitionKind::Call);
    levelCall->arguments = {unit, Scalar(3, "A0E0")}; level.call = levelCall;
    comparison->arguments = {level, Scalar(0, "OperatorGreater"), Scalar(3, "0")};
    const auto trigger = Id(GraphRecordKind::Trigger, f.doc.triggers.front().id);
    batch.operations.emplace_back(GraphInsertSubtree{Parent(GraphParentKind::TriggerEcas, trigger), {}, comparison});
    const auto edited = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch);
    Require(edited.Succeeded(), "actual ability-level condition accepts a four-byte abilcode through public edit");
    Require(f.doc.variables[0].value.text == "1234" && f.doc.variables[1].value.text == "1234",
        "object and decimal defaults retain source bytes and distinct declared roles");
    const auto parents = [&]() {
        std::vector<GraphRecordRef> children;
        for (const auto index : f.doc.triggers.front().ecas) children.push_back(Id(GraphRecordKind::Eca, f.doc.ecaNodes[index].id));
        return Parent(GraphParentKind::TriggerEcas, trigger, std::move(children));
    };
    for (const auto& value : {Scalar(3, "1234"), Scalar(1, "Count"), Scalar(0, "AbilityPreset")}) {
        batch = f.Batch(); auto use = Eca("alias", "UseAbilityAlias"); use->arguments.push_back(value);
        batch.operations.emplace_back(GraphInsertSubtree{parents(), {}, use});
        Require(ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "alias fixed/variable/preset source remains typed");
    }
    batch = f.Batch();
    batch.operations.emplace_back(GraphRename{trigger, f.doc.triggers.front().name, "RenamedRawcodeTrigger", {}});
    Require(ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "valid rawcodes are not opaque code during atomic rename");
    const auto unchanged = f.Batch().expected;
    for (const auto& value : {"A0E", "A0E00", "'A0E0'", "A\\00", "A'00", "null", "0"}) {
        batch = f.Batch(); auto invalid = Eca("invalid", "UseAbilityAlias"); invalid->arguments.push_back(Scalar(3, value));
        batch.operations.emplace_back(GraphInsertSubtree{parents(), {}, invalid});
        Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "invalid rawcode refuses the whole transaction");
    }
    for (const auto* function : {"StoreInteger", "UseCustomCode"}) {
        batch = f.Batch(); auto invalid = Eca("invalid", function); invalid->arguments.push_back(Scalar(3, "A0E0"));
        batch.operations.emplace_back(GraphInsertSubtree{parents(), {}, invalid});
        Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "ordinary integer/code suffix never accepts object text");
    }
    batch = f.Batch();
    batch.operations.emplace_back(GraphCreate{"bad-default", GraphVariableValue{"Invalid", "abilityalias2", false, 1, true, "0"},
        Parent(GraphParentKind::Variables, {}, {Id(GraphRecordKind::Variable, f.doc.variables[0].id), Id(GraphRecordKind::Variable, f.doc.variables[1].id)}), {}});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "object variable default uses the same strict role");
    Require(f.Batch().expected.currentWtgSha256 == unchanged.currentWtgSha256
        && f.Batch().expected.currentWctSha256 == unchanged.currentWctSha256, "all invalid rawcode operations leave both members exact");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog); Require(encoded.Succeeded(), "object graph serializes");
    const auto decoded = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(decoded.Succeeded() && decoded.value->variables[0].typeName.text == "abilityalias2"
        && decoded.value->variables[0].value.text == "1234", "rawcode role and source survive the WTG/WCT pair");
}
const EcaNode& Function(const TriggerDocument& document, std::string_view name) {
    const auto found = std::find_if(document.ecaNodes.begin(), document.ecaNodes.end(),
        [name](const EcaNode& node) { return node.name.text == name; });
    Require(found != document.ecaNodes.end(), "fixture function exists"); return *found;
}
StableId ArgumentId(const TriggerDocument& document, const EcaNode& owner, std::size_t ordinal) {
    return document.argumentNodes.at(owner.arguments.at(ordinal)).id;
}
void Seal(Fixture& fixture) {
    const auto encoded = WtgWctCodec{}.Serialize(fixture.doc, fixture.catalog); Require(encoded.Succeeded(), "encode legacy fixture");
    const auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, fixture.catalog);
    Require(parsed.Succeeded(), "reparse legacy fixture"); fixture.doc = *parsed.value;
}
struct LegacySlots { StableId owner, outer, nested, code, identity, getter; };
LegacySlots LegacyCodeSibling(Fixture& f) {
    Require(TriggerDocumentEditor::AddVariable(f.doc, "ContextUnit", "unit").Succeeded(), "unit context declaration");
    Require(TriggerDocumentEditor::AddVariable(f.doc, "Count", "integer").Succeeded(), "integer declaration");
    const auto appended = TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, f.doc.triggers.front().id,
        UiDefinitionKind::Action, "WithCodeSibling"); Require(appended.Succeeded(), "legacy source with code sibling");
    const auto outer = ArgumentId(f.doc, Function(f.doc, "WithCodeSibling"), 0);
    const auto code = ArgumentId(f.doc, Function(f.doc, "WithCodeSibling"), 1);
    const auto identity = TriggerDocumentEditor::AppendArgumentCall(f.doc, f.catalog, outer, "unit", "IdentityUnit");
    Require(identity.Succeeded(), "legacy unit wrapper call");
    const auto nested = ArgumentId(f.doc, Function(f.doc, "IdentityUnit"), 0);
    Require(TriggerDocumentEditor::AppendArgumentCall(f.doc, f.catalog, nested, "unit", "GetTriggerUnit").Succeeded(), "legacy context getter");
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, code, 3, "call ExistingLogic() // 保留\r\n").Succeeded(), "existing authored script slot");
    TriggerDocumentEditor::FindArgument(f.doc, code)->value.encoding = TextEncoding::Gb18030;
    f.doc.wct.customCode.text = "// original global code\r\n";
    Seal(f);
    const auto& owner = Function(f.doc, "WithCodeSibling");
    const auto& wrapper = Function(f.doc, "IdentityUnit");
    return {owner.id, ArgumentId(f.doc, owner, 0), ArgumentId(f.doc, wrapper, 0), ArgumentId(f.doc, owner, 1),
        wrapper.id, Function(f.doc, "GetTriggerUnit").id};
}
GraphArgumentReplacementGuard Guard(const Fixture& f, StableId id) {
    const auto result = CaptureArgumentReplacementGuard(f.doc, f.catalog, id);
    Require(result.Succeeded(), "capture typed replacement guard"); return *result.value;
}
void TestArgumentReplacementKeepsSiblingCode() {
    Fixture f; const auto ids = LegacyCodeSibling(f);
    const auto codeBefore = *TriggerDocumentEditor::FindArgument(f.doc, ids.code);
    const auto globalBefore = f.doc.wct.customCode;
    const auto nestedGuard = Guard(f, ids.nested);
    Require(nestedGuard.ownerEcaId == ids.identity && nestedGuard.argumentOrdinal == 0
        && nestedGuard.sourceTriggerId == f.doc.triggers.front().id && nestedGuard.expectedType == "unit", "query identifies real nested call slot");
    auto batch = f.Batch();
    batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, ids.nested), nestedGuard, Scalar(1, "udg_ContextUnit")});
    TriggerGraphEditLimits noAddedRecords; noAddedRecords.maxAddedRecords = 0;
    const auto replaced = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch, noAddedRecords);
    Require(replaced.Succeeded() && replaced.value->argumentReplacements.size() == 1, "replace nested getter without rebuilding parent code slot");
    const auto& receipt = replaced.value->argumentReplacements.front();
    Require(receipt.argumentId == ids.nested && receipt.removedEcas == 1 && receipt.removedArguments == 0
        && receipt.beforeSubtreeSha256 == nestedGuard.subtreeSha256 && receipt.afterSubtreeSha256 != receipt.beforeSubtreeSha256,
        "exact local detach receipt and advancing identity");
    Require(Function(f.doc, "WithCodeSibling").id == ids.owner && Function(f.doc, "IdentityUnit").id == ids.identity,
        "original owner and call parent identities retained");
    const auto* nested = TriggerDocumentEditor::FindArgument(f.doc, ids.nested);
    Require(nested && nested->type == 1 && nested->value.text == "udg_ContextUnit" && !nested->callEca, "root argument reused as variable");
    Require(std::none_of(f.doc.ecaNodes.begin(), f.doc.ecaNodes.end(), [&](const EcaNode& node) { return node.id == ids.getter; }), "old getter deleted");
    const auto* codeAfter = TriggerDocumentEditor::FindArgument(f.doc, ids.code);
    Require(codeAfter && codeAfter->value.text == codeBefore.value.text && codeAfter->value.encoding == codeBefore.value.encoding
        && codeAfter->value.originalBytes == codeBefore.value.originalBytes && codeAfter->value.ToBytes().value == codeBefore.value.ToBytes().value
        && codeAfter->type == codeBefore.type && codeAfter->insertCall == codeBefore.insertCall && codeAfter->insertIndex == codeBefore.insertIndex,
        "GBK sibling code bytes encoding flags and stable id untouched");
    Require(f.doc.wct.customCode.originalBytes == globalBefore.originalBytes && f.doc.wct.customCode.ToBytes().value == globalBefore.ToBytes().value,
        "global WCT unchanged");
    const auto outerGuard = Guard(f, ids.outer); batch = f.Batch();
    batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, ids.outer), outerGuard, Scalar(1, "ContextUnit")});
    const auto outerResult = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch);
    Require(outerResult.Succeeded() && outerResult.value->argumentReplacements.front().removedEcas == 1
        && outerResult.value->argumentReplacements.front().removedArguments == 1, "replace full old call closure with no orphan arguments");
    Require(!TriggerDocumentEditor::FindArgument(f.doc, ids.nested), "old descendant argument removed");
    batch = f.Batch(); GraphArgumentValue newCall; newCall.call = Eca("replacementGetter", "GetTriggerUnit", UiDefinitionKind::Call);
    batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, ids.outer), Guard(f, ids.outer), newCall});
    const auto callResult = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch);
    Require(callResult.Succeeded() && callResult.value->created.contains("replacementGetter"), "same recursive DTO creates replacement call");
    Require(WtgWctCodec{}.Validate(f.doc, f.catalog).Succeeded(), "replacement graph has complete unique ownership");
    Seal(f);
    const auto newId = ArgumentId(f.doc, Function(f.doc, "WithCodeSibling"), 0);
    Require(Guard(f, newId).expectedType == "unit", "saved pair queried again with current identities");
    Require(TriggerDocumentEditor::FindArgument(f.doc, ArgumentId(f.doc, Function(f.doc, "WithCodeSibling"), 1))->value.ToBytes().value
        == codeBefore.value.ToBytes().value, "sibling code survives serialized readback");
}
void TestArgumentReplacementRejectsStaleAndRollsBack() {
    Fixture f; const auto ids = LegacyCodeSibling(f);
    const auto outerGuard = Guard(f, ids.outer), nestedGuard = Guard(f, ids.nested);
    const auto unchanged = f.Batch().expected;
    const auto rejects = [&](GraphArgumentReplacementGuard expected, GraphArgumentValue value) {
        auto batch = f.Batch(); batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, ids.outer), std::move(expected), std::move(value)});
        Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "invalid replacement refuses");
        Require(f.Batch().expected.currentWtgSha256 == unchanged.currentWtgSha256 && f.Batch().expected.currentWctSha256 == unchanged.currentWctSha256,
            "failed replacement publishes neither member");
    };
    auto stale = outerGuard; stale.subtreeSha256 = std::string(64, '0'); rejects(stale, Scalar(1, "ContextUnit"));
    stale = outerGuard; ++stale.ownerEcaId; rejects(stale, Scalar(1, "ContextUnit"));
    stale = outerGuard; ++stale.argumentOrdinal; rejects(stale, Scalar(1, "ContextUnit"));
    stale = outerGuard; stale.uiSignature += "stale"; rejects(stale, Scalar(1, "ContextUnit"));
    rejects(outerGuard, Scalar(1, "Count")); rejects(outerGuard, Scalar(1, "MissingUnit"));
    auto batch = f.Batch();
    batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, ids.nested), nestedGuard, Scalar(1, "ContextUnit")});
    batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, ids.outer), outerGuard, Scalar(1, "ContextUnit")});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "earlier nested replacement invalidates outer expected closure in same batch");
    Require(f.Batch().expected.currentWtgSha256 == unchanged.currentWtgSha256 && Guard(f, ids.nested) == nestedGuard,
        "second operation failure rolls back successful first replacement");
    auto* root = TriggerDocumentEditor::FindArgument(f.doc, ids.outer);
    const auto originalBytes = root->value.originalBytes; root->value.originalBytes.push_back(std::byte{0x20});
    Require(Guard(f, ids.outer).subtreeSha256 != outerGuard.subtreeSha256, "fingerprint includes raw original bytes even when effective text is unchanged");
    root->value.originalBytes = originalBytes;
    TriggerGraphEditLimits tiny; tiny.maxDepth = 1;
    Require(!CaptureArgumentReplacementGuard(f.doc, f.catalog, ids.outer, tiny).Succeeded(), "bounded old closure query");
    Require(!CaptureArgumentReplacementGuard(f.doc, f.catalog, ids.code).Succeeded(), "scriptcode direct target stays protected");
    const auto ownerIndex = Function(f.doc, "WithCodeSibling").arguments[0];
    auto& wrapper = const_cast<EcaNode&>(Function(f.doc, "IdentityUnit"));
    const auto oldArgument = wrapper.arguments[0]; wrapper.arguments[0] = ownerIndex;
    Require(!CaptureArgumentReplacementGuard(f.doc, f.catalog, ids.outer).Succeeded(), "cyclic/shared old closure refuses before traversal or detach");
    wrapper.arguments[0] = oldArgument;
    Require(Guard(f, ids.outer) == outerGuard, "rejected queries leave source identity intact");
}
void TestArgumentReplacementOpaqueAndDynamicTypes() {
    Fixture f;
    Require(TriggerDocumentEditor::AddVariable(f.doc, "ContextUnit", "unit").Succeeded(), "dynamic unit variable");
    Require(TriggerDocumentEditor::AddVariable(f.doc, "Units", "unit", true).Succeeded(), "array source variable");
    const auto trigger = f.doc.triggers.front().id;
    Require(TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, trigger, UiDefinitionKind::Action, "DynamicStore").Succeeded(), "dynamic owner");
    const auto dynamicFirst = ArgumentId(f.doc, Function(f.doc, "DynamicStore"), 0);
    const auto dynamicValue = ArgumentId(f.doc, Function(f.doc, "DynamicStore"), 1);
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, dynamicFirst, 1, "ContextUnit").Succeeded(), "dynamic source type");
    Require(TriggerDocumentEditor::AppendArgumentCall(f.doc, f.catalog, dynamicValue, "unit", "GetTriggerUnit").Succeeded(), "dynamic typed getter");
    Require(Guard(f, dynamicValue).expectedType == "unit", "guard resolves actual Null type from preceding variable");
    auto batch = f.Batch(); batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, dynamicValue), Guard(f, dynamicValue), Scalar(1, "ContextUnit")});
    Require(ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "replacement uses resolved type rather than literal Null");
    Require(TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, trigger, UiDefinitionKind::Action, "StoreInteger").Succeeded(), "opaque closure parent");
    const auto integerArg = ArgumentId(f.doc, Function(f.doc, "StoreInteger"), 0);
    Require(TriggerDocumentEditor::AppendArgumentCall(f.doc, f.catalog, integerArg, "integer", "ReadCode").Succeeded(), "old opaque child call");
    const auto opaque = ArgumentId(f.doc, Function(f.doc, "ReadCode"), 0);
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, opaque, 3, "LegacyCode()").Succeeded(), "old code content");
    Require(!CaptureArgumentReplacementGuard(f.doc, f.catalog, integerArg).Succeeded(), "typed outer return never permits deleting nested scriptcode");
    const auto withOpaque = f.Batch().expected;
    batch = f.Batch(); batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, integerArg), {}, Scalar(3, "1")});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded()
        && f.Batch().expected.currentWtgSha256 == withOpaque.currentWtgSha256, "opaque closure rejects whole edit before any detach");
    Require(TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, trigger, UiDefinitionKind::Action, "StoreString").Succeeded(), "mapped code parent");
    const auto stringArg = ArgumentId(f.doc, Function(f.doc, "StoreString"), 0);
    Require(TriggerDocumentEditor::AppendArgumentCall(f.doc, f.catalog, stringArg, "string", "DispatchAlias").Succeeded(), "mapped ExecuteFunc source");
    const auto dispatchArg = ArgumentId(f.doc, Function(f.doc, "DispatchAlias"), 0);
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, dispatchArg, 3, "ExistingTrigger").Succeeded(), "mapped dispatch value");
    Require(!CaptureArgumentReplacementGuard(f.doc, f.catalog, stringArg).Succeeded()
        && !CaptureArgumentReplacementGuard(f.doc, f.catalog, dispatchArg).Succeeded(), "string-typed dynamic code and code ancestors remain protected");
    // 新数组 DTO 可在普通来源替换中创建；下一次旧根 index 替换仍必须明确拒绝。
    auto array = Scalar(1, "Units"); array.index = std::make_shared<const GraphArgumentValue>(Scalar(3, "0"));
    batch = f.Batch(); batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, dynamicValue), Guard(f, dynamicValue), array});
    Require(ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded(), "new indexed value uses existing DTO and after fingerprint");
    Require(!CaptureArgumentReplacementGuard(f.doc, f.catalog, dynamicValue).Succeeded(), "old index root replacement explicitly unsupported");
    Require(TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, trigger, UiDefinitionKind::Action, "SelectedStore").Succeeded(), "type selector owner");
    const auto selector = ArgumentId(f.doc, Function(f.doc, "SelectedStore"), 0);
    const auto selectedValue = ArgumentId(f.doc, Function(f.doc, "SelectedStore"), 1);
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, selector, 0, "typename_unit").Succeeded(), "unit selector");
    Require(TriggerDocumentEditor::SetScalarArgument(f.doc, selectedValue, 1, "ContextUnit").Succeeded(), "typed selector value");
    const auto before = f.Batch().expected;
    batch = f.Batch(); batch.operations.emplace_back(GraphReplaceArgument{Id(GraphRecordKind::Argument, selector), Guard(f, selector), Scalar(0, "typename_integer")});
    Require(!ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch).Succeeded() && f.Batch().expected.currentWtgSha256 == before.currentWtgSha256,
        "replacement cannot silently reinterpret untouched dependent sibling type");
}
} // namespace
// AddTriggerEvent 这类函数槽：eventcall 填事件、boolexpr 填条件，与 WE 存法一致并能往返。
void TestFunctionSlotArguments() {
    Fixture f; auto batch = f.Batch(); const auto trigger = Id(GraphRecordKind::Trigger, f.doc.triggers[0].id);
    batch.operations.emplace_back(GraphCreate{"trig", GraphVariableValue{"Trig", "trigger", false, 1, false, ""}, Parent(GraphParentKind::Variables), {}});
    auto add = Eca("add", "AddTriggerEvent"); add->arguments.push_back(Scalar(1, "Trig"));
    GraphArgumentValue event, unit; unit.call = Eca("unit", "GetTriggerUnit", UiDefinitionKind::Call);
    auto registerCall = Eca("register", "TriggerRegisterUnitEvent", UiDefinitionKind::Event); registerCall->arguments.push_back(unit);
    event.call = registerCall; add->arguments.push_back(event);
    batch.operations.emplace_back(GraphInsertSubtree{Parent(GraphParentKind::TriggerEcas, trigger), {}, add});
    auto filter = Eca("filter", "UseFilter"); GraphArgumentValue condition;
    auto compare = Eca("compare", "OperatorCompareInteger", UiDefinitionKind::Condition);
    compare->arguments = {Scalar(3, "1"), Scalar(0, "OperatorGreater"), Scalar(3, "0")}; condition.call = compare; filter->arguments.push_back(condition);
    batch.operations.emplace_back(GraphInsertSubtree{Parent(GraphParentKind::TriggerEcas, trigger, {Local(GraphRecordKind::Eca, "add")}), {}, filter});
    const auto applied = ApplyTriggerGraphEditBatch(f.doc, f.catalog, batch);
    Require(applied.Succeeded(), "event/condition slot calls are creatable");
    Require(TriggerDocumentEditor::FindEca(f.doc, applied.value->created.at("register").id)->type == 0
        && TriggerDocumentEditor::FindEca(f.doc, applied.value->created.at("compare").id)->type == 1, "slot calls keep WE event/condition ECA types");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog); Require(encoded.Succeeded(), "serialize slot calls");
    const auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog); Require(parsed.Succeeded(), "slot calls read back");
    Require(parsed.value->ecaNodes.size() == f.doc.ecaNodes.size() && parsed.value->argumentNodes.size() == f.doc.argumentNodes.size(), "slot call graph survives codec");
    const std::vector<GraphRecordRef> roots{Id(GraphRecordKind::Eca, applied.value->created.at("add").id), Id(GraphRecordKind::Eca, applied.value->created.at("filter").id)};
    const auto rejects = [&](std::shared_ptr<GraphEcaValue> root, const char* code, const char* message) {
        auto bad = f.Batch(); bad.operations.emplace_back(GraphInsertSubtree{Parent(GraphParentKind::TriggerEcas, trigger, roots), {}, root});
        const auto result = ApplyTriggerGraphEditBatch(f.doc, f.catalog, bad);
        Require(!result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [code](const auto& diagnostic) { return diagnostic.code == code; }), message);
    };
    auto wrongSlot = Eca("wrong", "AddTriggerEvent"); wrongSlot->arguments.push_back(Scalar(1, "Trig"));
    GraphArgumentValue conditionInEvent; auto misplaced = Eca("c", "OperatorCompareInteger", UiDefinitionKind::Condition);
    misplaced->arguments = compare->arguments; conditionInEvent.call = misplaced; wrongSlot->arguments.push_back(conditionInEvent);
    rejects(wrongSlot, "TRIGGER_GRAPH_CALL_INVALID", "condition cannot fill an eventcall slot");
    auto eventAsCall = Eca("asCall", "AddTriggerEvent"); eventAsCall->arguments.push_back(Scalar(1, "Trig"));
    GraphArgumentValue named; auto namedCall = Eca("e", "TriggerRegisterUnitEvent", UiDefinitionKind::Call); namedCall->arguments.push_back(unit);
    named.call = namedCall; eventAsCall->arguments.push_back(named);
    rejects(eventAsCall, "TRIGGER_CALL_SLOT_MISMATCH", "event named as call gets a kind hint");
    auto plain = Eca("plain", "StoreInteger"); GraphArgumentValue eventInInteger;
    auto eventCall = Eca("e2", "TriggerRegisterUnitEvent", UiDefinitionKind::Event); eventCall->arguments.push_back(unit);
    eventInInteger.call = eventCall; plain->arguments.push_back(eventInInteger);
    rejects(plain, "TRIGGER_GRAPH_CALL_INVALID", "event cannot fill a plain value slot");
    // 窄接口不带种类时按槽位自动找同名事件。
    auto root = TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, f.doc.triggers[0].id, UiDefinitionKind::Action, "AddTriggerEvent");
    Require(root.Succeeded(), "append add event");
    const auto slotId = f.doc.argumentNodes[TriggerDocumentEditor::FindEca(f.doc, root.value->rootEcaId)->arguments[1]].id;
    const auto picked = TriggerDocumentEditor::AppendArgumentCall(f.doc, f.catalog, slotId, "eventcall", "TriggerRegisterUnitEvent");
    Require(picked.Succeeded() && TriggerDocumentEditor::FindEca(f.doc, picked.value->rootEcaId)->type == 0, "narrow append picks the event slot");
}
int main() {
    try { TestFunctionSlotArguments(); TestRecursiveBatchAndAtomicNames(); TestFailurePublishesNothing(); TestWritableLoopTargets(); TestObjectCodeGraphAndAtomicRename();
        TestArgumentReplacementKeepsSiblingCode(); TestArgumentReplacementRejectsStaleAndRollsBack(); TestArgumentReplacementOpaqueAndDynamicTypes();
        std::cout << "public typed graph batch guards and recursive codec roundtrip passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
