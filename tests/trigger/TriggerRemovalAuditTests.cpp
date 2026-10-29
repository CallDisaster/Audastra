#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Trigger;
namespace {
void Require(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
template<class T> void Good(const T& result, const char* text) {
    if (!result.Succeeded()) { for (const auto& d : result.diagnostics) std::cerr << d.code << ": " << d.message << '\n'; }
    Require(result.Succeeded(), text);
}
template<class T> bool Code(const T& r, std::string_view code) { return std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [&](const auto& d) { return d.code == code; }); }
struct Fixture {
    inline static std::atomic<unsigned> sequence{0};
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-RemoveAudit-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()) + '-' + std::to_string(sequence.fetch_add(1)));
    UiCatalog catalog; TriggerDocument doc; StableId target = 0, category = 0;
    Fixture() {
        Require(std::filesystem::create_directory(root), "fresh original fixture");
        std::filesystem::create_directories(root / "ui/base");
        std::ofstream(root / "ui/config") << "base\n";
        std::ofstream(root / "ui/base/define.txt") << "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nstring=0,1,1,String,string\ntrigger=0,1,1,Trigger,trigger\n";
        std::ofstream(root / "ui/base/action.txt") << "[ExecuteFunc]\n[[.args]]\ntype=string\n[Run]\n[[.args]]\ntype=trigger\n";
        auto loaded = UiCatalog::Load(root / "ui"); Good(loaded, "catalog"); catalog = *loaded.value;
        auto empty = WtgWctCodec{}.CreateEmpty(catalog); Good(empty, "sealed source"); doc = *empty.value;
        auto c = TriggerDirectoryEditor::AddCategory(doc, "Original"); Good(c, "category"); category = *c.value;
        auto t = TriggerDocumentEditor::AddTrigger(doc, category, "Target Name"); Good(t, "target"); target = *t.value;
        for (const auto* name : {"OtherA", "OtherB"}) { Good(TriggerDocumentEditor::AddTrigger(doc, category, name), "code owner"); doc.triggers.back().customTextFlag = 1; doc.triggers.back().customText = RawText{"// original unrelated block\n"}; }
        doc.wct.customCode = RawText{"globals\ninteger unrelated = 0\nendglobals\n"};
    }
    ~Fixture() { std::error_code e; std::filesystem::remove_all(root, e); }
    GraphRemove Remove() const {
        GraphParentExpectation parent{GraphParentKind::CategoryTriggers, {GraphRecordKind::Category, doc.categories.front().id, {}}, {}, {}};
        for (const auto& t : doc.triggers) parent.orderedChildren.push_back({GraphRecordKind::Trigger, t.id, {}});
        return {{GraphRecordKind::Trigger, target, {}}, doc.triggers.front().name.text, parent};
    }
    TriggerGraphEditBatch Batch(const GraphRemove& op) const { auto b = CaptureTriggerGraphBinding(doc, catalog); Good(b, "binding"); return {*b.value, {op}}; }
    DeclarationRemovalReview Review(const GraphRemove& op) const { auto r = QueryDeclarationRemovalAudit(doc, catalog, Batch(op), 0); Good(r, "review"); return *r.value; }
    void Confirm(GraphRemove& op, const DeclarationRemovalReview& r) const {
        if (!r.groupableBlocks.empty()) {
            RemovalCodeConfirmation c{r.reviewSha256, "no_reference", "reviewed all listed zero-hit blocks", {}};
            for (const auto& b : r.groupableBlocks) c.blocks.push_back(b.identity);
            op.codeAudits.push_back(c);
        }
        for (const auto& b : r.individualBlocks) op.codeAudits.push_back({r.reviewSha256, "no_reference", "reviewed this individual evidence", {b.identity}});
    }
    void Fails(const GraphRemove& op, std::string_view code) {
        auto before = WtgWctCodec{}.Serialize(doc, catalog); Good(before, "serialize before");
        auto result = ApplyTriggerGraphEditBatch(doc, catalog, Batch(op)); Require(Code(result, code), "expected fail-closed diagnostic");
        auto after = WtgWctCodec{}.Serialize(doc, catalog); Good(after, "serialize after");
        Require(before.value->wtg == after.value->wtg && before.value->wct == after.value->wct, "failure preserves exact paired bytes");
    }
};
void GroupedAndStale() {
    Fixture f; auto op = f.Remove(); auto r = f.Review(op);
    Require(r.complete && r.groupableBlocks.size() == 3 && r.individualBlocks.empty(), "zero-hit blocks are grouped by service");
    f.Fails(op, "TRIGGER_REMOVE_CODE_AUDIT_REQUIRED");
    f.Confirm(op, r);
    auto old = op; f.doc.triggers.back().customText.text += "// changed\n";
    f.Fails(old, "TRIGGER_REMOVE_CODE_AUDIT_STALE");
    op.codeAudits.clear(); f.Confirm(op, f.Review(op));
    auto wct = f.doc.wct.customCode.ToBytes(); auto result = ApplyTriggerGraphEditBatch(f.doc, f.catalog, f.Batch(op)); Good(result, "grouped deletion publishes");
    Require(f.doc.triggers.size() == 2 && result.value->removalAudits.size() == 1 && f.doc.wct.customCode.ToBytes().value == wct.value, "one confirmation, exact code preserved");
}
void IndividualAndReferences() {
    Fixture f; f.doc.wct.customCode = RawText{"// Target Name only in a comment\ncall ExecuteFunc(\"Unrelated\")\n"};
    auto op = f.Remove(); auto r = f.Review(op);
    Require(r.complete && r.individualBlocks.size() == 1 && r.groupableBlocks.size() == 2, "constant dispatch and comment require individual review");
    f.Confirm(op, r); op.codeAudits.front().blocks.push_back(r.individualBlocks.front().identity); op.codeAudits.pop_back();
    f.Fails(op, "TRIGGER_REMOVE_CODE_AUDIT_INVALID");
    f.doc.wct.customCode = RawText{"globals\nstring label = \"prefix Target Name suffix\"\nendglobals\n"};
    op = f.Remove(); r = f.Review(op);
    Require(r.complete && r.individualBlocks.size() == 1 && !r.individualBlocks.front().dynamic && !r.individualBlocks.front().referenced,
        "target substring in an ordinary string requires separate review without claiming a definite call");
    for (const auto* code : {"call Run(gg_trg_Target_Name)\n", "call ExecuteFunc(\"InitTrig_Target_Name\")\n", "call LookupByName(table, \"Target Name\")\n"}) {
        f.doc.wct.customCode = RawText{code}; op = f.Remove(); r = f.Review(op); f.Confirm(op, r); f.Fails(op, "TRIGGER_GRAPH_REMOVE_REFERENCED");
    }
    for (const auto* code : {"call ExecuteFunc(prefix + suffix)\n", "call ExecuteFunc(\"InitTrig_\" + \"Target_Name\")\n", "set x = _G[key]\n"}) {
        f.doc.wct.customCode = RawText{code}; op = f.Remove(); r = f.Review(op);
        Require(r.complete && !r.individualBlocks.empty(), "dynamic evidence requires individual review rather than an incomplete verdict");
        f.Confirm(op, r); Good(CheckDeclarationRemovalAudit(f.doc, f.catalog, op), "explicit individual dynamic review is accepted");
    }
    for (const auto* code : {"//! import \"external.j\"\n", "/* unterminated", "call loadstring(code)\n"}) {
        f.doc.wct.customCode = RawText{code}; op = f.Remove(); r = f.Review(op); f.Confirm(op, r); f.Fails(op, "TRIGGER_GRAPH_REMOVE_REFERENCE_UNKNOWN");
    }
}
void TypedDynamicAndVariable() {
    Fixture f;
    // typed ExecuteFunc字符串槽不能被当成普通零命中标识符。
    f.doc.triggers.back().customTextFlag = 0; f.doc.triggers.back().customText = {};
    auto added = TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, f.doc.triggers.back().id, UiDefinitionKind::Action, "ExecuteFunc"); Good(added, "typed dispatcher");
    const auto* eca = TriggerDocumentEditor::FindEca(f.doc, added.value->rootEcaId);
    auto& arg = f.doc.argumentNodes[eca->arguments.front()]; arg.type = 1; arg.value = RawText{"udg_TargetFunction"};
    Good(TriggerDocumentEditor::AddVariable(f.doc, "TargetFunction", "string"), "dynamic function variable");
    auto op = f.Remove(); auto r = f.Review(op); Require(r.complete && !r.individualBlocks.empty(), "typed dynamic destination is individually reviewable");
    f.Confirm(op, r); Good(CheckDeclarationRemovalAudit(f.doc, f.catalog, op), "typed dynamic review accepted without claiming runtime proof");
    arg.type = 3; arg.value = RawText{"InitTrig_Target_Name"}; op = f.Remove(); r = f.Review(op); f.Confirm(op, r); f.Fails(op, "TRIGGER_GRAPH_REMOVE_REFERENCED");
    arg.value = RawText{"Unrelated"};
    auto variable = f.doc.variables.front(); GraphParentExpectation parent{GraphParentKind::Variables, {}, {{GraphRecordKind::Variable, variable.id, {}}}, {}};
    GraphRemove remove{{GraphRecordKind::Variable, variable.id, {}}, variable.name.text, parent};
    f.Confirm(remove, f.Review(remove)); Good(ApplyTriggerGraphEditBatch(f.doc, f.catalog, f.Batch(remove)), "variable deletion uses same audit contract");
}
void EncodingAndPrefix() {
    Fixture f; f.doc.triggers.front().name = RawText{"目标 名", TextEncoding::Gb18030, {}};
    f.doc.wct.customCode = RawText{"// 目标 名仅作注释\r\ncall BJDebugMsg(\"无关显示：目标 名\")\r\n", TextEncoding::Gb18030, {}};
    auto op = f.Remove(); auto r = f.Review(op);
    Require(r.complete && r.individualBlocks.size() == 1, "encoded comment and string purpose remain individually reviewable");
    Require(r.symbols.front() == TriggerDirectoryEditor::TriggerSymbol(f.doc.triggers.front().name), "Chinese symbol uses the actual generator mapping");
    bool encodedOffset = false;
    for (const auto& hit : r.individualBlocks.front().evidence) { Require(hit.rawRangeExact, "original encoded range is exact"); encodedOffset |= hit.rawByteOffset < hit.decodedByteOffset; }
    Require(encodedOffset, "GB18030 byte offsets are not mislabeled UTF8 offsets");
    f.Confirm(op, r); Good(ApplyTriggerGraphEditBatch(f.doc, f.catalog, f.Batch(op)), "Chinese target deletion with separately reviewed encoded code");

    Fixture p;
    p.doc.wct.customCode = RawText{"set i = i + 1\ncall BJDebugMsg(\"a\" + I2S(n))\ncall DisplayTextToPlayer(p,0,0,\"plain\")\ncall TriggerExecute(udg_t)\n"};
    auto append = TriggerDocumentEditor::AppendRootEca(p.doc, p.catalog, p.doc.triggers.front().id, UiDefinitionKind::Action, "Run"); Good(append, "self reference");
    const auto* eca = TriggerDocumentEditor::FindEca(p.doc, append.value->rootEcaId);
    p.doc.argumentNodes[eca->arguments.front()].type = 1; p.doc.argumentNodes[eca->arguments.front()].value = RawText{"gg_trg_Target_Name"};
    op = p.Remove(); r = p.Review(op);
    Require(r.complete && r.typedArgumentIds.empty() && r.groupableBlocks.size() == 3, "removed subtree references and literal arithmetic do not create false blockers");
    p.Confirm(op, r); Good(ApplyTriggerGraphEditBatch(p.doc, p.catalog, p.Batch(op)), "self reference disappears with its owner");
}
}
void DerivedLiteralClassification() {
    Fixture f;
    auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog); Good(encoded, "before catalog extension");
    std::ofstream(f.root / "ui/base/define.txt", std::ios::app)
        << "degree=0,1,1,Degree,real\nrealalias=0,1,1,Alias,degree\ndeepreal=0,1,1,Deep,realalias\n"
        << "modelfile=0,1,1,Model,string\nmodelalias=0,1,1,ModelAlias,modelfile\n"
        << "abilcode=0,1,1,Ability,integer\nabilityalias=0,1,1,Alias,abilcode\nabilityalias2=0,1,1,Alias2,abilityalias\n"
        << "scriptcode=1,0,0,Code,string\n";
    std::ofstream(f.root / "ui/base/action.txt", std::ios::app)
        << "[UseDerived]\n[[.args]]\ntype=deepreal\n[[.args]]\ntype=modelalias\n[[.args]]\ntype=abilityalias2\n"
        << "[UseCode]\n[[.args]]\ntype=scriptcode\n[UseUnknown]\n[[.args]]\ntype=MissingType\n";
    auto loaded = UiCatalog::Load(f.root / "ui"); Good(loaded, "extended original catalog"); f.catalog = *loaded.value;
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog); Good(parsed, "bind extended catalog");
    f.doc = *parsed.value; f.target = f.doc.triggers.front().id; f.category = f.doc.categories.front().id;
    auto owner = TriggerDocumentEditor::AddTrigger(f.doc, f.doc.categories.front().categoryId, "Typed owner"); Good(owner, "typed owner");
    auto derived = TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, *owner.value, UiDefinitionKind::Action, "UseDerived"); Good(derived, "derived slots");
    for (std::size_t i = 0; i < 3; ++i) Good(TriggerDocumentEditor::SetScalarArgument(f.doc, derived.value->appendedArguments[i].id, 3,
        std::array<const char*, 3>{"0.138", "Models\\original.mdx", "A001"}[i]), "valid derived literal");
    auto code = TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, *owner.value, UiDefinitionKind::Action, "UseCode"); Good(code, "source slot");
    Good(TriggerDocumentEditor::SetScalarArgument(f.doc, code.value->appendedArguments.front().id, 3, "gg_trg_Target_Name"), "source symbol");
    auto unknown = TriggerDocumentEditor::AppendRootEca(f.doc, f.catalog, *owner.value, UiDefinitionKind::Action, "UseUnknown"); Good(unknown, "unknown slot");
    Good(TriggerDocumentEditor::SetScalarArgument(f.doc, unknown.value->appendedArguments.front().id, 3, "null"), "unknown null fixture");
    auto inventory = InspectOpaqueArgumentInventory(f.doc, f.catalog); Good(inventory, "same classifier inventory");
    for (const auto& row : inventory.value->classes) {
        if (row.expectedType == "deepreal" || row.expectedType == "modelalias" || row.expectedType == "abilityalias2")
            Require(row.validateLiteral && !row.opaque, "validated literal chains do not require opaque review");
        if (row.expectedType == "scriptcode" || row.expectedType == "MissingType") Require(row.opaque, "source and unknown types remain auditable");
    }
    auto op = f.Remove(); auto review = f.Review(op); f.Confirm(op, review); f.Fails(op, "TRIGGER_GRAPH_REMOVE_REFERENCED");
}
int main() { try { GroupedAndStale(); IndividualAndReferences(); TypedDynamicAndVariable(); EncodingAndPrefix(); DerivedLiteralClassification(); std::cout << "Removal audit transactions passed\n"; return 0; } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
