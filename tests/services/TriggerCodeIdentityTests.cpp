#include "War3MapCore/Services/TriggerCodeIdentityEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerTransactionSnapshot.h"
#include "War3MapCore/Jass/IdentifierRewrite.h"
#include "War3MapCore/Hash.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
using namespace War3Map::Services;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) { for (const auto& d : result.diagnostics) std::cerr << d.code << ' ' << d.message << '\n'; throw std::runtime_error("expected result"); }
    return std::move(*result.value);
}
template<class T> bool Code(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
std::string Body(std::string stem) {
    return "// InitTrig_" + stem + " gg_trg_" + stem + "\r\nfunction Trig_" + stem + "_Actions takes nothing returns nothing\r\n"
        "call BJDebugMsg(\"InitTrig_" + stem + " gg_trg_" + stem + " Trig_" + stem + "_Actions\")\r\nendfunction\r\n"
        "function InitTrig_" + stem + " takes nothing returns nothing\r\nset gg_trg_" + stem + " = CreateTrigger()\r\n"
        "call TriggerAddAction(gg_trg_" + stem + ", function Trig_" + stem + "_Actions)\r\nendfunction\r\n";
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("Audastra-CodeIdentity-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    UiCatalog catalog; TriggerDocument document; EncodedTriggerDocument original;
    StableId id = 0;
    Fixture(std::string name = "Old", TextEncoding encoding = TextEncoding::Utf8) {
        std::filesystem::create_directories(root / "ui/base");
        std::ofstream(root / "ui/config") << "base\n";
        std::ofstream(root / "ui/base/define.txt") << "[TriggerTypes]\ntrigger=0,1,0,Trigger,trigger\nstring=0,1,1,String,string\nscriptcode=0,0,0,Script,string\n";
        std::ofstream(root / "ui/base/action.txt") << "[UseTrigger]\n[[.args]]\ntype=trigger\n[CustomScriptCode]\n[[.args]]\ntype=string\n";
        catalog = Take(UiCatalog::Load(root / "ui"));
        document = Take(WtgWctCodec{}.CreateEmpty(catalog));
        const auto category = Take(TriggerDirectoryEditor::AddCategory(document, "Original"));
        const auto added = Take(TriggerDocumentEditor::AddTrigger(document, category, name));
        auto* row = TriggerDocumentEditor::FindTrigger(document, added); row->name.encoding = encoding;
        row->customTextFlag = 1; row->customText.text = Body(TriggerDirectoryEditor::ScriptName(row->name));
        const auto stem = TriggerDirectoryEditor::ScriptName(row->name);
        const auto other = Take(TriggerDocumentEditor::AddTrigger(document, category, "Other"));
        auto* otherRow = TriggerDocumentEditor::FindTrigger(document, other); otherRow->customTextFlag = 1;
        otherRow->customText.text = "function InitTrig_Other takes nothing returns nothing\ncall InitTrig_" + stem + "()\ncall Trig_" + stem + "_Actions()\nendfunction\n";
        const auto gui = Take(TriggerDocumentEditor::AddTrigger(document, category, "Gui"));
        auto eca = Take(TriggerDocumentEditor::AppendRootEca(document, catalog, gui, UiDefinitionKind::Action, "UseTrigger"));
        auto* node = TriggerDocumentEditor::FindEca(document, eca.rootEcaId);
        auto& arg = document.argumentNodes[node->arguments[0]]; arg.type = 1; arg.value.text = "gg_trg_" + stem;
        const auto codeEca = Take(TriggerDocumentEditor::AppendRootEca(document, catalog, gui, UiDefinitionKind::Action, "CustomScriptCode"));
        auto* codeNode = TriggerDocumentEditor::FindEca(document, codeEca.rootEcaId);
        auto& codeArg = document.argumentNodes[codeNode->arguments[0]]; codeArg.type = 3; codeArg.value.text = "call InitTrig_" + stem + "()";
        document.wct.customCode.text = "function SharedCaller takes nothing returns nothing\ncall InitTrig_" + stem + "()\nendfunction\n";
        original = Take(WtgWctCodec{}.Serialize(document, catalog)); document = Take(WtgWctCodec{}.Parse(original.wtg, original.wct, catalog));
        id = document.triggers[0].id;
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    TriggerCodeIdentityContext Context() const { return {&document, &catalog, nullptr, TriggerCodeScriptTarget::Jass, true, {}, {}}; }
    TriggerTransactionBinding Binding() const { const std::string sha(64, 'a'); return {sha, 1, {1, sha, catalog.Sha256()}, 1}; }
    TriggerCodeIdentityRequest Request(std::string name = "New", bool copy = false) const {
        const auto& row = document.triggers[0];
        TriggerCodeIdentityRequest request; request.action = copy ? TriggerCodeIdentityAction::Copy : TriggerCodeIdentityAction::Rename;
        request.expected = Take(CaptureTriggerGraphBinding(document, catalog)); request.triggerId = id; request.expectedName = row.name;
        request.expectedCodeSha256 = Take(Sha256(Take(row.customText.ToBytes()))); request.name = name; request.categoryId = row.categoryId;
        for (const auto& t : document.triggers) if (t.categoryId == row.categoryId) request.expectedCategoryOrder.push_back(t.id);
        request.afterTriggerId = id; return request;
    }
    void Reseal() { original = Take(WtgWctCodec{}.Serialize(document, catalog)); document = Take(WtgWctCodec{}.Parse(original.wtg, original.wct, catalog)); id = document.triggers[0].id; }
};
void RenameAndFreeze() {
    Fixture f; const auto before = f.document; const auto request = f.Request();
    const auto plan = Take(TriggerCodeIdentityEditing::Prepare(f.Context(), request));
    Require(plan.Summary().changed && plan.Summary().changedBlocks == 4, "all WCT and code parameter references prepared");
    Require(f.document.triggers[0].name.text == "Old", "prepare read only");
    Take(TriggerCodeIdentityEditing::Apply(f.Context(), f.document, plan));
    Require(f.document.triggers[0].id == f.id && f.document.triggers[0].name.text == "New", "rename same identity");
    const auto& code = f.document.triggers[0].customText.text;
    Require(code.find("function InitTrig_New") != code.npos && code.find("function Trig_New_Actions") != code.npos, "definitions renamed");
    Require(code.find("// InitTrig_Old gg_trg_Old\r\n") != code.npos && code.find("\"InitTrig_Old gg_trg_Old Trig_Old_Actions\"") != code.npos, "comments strings and CRLF unchanged");
    Require(f.document.triggers[1].customText.text.find("call Trig_New_Actions") != std::string::npos, "other custom reference renamed");
    Require(f.document.argumentNodes[0].value.text == "gg_trg_New", "GUI trigger parameter renamed");
    Require(f.document.argumentNodes[1].value.text == "call InitTrig_New()", "GUI code parameter renamed through lexer");
    const auto frozen = Take(TriggerTransactionSnapshot::Freeze(f.document, f.catalog, f.original.wtg, f.original.wct, f.Binding()));
    Require(frozen->Document().triggers[0].name.text == "New", "authorized transaction frozen");
    f.document.triggers[0].customText.text += "// unauthorized\n";
    Require(!TriggerTransactionSnapshot::Freeze(f.document, f.catalog, f.original.wtg, f.original.wct, f.Binding()).Succeeded(), "post-plan code tamper rejected");
    f.document = before;
    auto batch = TriggerGraphEditBatch{Take(CaptureTriggerGraphBinding(f.document, f.catalog)), {plan.Operation(), GraphRename{{GraphRecordKind::Trigger, f.id, {}}, RawText{"wrong"}, "Oops", {}}}};
    Require(!ApplyTriggerGraphEditBatch(f.document, f.catalog, batch).Succeeded(), "suffix failure rejects batch");
    Require(f.document.triggers[0].name.text == "Old", "no authorized prefix leaked");
}
void CopyAndGuards() {
    Fixture f; const auto old = Take(WtgWctCodec{}.Serialize(f.document, f.catalog));
    const auto plan = Take(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request("Clone", true)));
    Take(TriggerCodeIdentityEditing::Apply(f.Context(), f.document, plan));
    Require(f.document.triggers.size() == 4 && plan.Summary().resultId != f.id, "new stable identity");
    Require(f.document.triggers[0].name.text == "Old" && f.document.triggers[1].name.text == "Clone", "copy adjacent");
    Require(f.document.triggers[0].customText.text == Body("Old") && f.document.triggers[2].customText.text.find("InitTrig_Old") != std::string::npos, "source and external references preserved");
    Require(f.document.triggers[1].customText.text.find("function Trig_Clone_Actions") != std::string::npos, "owned helpers copied");
    Take(TriggerTransactionSnapshot::Freeze(f.document, f.catalog, f.original.wtg, f.original.wct, f.Binding()));
    f.document = Take(WtgWctCodec{}.Parse(old.wtg, old.wct, f.catalog));
    auto request = f.Request(); auto context = f.Context(); context.target = TriggerCodeScriptTarget::Lua;
    Require(Code(TriggerCodeIdentityEditing::Prepare(context, request), "TRIGGER_CODE_LUA_UNSUPPORTED"), "actual Lua refused");
    context = f.Context(); context.opaqueScriptSources = {"opaque-loader"};
    Require(Code(TriggerCodeIdentityEditing::Prepare(context, request), "TRIGGER_CODE_COVERAGE_INCOMPLETE"), "opaque coverage refused");
    context = f.Context(); bool ready = true; context.verify = [&] { return ready ? Status::Ok() : Status::Failure("test", "STALE", "来源变更"); };
    const auto stalePlan = Take(TriggerCodeIdentityEditing::Prepare(context, request)); ready = false;
    Require(!TriggerCodeIdentityEditing::Apply(context, f.document, stalePlan).Succeeded(), "publication source drift refused");
    auto wrong = f.Request("Clone", true); wrong.expectedCategoryOrder.clear();
    Require(Code(TriggerCodeIdentityEditing::Prepare(f.Context(), wrong), "TRIGGER_CODE_SOURCE_STALE"), "directory CAS");
    const auto same = Take(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request("Old")));
    Require(!same.Summary().changed && Take(TriggerCodeIdentityEditing::Apply(f.Context(), f.document, same)).codeIdentityChanges == 0, "no-op no publication");
}
void RejectionsAndNames() {
    { Fixture f; f.document.wct.customCode.text += "function UnapprovedPayload takes nothing returns nothing\nendfunction\n";
      const auto plan = Take(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request())); Take(TriggerCodeIdentityEditing::Apply(f.Context(), f.document, plan));
      Require(!TriggerTransactionSnapshot::Freeze(f.document, f.catalog, f.original.wtg, f.original.wct, f.Binding()).Succeeded(), "identity proof must not adopt unrelated prior code changes"); }
    { Fixture f; f.document.wct.customCode.text += "function InitTrig_OldExtra takes nothing returns nothing\nendfunction\n"; f.Reseal();
      const auto plan = Take(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request())); Take(TriggerCodeIdentityEditing::Apply(f.Context(), f.document, plan));
      Require(f.document.wct.customCode.text.find("function InitTrig_OldExtra") != std::string::npos, "full identifier boundary, not prefix replace"); }
    { Fixture f; f.document.wct.customCode.text += "function Shadow takes trigger gg_trg_Old returns nothing\ncall Use(gg_trg_Old)\nendfunction\n"; f.Reseal();
      Require(Code(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request()), "TRIGGER_CODE_BINDING_UNSUPPORTED"), "local/parameter shadow never globally replaced"); }
    for (const bool copy : {false, true}) {
        Fixture f; f.document.wct.customCode.text += "function Trig_Old_Foreign takes nothing returns nothing\nendfunction\n"; f.Reseal();
        const auto rejected = TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request("New", copy));
        Require(Code(rejected, "TRIGGER_CODE_SYMBOL_CONFLICT"), "foreign same-prefix declaration refused for both");
        Require(rejected.diagnostics[0].message.find("全局WCT 4:1") != std::string::npos, "human location provided");
    }
    for (const auto extra : {"scope Unsafe\nendscope\n", "//! runtextmacro Unsafe()\n", "function Dynamic takes nothing returns nothing\ncall ExecuteFunc(\"InitTrig_Old\")\nendfunction\n"}) {
        Fixture f; f.document.wct.customCode.text += extra; f.Reseal();
        Require(!TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request()).Succeeded(), "unbound vJass macro dynamic refused");
    }
    { Fixture f; f.document.triggers[0].customText.text += "function UserHelper takes nothing returns nothing\nendfunction\n"; f.Reseal();
      Require(Code(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request("Clone", true)), "TRIGGER_CODE_COPY_DECLARATION_UNSUPPORTED"), "user declarations not blindly cloned"); }
    for (auto encoding : {TextEncoding::Utf8, TextEncoding::Gb18030}) {
        Fixture f("中文甲", encoding); const auto plan = Take(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request("另一名称")));
        Take(TriggerCodeIdentityEditing::Apply(f.Context(), f.document, plan));
        const auto stem = TriggerDirectoryEditor::ScriptName(f.document.triggers[0].name);
        Require(f.document.triggers[0].customText.text.find("function InitTrig_" + stem) != std::string::npos, "Chinese storage mapping matches generator");
        Require(f.document.triggers[0].name.encoding == encoding, "name encoding retained");
    }
    { Fixture f("中文甲"); Require(Code(TriggerCodeIdentityEditing::Prepare(f.Context(), f.Request("中文乙", true)), "TRIGGER_CODE_SYMBOL_CONFLICT"), "same underscore Chinese symbol collision"); }
    { Fixture f; const auto gui = f.document.triggers.back(); auto request = f.Request(); request.triggerId = gui.id; request.expectedName = gui.name;
      request.expectedCodeSha256 = Take(Sha256(Take(gui.customText.ToBytes())));
      Require(Code(TriggerCodeIdentityEditing::Prepare(f.Context(), request), "TRIGGER_CODE_BINDING_UNSUPPORTED"), "ordinary GUI not accepted"); }
}
}
int main() { try { RenameAndFreeze(); CopyAndGuards(); RejectionsAndNames(); std::cout << "code identity service cases passed\n"; return 0; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
