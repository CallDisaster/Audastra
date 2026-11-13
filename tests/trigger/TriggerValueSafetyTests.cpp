#include "War3MapCore/Trigger/TriggerReal.h"
#include "War3MapCore/Trigger/TriggerValueSafety.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <algorithm>

using namespace War3Map;
using namespace War3Map::Trigger;
namespace {
void Require(bool b, const char* text) { if (!b) throw std::runtime_error(text); }
template<class T> void Good(const T& r, const char* text) {
    if (!r.Succeeded()) for (const auto& d : r.diagnostics) std::cerr << d.code << ':' << d.message << '\n';
    Require(r.Succeeded(), text);
}
template<class T> bool Code(const T& r, std::string_view code) {
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [&](const auto& d) { return d.code == code; });
}
void Numbers() {
    Require(ShortestTriggerReal("0.13799999999999998") == "0.138", "same float32 short form");
    Require(ShortestTriggerReal("-0.000") == "-0.0", "negative zero preserved");
    Require(ShortestTriggerReal("1.0") == "1.0", "integral real syntax");
    Require(ShortestTriggerReal("0.0000000001") == "0.0000000001", "expand exponent");
    Require(!ShortestTriggerReal("nan") && !ShortestTriggerReal("1e3") && !FormatTriggerReal(std::numeric_limits<double>::infinity()), "invalid literal rejected");
    Require(!FormatTriggerReal(1e-300) && !FormatTriggerReal(1e300), "no silent overflow or underflow");
    const auto warning = CheckTriggerRealLiteral("0.13799999999999998");
    Require(warning && warning->severity == DiagnosticSeverity::Warning && warning->message.find("0.138") != warning->message.npos, "warning gives suggestion");
    Require(!CheckTriggerRealLiteral("0.138") && !CheckTriggerRealLiteral("\"0.13799999999999998\""), "short text and strings clean");
    Require(CheckTriggerRealLiteral("0.0000000001").has_value(), "unsafe short form has warning without false safer suggestion");
}
void Publishing() {
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-ValueSafety-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directory(root), "fresh original fixture");
    std::filesystem::create_directories(root / "ui/base");
    std::ofstream(root / "ui/config") << "base\n";
    std::ofstream(root / "ui/base/define.txt") << "[TriggerTypes]\nreal=0,1,1,Real,real\ninteger=0,1,1,Integer,integer\nstring=0,1,1,String,string\nhandle=0,1,1,Handle,handle\nunit=0,1,1,Unit,unit\n";
    std::ofstream(root / "ui/base/action.txt") << "[RecordReal]\n[[.args]]\ntype=real\n[RecordInteger]\n[[.args]]\ntype=integer\n";
    std::ofstream(root / "ui/base/call.txt") << "[GetHandleId]\nreturns=integer\n[[.args]]\ntype=handle\n[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=string\n";
    const auto loaded = UiCatalog::Load(root / "ui"); Good(loaded, "catalog");
    const auto& catalog = *loaded.value;
    auto created = WtgWctCodec{}.CreateEmpty(catalog); Good(created, "source"); auto doc = *created.value;
    const auto category = TriggerDirectoryEditor::AddCategory(doc, "Original"); Good(category, "category");
    const auto trigger = TriggerDocumentEditor::AddTrigger(doc, *category.value, "Original"); Good(trigger, "trigger");
    const auto replace = [&](std::string text) {
        const auto binding = CaptureTriggerGraphBinding(doc, catalog); Good(binding, "binding");
        return ApplyTriggerGraphEditBatch(doc, catalog, {*binding.value, {GraphReplaceTriggerText{{GraphRecordKind::Trigger, *trigger.value, {}}, std::move(text)}}});
    };
    const auto edited = replace("actions:\r\n    RecordReal(0.13799999999999998)\r\n");
    Good(edited, "precision warning does not reject publication");
    Require(Code(edited, "TRIGGER_REAL_LITERAL_PRECISION_RISK"), "graph edit retains warning");
    Require(edited.diagnostics.front().message.find("第 2 行第 16 列") != std::string::npos, "exact original argument location");
    Require(doc.argumentNodes.front().value.text == "0.13799999999999998", "author bytes not rewritten");
    const auto scalarBinding = CaptureTriggerGraphBinding(doc, catalog); Good(scalarBinding, "scalar binding");
    const auto scalar = PrepareTriggerGraphScalarEditBatch(doc, catalog, {*scalarBinding.value, {GraphSet{
        {GraphRecordKind::Argument, doc.argumentNodes.front().id, {}}, GraphField::ArgumentScalar,
        GraphScalarValue{3, "0.13799999999999998"}, GraphScalarValue{3, "0.23333333333333333"}}}});
    Good(scalar, "warning permits compact scalar preparation");
    Require(Code(scalar, "TRIGGER_REAL_LITERAL_PRECISION_RISK") && doc.argumentNodes.front().value.text == "0.13799999999999998",
        "compact scalar receipt retains warning and preparation preserves source");
    std::vector<TriggerTextArgumentAnchor> anchors;
    const auto rendered = RenderTriggerText(doc, catalog, *trigger.value, 4096, nullptr, nullptr, &anchors); Good(rendered, "render");
    Require(rendered.value->find("RecordReal(0.138)") != std::string::npos && anchors.size() == 1 && anchors.front().column == 16, "editor output shortest and exact argument anchor");
    const auto before = WtgWctCodec{}.Serialize(doc, catalog); Good(before, "before");
    const auto failed = replace("actions:\n    RecordInteger(GetHandleId(YDWEGetAnyTypeLocalVariable(\"英雄\")))\n");
    Require(!failed.Succeeded() && Code(failed, "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE"), "generic handle needs actual snapshot before publication");
    const auto after = WtgWctCodec{}.Serialize(doc, catalog); Good(after, "after");
    Require(before.value->wtg == after.value->wtg && before.value->wct == after.value->wct, "failed transaction preserves paired source");
    Good(replace("actions:\n    ~RecordInteger(GetHandleId(YDWEGetAnyTypeLocalVariable(\"英雄\")))\n"), "disabled issue only warns");
    const auto ecaId = doc.ecaNodes[doc.triggers.front().ecas.front()].id;
    const auto binding = CaptureTriggerGraphBinding(doc, catalog); Good(binding, "enable binding");
    const auto enabled = ApplyTriggerGraphEditBatch(doc, catalog, {*binding.value, {GraphSet{{GraphRecordKind::Eca, ecaId, {}}, GraphField::Enabled, false, true}}});
    Require(!enabled.Succeeded() && Code(enabled, "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE"), "enabling checks descendants before publication");
    Good(TriggerDocumentEditor::SetEcaEnabled(doc, ecaId, true), "fixture simulates an active legacy issue");
    const auto legacyBinding = CaptureTriggerGraphBinding(doc, catalog); Good(legacyBinding, "legacy binding");
    const auto unrelated = ApplyTriggerGraphEditBatch(doc, catalog, {*legacyBinding.value, {GraphSet{
        {GraphRecordKind::Trigger, *trigger.value, {}}, GraphField::Description, std::string{}, std::string{"label"}}}});
    Good(unrelated, "unrelated edit is not blocked by an old local read");
    const auto name = std::find_if(doc.argumentNodes.begin(), doc.argumentNodes.end(), [](const auto& arg) { return arg.value.text == "英雄"; });
    Require(name != doc.argumentNodes.end(), "legacy local name argument");
    const auto nameBinding = CaptureTriggerGraphBinding(doc, catalog); Good(nameBinding, "nested name binding");
    const auto renamed = ApplyTriggerGraphEditBatch(doc, catalog, {*nameBinding.value, {GraphSet{
        {GraphRecordKind::Argument, name->id, {}}, GraphField::ArgumentScalar, GraphScalarValue{3, "英雄"}, GraphScalarValue{3, "另名"}}}});
    Require(!renamed.Succeeded() && Code(renamed, "TRIGGER_VALUE_CHECK_CONTEXT_INCOMPLETE"), "editing a nested name rechecks its containing generic read");
    std::error_code ec; std::filesystem::remove_all(root, ec);
}
// 设置变量的目标被存成文字时，值槽（Null）定不下类型：报出真实原因，不报“Null 没有存取宏”，也不去要 Null 的存取能力。
void UnresolvedNull() {
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-ValueNull-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directory(root), "fresh null fixture");
    std::filesystem::create_directories(root / "ui/base");
    std::ofstream(root / "ui/config") << "base\n";
    std::ofstream(root / "ui/base/define.txt") << "[TriggerTypes]\nunit=0,1,1,Unit,unit\nstring=0,1,1,String,string\nAnyGlobal=0,0,0,Variable\nNull=0,0,0,Value\n";
    std::ofstream(root / "ui/base/action.txt") << "[SetVariable]\n[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n";
    std::ofstream(root / "ui/base/call.txt") << "[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=string\n";
    const auto loaded = UiCatalog::Load(root / "ui"); Good(loaded, "null catalog");
    const auto& catalog = *loaded.value;
    auto created = WtgWctCodec{}.CreateEmpty(catalog); Good(created, "null source"); auto doc = *created.value;
    const auto category = TriggerDirectoryEditor::AddCategory(doc, "Original"); Good(category, "null category");
    const auto trigger = TriggerDocumentEditor::AddTrigger(doc, *category.value, "Bind"); Good(trigger, "null trigger");
    Good(TriggerDocumentEditor::AddVariable(doc, "Hero", "unit"), "unit variable");
    const auto set = TriggerDocumentEditor::AppendRootEca(doc, catalog, *trigger.value, UiDefinitionKind::Action, "SetVariable"); Good(set, "set");
    const auto* eca = TriggerDocumentEditor::FindEca(doc, set.value->rootEcaId);
    const auto target = doc.argumentNodes[eca->arguments[0]].id, value = doc.argumentNodes[eca->arguments[1]].id;
    Good(TriggerDocumentEditor::SetScalarArgument(doc, target, 3, "Hero"), "target stored as text");
    const auto read = TriggerDocumentEditor::AppendArgumentCall(doc, catalog, value, "Null", "YDWEGetAnyTypeLocalVariable"); Good(read, "local read");
    const auto* call = TriggerDocumentEditor::FindEca(doc, read.value->rootEcaId);
    Good(TriggerDocumentEditor::SetScalarArgument(doc, doc.argumentNodes[call->arguments[0]].id, 3, "u"), "local name");
    auto report = AnalyzeTriggerValues(doc, catalog); Good(report, "analysis");
    Require(report.value->issues.size() == 1, "one unresolved issue");
    const auto& issue = report.value->issues.front();
    Require(issue.diagnostic.code == "TRIGGER_LOCAL_VALUE_TYPE_UNRESOLVED" && issue.diagnostic.severity == DiagnosticSeverity::Error
        && issue.storageType.empty() && issue.diagnostic.message.find("直接值") != std::string::npos
        && issue.diagnostic.message.find("Hero") != std::string::npos && issue.diagnostic.message.find("父槽") == std::string::npos,
        "names the text target instead of a missing Null storage macro");
    Require(RequiredTriggerStorageTypes(doc, catalog).empty(), "no capability request for Null");
    Good(TriggerDocumentEditor::SetScalarArgument(doc, target, 1, "Hero"), "target as variable");
    report = AnalyzeTriggerValues(doc, catalog); Good(report, "reanalysis");
    Require(report.value->issues.empty(), "unit variable target resolves the read");
    std::error_code ec; std::filesystem::remove_all(root, ec);
}
}
int main() { try { Numbers(); Publishing(); UnresolvedNull(); std::cout << "trigger value safety passed\n"; return 0; } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
