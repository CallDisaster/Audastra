#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/GuiConversionSnapshots.h"

#include <Windows.h>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// 代码转回 T：原创函数库与触发器，覆盖解析转回、改值后转回、快照原样恢复、拒绝定位与撤销重做。
namespace {
using namespace War3Map;
using namespace War3Map::Trigger;
void Require(bool value, std::string_view reason) { if (!value) throw std::runtime_error(std::string(reason)); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) {
        for (const auto& diagnostic : result.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        throw std::runtime_error("共用操作失败。");
    }
    return std::move(*result.value);
}
void Check(Status status) {
    for (const auto& diagnostic : status.diagnostics) if (!status.Succeeded()) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Require(status.Succeeded(), "重放被拒绝。");
}
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary); stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(stream.good(), "不能建立原创目录。");
}
EncodedTriggerDocument Wire(const TriggerDocument& document, const UiCatalog& catalog) {
    return Take(WtgWctCodec{}.Serialize(document, catalog));
}

// 片段搭建：局部下标。
struct Builder {
    GuiTriggerFragment fragment;
    std::uint32_t Arg(std::int32_t type, std::string value, std::optional<std::uint32_t> call = std::nullopt,
        std::optional<std::uint32_t> index = std::nullopt) {
        ArgumentNode node; node.type = type; node.value.text = std::move(value);
        if (call) { node.insertCall = 1; node.callEca = call; }
        if (index) { node.insertIndex = 1; node.indexArgument = index; }
        fragment.arguments.push_back(std::move(node));
        return static_cast<std::uint32_t>(fragment.arguments.size() - 1);
    }
    std::uint32_t Eca(std::int32_t type, std::string name, std::vector<std::uint32_t> args,
        std::optional<std::int32_t> childId = std::nullopt, std::vector<std::uint32_t> children = {}) {
        EcaNode node; node.type = type; node.name.text = std::move(name); node.childId = childId;
        node.arguments = std::move(args); node.children = std::move(children);
        fragment.ecas.push_back(std::move(node));
        return static_cast<std::uint32_t>(fragment.ecas.size() - 1);
    }
};

GuiTriggerFragment Sample(bool withDisabled) {
    Builder b;
    const auto periodic = b.Eca(0, "TriggerRegisterTimerEventPeriodic", {b.Arg(3, "2.00")});
    const auto less = b.Eca(1, "OperatorCompareInteger", {b.Arg(1, "Count"), b.Arg(0, "OperatorLess"), b.Arg(3, "5")});
    const auto add = b.Eca(3, "OperatorInt", {b.Arg(1, "Count"), b.Arg(0, "OperatorAdd"), b.Arg(3, "1")});
    const auto set = b.Eca(2, "SetVariable", {b.Arg(1, "Count"), b.Arg(2, "OperatorInt", add)});
    const auto comment = b.Eca(2, "CommentString", {b.Arg(3, "增加计数")});
    const auto player = b.Eca(3, "GetTriggerPlayer", {});
    const auto show = b.Eca(2, "DisplayTextToPlayer", {b.Arg(2, "GetTriggerPlayer", player), b.Arg(3, "0"), b.Arg(3, "0"), b.Arg(3, "Hi \"x\"")});
    const auto ifCondition = b.Eca(1, "OperatorCompareInteger", {b.Arg(1, "Count"), b.Arg(0, "OperatorEqual"), b.Arg(3, "3")}, 0);
    const auto thenAction = b.Eca(2, "DoNothing", {}, 1);
    const auto identity = b.Eca(3, "Identity", {b.Arg(3, "3")});
    const auto elseAction = b.Eca(2, "SetVariable", {b.Arg(1, "Slots", std::nullopt, b.Arg(3, "2")), b.Arg(2, "Identity", identity)}, 2);
    const auto branch = b.Eca(2, "IfThenElseMultiple", {}, std::nullopt, {ifCondition, thenAction, elseAction});
    const auto body = b.Eca(2, "DoNothing", {}, 0);
    const auto loop = b.Eca(2, "ForLoopAMultiple", {b.Arg(3, "1"), b.Arg(3, "3")}, std::nullopt, {body});
    b.fragment.roots = {periodic, less, set, comment, show, branch, loop};
    if (withDisabled) {
        const auto disabled = b.Eca(2, "DoNothing", {});
        b.fragment.ecas[disabled].enabled = 0;
        b.fragment.roots.push_back(disabled);
    }
    return b.fragment;
}

// 结构比较（不含稳定身份与编码）：名称、类型、分支号、开关、参数值与嵌套。
std::string Shape(const GuiTriggerFragment& fragment) {
    std::string out;
    std::function<void(std::uint32_t)> arg;
    std::function<void(std::uint32_t)> eca = [&](std::uint32_t index) {
        const auto& node = fragment.ecas[index];
        out += "<" + std::to_string(node.type) + ":" + node.name.text + ":" + (node.childId ? std::to_string(*node.childId) : "-")
            + ":" + std::to_string(node.enabled) + "(";
        for (const auto a : node.arguments) arg(a);
        out += ")[";
        for (const auto c : node.children) eca(c);
        out += "]>";
    };
    arg = [&](std::uint32_t index) {
        const auto& node = fragment.arguments[index];
        out += "{" + std::to_string(node.type) + ":" + node.value.text;
        if (node.callEca) eca(*node.callEca);
        if (node.indexArgument) { out += "["; arg(*node.indexArgument); out += "]"; }
        out += "}";
    };
    for (const auto root : fragment.roots) eca(root);
    return out + (fragment.runOnInit ? "+init" : "");
}

struct Fixture {
    TriggerDocument document;
    StableId id = 0;
};
Fixture GuiFixture(const UiCatalog& catalog, bool withDisabled) {
    Fixture f;
    f.document = Take(WtgWctCodec{}.CreateEmpty(catalog));
    Take(TriggerDirectoryEditor::AddCategory(f.document, "Code"));
    Check(TriggerDocumentEditor::AddVariable(f.document, "Count", "integer", false));
    Check(TriggerDocumentEditor::AddVariable(f.document, "Slots", "integer", true));
    // 先建代码触发器，再用结构计划换成 GUI 片段（原创夹具，不经解析）。
    auto created = Take(TriggerCodeEditor::PrepareNewCustom(f.document, catalog, f.document.categories.front().categoryId, "Hero Revive"));
    Take(TriggerCodeEditor::Apply(f.document, catalog, created));
    f.id = created.Target().triggerId;
    auto gui = Take(TriggerCodeEditor::PrepareRecovery(f.document, catalog, f.id, Sample(withDisabled)));
    Take(TriggerCodeEditor::Apply(f.document, catalog, gui));
    Check(WtgWctCodec{}.Validate(f.document, catalog));
    return f;
}
std::string Convert(Fixture& f, const UiCatalog& catalog) {
    Script::MapScriptInputs inputs; inputs.triggers = &f.document; inputs.catalog = &catalog;
    auto source = Take(Script::GenerateGuiTriggerSource(inputs, f.id));
    auto plan = Take(TriggerCodeEditor::PrepareConversion(f.document, catalog, f.id, source.text, source.effectiveRunOnInit));
    Take(TriggerCodeEditor::Apply(f.document, catalog, plan));
    return source.text;
}
Result<Script::GuiTriggerRecovery> Recover(Fixture& f, const UiCatalog& catalog, const GuiTriggerFragment* snapshot = nullptr) {
    Script::MapScriptInputs inputs; inputs.triggers = &f.document; inputs.catalog = &catalog;
    return Script::RecoverGuiTrigger(inputs, f.id, snapshot);
}
void SetText(Fixture& f, const UiCatalog& catalog, std::string text) {
    auto plan = Take(TriggerCodeEditor::PrepareText(f.document, catalog, {CodeBlockKind::CustomTrigger, f.id}, text));
    Take(TriggerCodeEditor::Apply(f.document, catalog, plan));
}
std::string Replace(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    Require(at != std::string::npos, "样例正文缺少要修改的片段：" + std::string(from));
    return text.replace(at, from.size(), to);
}

void ParsedRoundTrip(const UiCatalog& catalog) {
    auto f = GuiFixture(catalog, false);
    const auto original = Shape(Take(TriggerCodeEditor::CaptureGuiFragment(f.document, f.id)));
    const auto text = Convert(f, catalog);
    const auto beforeRecovery = Wire(f.document, catalog);
    auto recovered = Take(Recover(f, catalog));
    Require(!recovered.fromSnapshot, "没有快照时应按正文解析。");
    Require(Shape(recovered.fragment) == original, "解析转回的结构须与原 GUI 一致：\n" + Shape(recovered.fragment) + "\n" + original);
    auto plan = Take(TriggerCodeEditor::PrepareRecovery(f.document, catalog, f.id, recovered.fragment));
    auto receipt = Take(TriggerCodeEditor::Apply(f.document, catalog, plan));
    Check(WtgWctCodec{}.Validate(f.document, catalog));
    const auto* record = TriggerDocumentEditor::FindTrigger(f.document, f.id);
    Require(record && record->customTextFlag == 0 && record->customText.text.empty() && record->name.text == "Hero Revive", "转回后为 GUI 触发器。");
    Script::MapScriptInputs inputs; inputs.triggers = &f.document; inputs.catalog = &catalog;
    Require(Take(Script::GenerateGuiTriggerSource(inputs, f.id)).text == text, "转回后再生成须与原代码一致。");
    const auto afterRecovery = Wire(f.document, catalog);
    Check(TriggerCodeEditor::Replay(f.document, catalog, receipt, false));
    const auto undone = Wire(f.document, catalog);
    Require(undone.wtg == beforeRecovery.wtg && undone.wct == beforeRecovery.wct, "撤销转回须逐字节恢复代码触发器。");
    Check(TriggerCodeEditor::Replay(f.document, catalog, receipt, true));
    const auto redone = Wire(f.document, catalog);
    Require(redone.wtg == afterRecovery.wtg && redone.wct == afterRecovery.wct, "重做转回须逐字节一致。");
}

void EditedValues(const UiCatalog& catalog) {
    auto f = GuiFixture(catalog, false);
    auto text = Convert(f, catalog);
    // 作者改了比较值和字符串，并改动了缩进与空格：仍可转回，新值进入 T。
    text = Replace(text, "< 5)", "<   7 )");
    text = Replace(text, "\"Hi \\\"x\\\"\"", "\"Bye\"");
    text = Replace(text, "    call DoNothing()", "\t\tcall DoNothing( )");
    SetText(f, catalog, text);
    auto recovered = Take(Recover(f, catalog));
    const auto shape = Shape(recovered.fragment);
    Require(shape.find("{3:7}") != std::string::npos && shape.find("{3:Bye}") != std::string::npos, "改动的值须进入转回结果：" + shape);
}

void Rejections(const UiCatalog& catalog) {
    auto f = GuiFixture(catalog, false);
    const auto text = Convert(f, catalog);
    const auto rejected = [&](std::string edited, std::string_view expectLine) {
        SetText(f, catalog, std::move(edited));
        const auto result = Recover(f, catalog);
        Require(!result.Succeeded() && !result.diagnostics.empty(), "应当拒绝转回。");
        const auto& message = result.diagnostics.front().message;
        Require(message.find(expectLine) != std::string::npos, "拒绝须指出行号 " + std::string(expectLine) + "：" + message);
    };
    // 不认识的动作：指向该行。
    auto lines = std::vector<std::string>{};
    {
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto stop = text.find('\n', start);
            lines.push_back(text.substr(start, stop == std::string::npos ? std::string::npos : stop - start));
            if (stop == std::string::npos) break;
            start = stop + 1;
        }
    }
    std::size_t actionsLine = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) if (lines[i].find("Actions takes") != std::string::npos) actionsLine = i + 1;
    Require(actionsLine > 0, "样例缺少动作函数。");
    auto unknown = lines; unknown.insert(unknown.begin() + static_cast<std::ptrdiff_t>(actionsLine), "    call UnknownThing()");
    std::string joined; for (const auto& line : unknown) joined += line + "\n";
    joined.pop_back();
    rejected(joined, "第 " + std::to_string(actionsLine + 1) + " 行");
    // 自定义局部变量不是 T 写法。
    auto local = lines; local.insert(local.begin() + static_cast<std::ptrdiff_t>(actionsLine), "    local integer x = 1");
    joined.clear(); for (const auto& line : local) joined += line + "\n";
    joined.pop_back();
    rejected(joined, "第 " + std::to_string(actionsLine + 1) + " 行");
    // 未改正文仍可转回。
    SetText(f, catalog, text);
    Require(Recover(f, catalog).Succeeded(), "恢复原文后应可转回。");
}

void SnapshotRestoresDisabledLines(const UiCatalog& catalog) {
    auto f = GuiFixture(catalog, true);
    const auto snapshot = Take(TriggerCodeEditor::CaptureGuiFragment(f.document, f.id));
    const auto original = Shape(snapshot);
    const auto text = Convert(f, catalog);
    auto exact = Take(Recover(f, catalog, &snapshot));
    Require(exact.fromSnapshot && Shape(exact.fragment) == original, "未改正文须按快照原样恢复（含禁用行）。");
    auto parsed = Take(Recover(f, catalog));
    Require(!parsed.fromSnapshot && Shape(parsed.fragment) != original, "没有快照时禁用行无法从代码得知。");
    // 正文改动后快照不再适用，改走解析。
    SetText(f, catalog, Replace(text, "< 5)", "< 6)"));
    auto edited = Take(Recover(f, catalog, &snapshot));
    Require(!edited.fromSnapshot && Shape(edited.fragment).find("{3:6}") != std::string::npos, "正文已改时不能沿用快照。");
}

Fixture FragmentFixture(const UiCatalog& catalog, const GuiTriggerFragment& fragment) {
    auto fixture = GuiFixture(catalog, false);
    Convert(fixture, catalog);
    auto plan = Take(TriggerCodeEditor::PrepareRecovery(fixture.document, catalog, fixture.id, fragment));
    Take(TriggerCodeEditor::Apply(fixture.document, catalog, plan));
    return fixture;
}

void VerifyParsed(const UiCatalog& catalog, const GuiTriggerFragment& fragment, bool partial = false) {
    auto f = FragmentFixture(catalog, fragment);
    const auto original = Wire(f.document, catalog);
    Script::MapScriptInputs inputs; inputs.triggers = &f.document; inputs.catalog = &catalog;
    const auto generated = Take(Script::GenerateGuiTriggerSource(inputs, f.id));
    auto conversion = Take(TriggerCodeEditor::PrepareConversion(f.document, catalog, f.id, generated.text, generated.effectiveRunOnInit));
    auto converted = Take(TriggerCodeEditor::Apply(f.document, catalog, conversion));
    auto recovered = Take(Recover(f, catalog));
    Require(!recovered.fromSnapshot, "原创 YDWE 样例必须只凭解析恢复。");
    Require((recovered.preservedCodeLines + recovered.preservedCodeExpressions != 0) == partial, "完整/保留代码的覆盖必须分别计数。");
    auto plan = Take(TriggerCodeEditor::PrepareRecovery(f.document, catalog, f.id, recovered.fragment));
    auto receipt = Take(TriggerCodeEditor::Apply(f.document, catalog, plan));
    Check(WtgWctCodec{}.Validate(f.document, catalog));
    // 辅助函数编号允许整体改名，Verify 内始终进行逐词门禁。
    Check(TriggerCodeEditor::Replay(f.document, catalog, receipt, false));
    Check(TriggerCodeEditor::Replay(f.document, catalog, converted, false));
    const auto restored = Wire(f.document, catalog);
    Require(original.wtg == restored.wtg && original.wct == restored.wct, "YDWE 转码/转回全部撤销须整图逐字节恢复。");
}

void YdweStructuredShapes(const UiCatalog& catalog) {
    {
        Builder b;
        const auto disabled = b.Eca(0, "YDWEDisableRegister", {});
        const auto action = b.Eca(2, "DoNothing", {});
        b.fragment.roots = {disabled, action}; VerifyParsed(catalog, b.fragment);
    }
    {
        Builder b;
        const auto callback = b.Eca(2, "DoNothing", {});
        const auto callbackAction = b.Eca(2, "RunCallback", {b.Arg(2, "DoNothing", callback)});
        const auto less = b.Eca(1, "OperatorCompareInteger", {b.Arg(1, "Count"), b.Arg(0, "OperatorLess"), b.Arg(3, "8")});
        const auto predicate = b.Eca(2, "UsePredicate", {b.Arg(2, "OperatorCompareInteger", less)});
        b.fragment.roots = {callbackAction, predicate}; VerifyParsed(catalog, b.fragment);
        auto f = FragmentFixture(catalog, b.fragment);
        const auto text = Convert(f, catalog);
        SetText(f, catalog, Replace(text, "takes nothing returns boolean", "takes integer value returns boolean"));
        const auto before = Wire(f.document, catalog);
        Require(!Recover(f, catalog).Succeeded(), "带参数的相似条件函数不能套用无参数回调形状。");
        const auto after = Wire(f.document, catalog);
        Require(before.wtg == after.wtg && before.wct == after.wct, "回调拒绝必须保持正文。");
    }
    {
        Builder b;
        const auto callback = b.Eca(2, "DoNothing", {});
        const auto callbackAction = b.Eca(2, "RunCallbackNumber", {b.Arg(2, "DoNothing", callback), b.Arg(1, "Count")});
        const auto less = b.Eca(1, "OperatorCompareInteger", {b.Arg(1, "Count"), b.Arg(0, "OperatorLess"), b.Arg(3, "8")});
        const auto predicate = b.Eca(2, "UseBooleanCall", {b.Arg(2, "OperatorCompareInteger", less)});
        const auto event = b.Eca(0, "TriggerRegisterTimerEventPeriodic", {b.Arg(3, "1.00")});
        const auto handle = b.Eca(3, "CreateTrigger", {});
        const auto addEvent = b.Eca(2, "AddTriggerEvent", {b.Arg(2, "CreateTrigger", handle), b.Arg(2, "TriggerRegisterTimerEventPeriodic", event)});
        b.fragment.roots = {callbackAction, predicate, addEvent}; VerifyParsed(catalog, b.fragment);
        Builder wrong;
        const auto wrongEvent = wrong.Eca(0, "TriggerRegisterTimerEventPeriodic", {wrong.Arg(3, "1.00")});
        wrong.fragment.roots = {wrong.Eca(2, "RunCallback", {wrong.Arg(2, "TriggerRegisterTimerEventPeriodic", wrongEvent)})};
        auto f = GuiFixture(catalog, false); Convert(f, catalog);
        Require(!TriggerCodeEditor::PrepareRecovery(f.document, catalog, f.id, wrong.fragment).Succeeded(),
            "事件参数不能塞进 code 动作参数槽，新增函数槽支持不能放宽类型门。");
    }
    {
        Builder b;
        const auto index = b.Eca(3, "YDWEForLoopLocVarIndex", {b.Arg(3, "counter")});
        const auto set = b.Eca(2, "SetVariable", {b.Arg(1, "Count"), b.Arg(2, "YDWEForLoopLocVarIndex", index)}, 0);
        const auto loop = b.Eca(2, "YDWEForLoopLocVarMultiple", {b.Arg(3, "counter"), b.Arg(3, "1"), b.Arg(3, "5")}, {}, {set});
        b.fragment.roots = {loop}; VerifyParsed(catalog, b.fragment);
        auto f = FragmentFixture(catalog, b.fragment);
        const auto text = Convert(f, catalog);
        SetText(f, catalog, Replace(text, "ydul_counter + 1", "ydul_counter + 2"));
        Require(!Recover(f, catalog).Succeeded(), "局部循环的递增改变后不能伪造标准循环。");
    }
    {
        Builder b;
        const auto timer = b.Eca(3, "CreateTimer", {});
        const auto action = b.Eca(2, "DoNothing", {}, 1);
        const auto flush = b.Eca(2, "YDWETimerStartFlush", {}, 1);
        const auto closure = b.Eca(2, "YDWETimerStartMultiple", {b.Arg(2, "CreateTimer", timer), b.Arg(3, "0.25"), b.Arg(0, "Once")}, {}, {action, flush});
        b.fragment.roots = {closure}; VerifyParsed(catalog, b.fragment);
        auto f = FragmentFixture(catalog, b.fragment);
        const auto text = Convert(f, catalog);
        SetText(f, catalog, Replace(text, "call DestroyTimer(GetExpiredTimer())", "call DestroyTrigger(GetExpiredTimer())"));
        Require(!Recover(f, catalog).Succeeded(), "闭包清理句柄类型变化必须拒绝。");
    }
    {
        Builder b;
        const auto trigger = b.Eca(3, "CreateTrigger", {});
        const auto event = b.Eca(0, "TriggerRegisterTimerEventPeriodic", {b.Arg(3, "1.00")}, 0);
        const auto action = b.Eca(2, "DoNothing", {}, 2);
        const auto closure = b.Eca(2, "YDWERegisterTriggerMultiple", {b.Arg(2, "CreateTrigger", trigger)}, {}, {event, action});
        b.fragment.roots = {closure}; VerifyParsed(catalog, b.fragment);
    }
    {
        Builder b;
        const auto unit = b.Eca(3, "GetEnumUnit", {});
        const auto action = b.Eca(2, "KillUnit", {b.Arg(2, "GetEnumUnit", unit)}, 0);
        const auto enumeration = b.Eca(2, "YDWEEnumUnitsInRangeMultiple", {b.Arg(3, "0"), b.Arg(3, "0"), b.Arg(3, "100")}, {}, {action});
        b.fragment.roots = {enumeration}; VerifyParsed(catalog, b.fragment);
    }
    for (const auto mode : {"true", "false"}) {
        Builder b;
        const auto trigger = b.Eca(3, "CreateTrigger", {});
        const auto closure = b.Eca(2, "YDWEExecuteTriggerMultiple", {b.Arg(2, "CreateTrigger", trigger), b.Arg(3, mode)});
        b.fragment.roots = {closure}; VerifyParsed(catalog, b.fragment);
        auto f = FragmentFixture(catalog, b.fragment);
        const auto text = Convert(f, catalog);
        SetText(f, catalog, Replace(text, " + 3)", " + 4)"));
        Require(!Recover(f, catalog).Succeeded(), "运行触发器页代际不符必须拒绝。");
    }
}

void YdweStorageIsHonestPartialRecovery(const UiCatalog& catalog) {
    Builder b;
    const auto save = b.Eca(2, "YDWESetAnyTypeLocalVariable", {b.Arg(0, "typename01_integer"), b.Arg(3, "score"), b.Arg(3, "7")});
    const auto read = b.Eca(3, "YDWEGetAnyTypeLocalVariable", {b.Arg(3, "score")});
    const auto set = b.Eca(2, "SetVariable", {b.Arg(1, "Count"), b.Arg(2, "YDWEGetAnyTypeLocalVariable", read)});
    b.fragment.roots = {save, set};
    VerifyParsed(catalog, b.fragment, true);
    auto f = FragmentFixture(catalog, b.fragment);
    const auto text = Convert(f, catalog);
    SetText(f, catalog, Replace(text, "ydl_localvar_step + 3", "ydl_localvar_step + 4"));
    const auto before = Wire(f.document, catalog);
    const auto rejected = Recover(f, catalog);
    Require(!rejected.Succeeded() && !rejected.diagnostics.empty() && rejected.diagnostics.front().message.find("第 ") != std::string::npos,
        "相似但改动页分配的代码应定位拒绝，不得宽松吞掉。");
    const auto after = Wire(f.document, catalog);
    Require(before.wtg == after.wtg && before.wct == after.wct, "拒绝不能改变代码。");
    {
        Builder array;
        const auto saveArray = array.Eca(2, "YDWESetAnyTypeLocalArray", {array.Arg(0, "typename01_integer"), array.Arg(3, "values"), array.Arg(1, "Count"), array.Arg(3, "9")});
        const auto getArray = array.Eca(3, "YDWEGetAnyTypeLocalArray", {array.Arg(3, "values"), array.Arg(1, "Count")});
        const auto assign = array.Eca(2, "SetVariable", {array.Arg(1, "Count"), array.Arg(2, "YDWEGetAnyTypeLocalArray", getArray)});
        array.fragment.roots = {saveArray, assign}; VerifyParsed(catalog, array.fragment, true);
    }
    {
        Builder captured;
        const auto saveLocal = captured.Eca(2, "YDWESetAnyTypeLocalVariable", {captured.Arg(0, "typename01_integer"), captured.Arg(3, "payload"), captured.Arg(3, "7")});
        const auto readLocal = captured.Eca(3, "YDWEGetAnyTypeLocalVariable", {captured.Arg(3, "payload")});
        const auto callback = captured.Eca(2, "SetVariable", {captured.Arg(1, "Count"), captured.Arg(2, "YDWEGetAnyTypeLocalVariable", readLocal)}, 1);
        const auto timer = captured.Eca(3, "CreateTimer", {});
        const auto closure = captured.Eca(2, "YDWETimerStartMultiple", {captured.Arg(2, "CreateTimer", timer), captured.Arg(3, "0.25"), captured.Arg(0, "Once")}, {}, {callback});
        captured.fragment.roots = {saveLocal, closure}; VerifyParsed(catalog, captured.fragment, true);
    }
    {
        Builder table;
        const auto saveData = table.Eca(2, "YDWESaveAnyTypeDataByUserData", {table.Arg(0, "typename01_integer"), table.Arg(1, "Count"), table.Arg(3, "score"), table.Arg(0, "typename01_integer"), table.Arg(3, "8")});
        const auto flushData = table.Eca(2, "YDWEFlushAllByUserData", {table.Arg(0, "typename01_integer"), table.Arg(1, "Count")});
        table.fragment.roots = {saveData, flushData}; VerifyParsed(catalog, table.fragment, true);
        auto tableFixture = FragmentFixture(catalog, table.fragment);
        const auto tableCode = Convert(tableFixture, catalog);
        SetText(tableFixture, catalog, Replace(tableCode, "YDHT", "UNKNOWN_TABLE"));
        Require(!Recover(tableFixture, catalog).Succeeded(), "未知哈希表不得套用框架存储表达式。");
    }
}

void ProjectSnapshotsBindSourceAndDocument(const UiCatalog& catalog) {
    auto f = GuiFixture(catalog, true);
    Script::MapScriptInputs inputs; inputs.triggers = &f.document; inputs.catalog = &catalog;
    const auto source = Take(Script::GenerateGuiTriggerSource(inputs, f.id));
    const auto original = Shape(Take(TriggerCodeEditor::CaptureGuiFragment(f.document, f.id)));
    GuiConversionSnapshots snapshots;
    {
        auto cancelled = Take(GuiConversionSnapshots::Prepare(f.document, catalog, f.id, source.text));
        Require(snapshots.Empty() && snapshots.Revision() == 0, "只准备但取消的转换不能发布快照。");
    }
    Check(snapshots.Remember(f.document, catalog, f.id, source.text));
    auto plan = Take(TriggerCodeEditor::PrepareConversion(f.document, catalog, f.id, source.text, source.effectiveRunOnInit));
    auto converted = Take(TriggerCodeEditor::Apply(f.document, catalog, plan));
    Require(snapshots.Find(f.document, catalog, f.id), "转换成功后当前快照应匹配。");
    const auto transport = Take(snapshots.Serialize(f.document, catalog, "directory:original-synthetic-source"));
    const auto wire = Wire(f.document, catalog);
    auto reopened = Take(WtgWctCodec{}.Parse(wire.wtg, wire.wct, catalog));
    auto restored = Take(GuiConversionSnapshots::Parse(transport, reopened, catalog, "directory:original-synthetic-source"));
    const auto newId = reopened.triggers.front().id;
    const auto* fragment = restored.Find(reopened, catalog, newId);
    Require(fragment && Shape(*fragment) == original, "工程重开后应保持禁用行、分支及参数。");
    Script::MapScriptInputs reopenedInputs; reopenedInputs.triggers = &reopened; reopenedInputs.catalog = &catalog;
    Require(Take(Script::RecoverGuiTrigger(reopenedInputs, newId, fragment)).fromSnapshot, "恢复的快照仍须通过生产生成门禁。");
    reopened.triggers.front().runOnInit = 1;
    const auto changedInit = Take(Script::RecoverGuiTrigger(reopenedInputs, newId, fragment));
    Require(!changedInit.fromSnapshot && changedInit.fragment.runOnInit,
        "初始化语义变化后须丢弃旧快照，解析恢复仍保留当前初始化标志。");
    reopened.triggers.front().runOnInit = 0;
    Require(!GuiConversionSnapshots::Parse(transport, reopened, catalog, "directory:another-source").Succeeded(), "不同工程来源不得套用相同脚本快照。");
    auto wrongVersion = transport; wrongVersion[4] = std::byte{2};
    Require(!GuiConversionSnapshots::Parse(wrongVersion, reopened, catalog, "directory:original-synthetic-source").Succeeded(), "未知快照版本必须拒绝。");
    auto truncated = transport; truncated.pop_back();
    Require(!GuiConversionSnapshots::Parse(truncated, reopened, catalog, "directory:original-synthetic-source").Succeeded(), "截断快照必须拒绝。");
    auto appended = transport; appended.push_back(std::byte{0});
    Require(!GuiConversionSnapshots::Parse(appended, reopened, catalog, "directory:original-synthetic-source").Succeeded(), "未知尾部必须拒绝。");
    auto corrupt = transport; corrupt[corrupt.size() - 1] ^= std::byte{1};
    Require(!GuiConversionSnapshots::Parse(corrupt, reopened, catalog, "directory:original-synthetic-source").Succeeded(), "损坏摘要必须拒绝。");
    SetText(f, catalog, Replace(source.text, "< 5)", "< 6)"));
    Require(!snapshots.Find(f.document, catalog, f.id), "正文改动后不能用旧快照。");
    Require(!GuiConversionSnapshots::Parse(transport, f.document, catalog, "directory:original-synthetic-source").Succeeded(), "工程边车与外部改动后的正文不匹配。");
    SetText(f, catalog, source.text);
    Check(TriggerCodeEditor::Replay(f.document, catalog, converted, false));
    Require(!snapshots.Find(f.document, catalog, f.id), "GUI 状态不得被作为代码快照命中。");
    Check(TriggerCodeEditor::Replay(f.document, catalog, converted, true));
    Require(snapshots.Find(f.document, catalog, f.id), "撤销后重做转换仍可命中同一个快照。");
    const auto second = Take(TriggerCodeEditor::PrepareNewCustom(f.document, catalog, f.document.categories.front().categoryId, "Second"));
    Take(TriggerCodeEditor::Apply(f.document, catalog, second));
    std::swap(f.document.triggers[0], f.document.triggers[1]);
    Require(!GuiConversionSnapshots::Parse(transport, f.document, catalog, "directory:original-synthetic-source").Succeeded(), "重排触发器不能按旧槽位套快照。");
    restored.Clear(); Require(!restored.Find(reopened, catalog, newId), "工程切换必须清空旧快照。");
}

void SnapshotVersionsFollowConversionUndo(const UiCatalog& catalog) {
    auto f = GuiFixture(catalog, true);
    const auto original = Wire(f.document, catalog);
    const auto originalShape = Shape(Take(TriggerCodeEditor::CaptureGuiFragment(f.document, f.id)));
    GuiConversionSnapshots snapshots;
    const auto emptyFingerprint = snapshots.ActiveFingerprint(f.document, catalog);
    Script::MapScriptInputs inputs; inputs.triggers = &f.document; inputs.catalog = &catalog;
    const auto firstSource = Take(Script::GenerateGuiTriggerSource(inputs, f.id));
    auto firstSnapshot = Take(GuiConversionSnapshots::Prepare(f.document, catalog, f.id, firstSource.text));
    const auto firstPlan = Take(TriggerCodeEditor::PrepareConversion(f.document, catalog, f.id, firstSource.text, firstSource.effectiveRunOnInit));
    const auto firstConversion = Take(TriggerCodeEditor::Apply(f.document, catalog, firstPlan));
    snapshots.Swap(firstSnapshot);
    const auto firstFingerprint = snapshots.ActiveFingerprint(f.document, catalog);
    Require(!firstFingerprint.empty(), "发布后的有效快照应参与工程指纹。");
    const auto withoutDisabled = Sample(false);
    const auto recoveryPlan = Take(TriggerCodeEditor::PrepareRecovery(f.document, catalog, f.id, withoutDisabled));
    const auto recovery = Take(TriggerCodeEditor::Apply(f.document, catalog, recoveryPlan));
    Require(snapshots.ActiveFingerprint(f.document, catalog) == emptyFingerprint, "GUI 态遗留缓存不应使工程变脏。");
    const auto secondSource = Take(Script::GenerateGuiTriggerSource(inputs, f.id));
    Require(secondSource.text == firstSource.text, "反例必须是禁用节点不同、生成代码相同。");
    auto secondSnapshot = Take(GuiConversionSnapshots::Prepare(f.document, catalog, f.id, secondSource.text));
    const auto secondPlan = Take(TriggerCodeEditor::PrepareConversion(f.document, catalog, f.id, secondSource.text, secondSource.effectiveRunOnInit));
    const auto secondConversion = Take(TriggerCodeEditor::Apply(f.document, catalog, secondPlan));
    snapshots.Swap(secondSnapshot);
    const auto secondFingerprint = snapshots.ActiveFingerprint(f.document, catalog);
    Require(secondFingerprint != firstFingerprint, "相同代码对应不同原 GUI 时必须区分快照版本。");
    const auto* secondFragment = snapshots.Find(f.document, catalog, f.id);
    Require(secondFragment && Shape(*secondFragment) == Shape(withoutDisabled), "第二次转换保存第二份 GUI。");
    Check(TriggerCodeEditor::Replay(f.document, catalog, secondConversion, false)); snapshots.Swap(secondSnapshot);
    Check(TriggerCodeEditor::Replay(f.document, catalog, recovery, false));
    Require(snapshots.ActiveFingerprint(f.document, catalog) == firstFingerprint, "撤销须回到原有有效快照版本。");
    const auto* previousFragment = snapshots.Find(f.document, catalog, f.id);
    Require(previousFragment && Shape(*previousFragment) == originalShape, "撤销不能把较新的禁用节点布局套给旧代码。");
    Check(TriggerCodeEditor::Replay(f.document, catalog, recovery, true));
    Check(TriggerCodeEditor::Replay(f.document, catalog, secondConversion, true)); snapshots.Swap(secondSnapshot);
    Require(snapshots.ActiveFingerprint(f.document, catalog) == secondFingerprint, "重做应复用同一快照版本。");
    const auto* redoneFragment = snapshots.Find(f.document, catalog, f.id);
    Require(redoneFragment && Shape(*redoneFragment) == Shape(withoutDisabled), "重做应精确回到第二份 GUI 快照。");
    Check(TriggerCodeEditor::Replay(f.document, catalog, secondConversion, false)); snapshots.Swap(secondSnapshot);
    Check(TriggerCodeEditor::Replay(f.document, catalog, recovery, false));
    Check(TriggerCodeEditor::Replay(f.document, catalog, firstConversion, false)); snapshots.Swap(firstSnapshot);
    Require(snapshots.Empty() && snapshots.ActiveFingerprint(f.document, catalog) == emptyFingerprint,
        "全部撤销后不存在新快照或虚假工程脏标记。");
    const auto undone = Wire(f.document, catalog);
    Require(undone.wtg == original.wtg && undone.wct == original.wct, "包含快照交换的全部撤销仍须逐字节一致。");
}

void MultipleSnapshotsKeepIndependentNodes(const UiCatalog& catalog) {
    auto f = GuiFixture(catalog, true);
    GuiConversionSnapshots snapshots;
    std::string firstFingerprint;
    for (int index = 0; index < 2; ++index) {
        if (index) {
            auto created = Take(TriggerCodeEditor::PrepareNewCustom(f.document, catalog, f.document.categories.front().categoryId, "SecondSnapshot"));
            Take(TriggerCodeEditor::Apply(f.document, catalog, created)); f.id = created.Target().triggerId;
            auto gui = Take(TriggerCodeEditor::PrepareRecovery(f.document, catalog, f.id, Sample(true)));
            Take(TriggerCodeEditor::Apply(f.document, catalog, gui));
        }
        Script::MapScriptInputs inputs; inputs.triggers = &f.document; inputs.catalog = &catalog;
        const auto source = Take(Script::GenerateGuiTriggerSource(inputs, f.id));
        auto staged = Take(GuiConversionSnapshots::Prepare(f.document, catalog, f.id, source.text));
        const auto plan = Take(TriggerCodeEditor::PrepareConversion(f.document, catalog, f.id, source.text, source.effectiveRunOnInit));
        const auto converted = Take(TriggerCodeEditor::Apply(f.document, catalog, plan));
        snapshots.Swap(staged);
        if (!index) firstFingerprint = snapshots.ActiveFingerprint(f.document, catalog);
        else {
            Check(TriggerCodeEditor::Replay(f.document, catalog, converted, false)); snapshots.Swap(staged);
            Require(snapshots.ActiveFingerprint(f.document, catalog) == firstFingerprint,
                "只撤销第二个触发器时，第一份快照的工程指纹须保持不变。");
            Check(TriggerCodeEditor::Replay(f.document, catalog, converted, true)); snapshots.Swap(staged);
        }
    }
    const auto transport = Take(snapshots.Serialize(f.document, catalog, "archive:synthetic-source"));
    const auto wire = Wire(f.document, catalog);
    auto reopened = Take(WtgWctCodec{}.Parse(wire.wtg, wire.wct, catalog));
    auto restored = Take(GuiConversionSnapshots::Parse(transport, reopened, catalog, "archive:synthetic-source"));
    Script::MapScriptInputs inputs; inputs.triggers = &reopened; inputs.catalog = &catalog;
    for (const auto& record : reopened.triggers) {
        const auto* fragment = restored.Find(reopened, catalog, record.id);
        Require(fragment && Take(Script::RecoverGuiTrigger(inputs, record.id, fragment)).fromSnapshot,
            "连续转换的新旧快照节点身份不得互相冲突，重开后均应命中。");
    }
}

void WriteYdweShapes(const std::filesystem::path& root) {
    // 原创的最小函数库，只描述本测试所需接口，不复制用户本机定义或代码。
    Write(root / "ui/shapes/define.txt",
        "[TriggerTypes]\nboolean=0,1,1,Boolean,boolean\ntimer=0,1,1,Timer,timer\ntrigger=0,1,1,Trigger,trigger\n"
        "unit=0,1,1,Unit,unit\nhashtable=0,1,1,Hashtable,hashtable\nperiodicoption=0,0,0,Periodic,boolean\n"
        "typename=0,0,0,Type\nNull=0,0,0,Value\nAnyReturnType=0,0,0,Any\n"
        "[TriggerParams]\nOnce=0,periodicoption,false,Once\ntypename01_integer=0,typename,integer,Integer\n");
    Write(root / "ui/shapes/event.txt", "[YDWEDisableRegister]\ntitle=\"Deferred\"\n[[.args]]\ntype=nothing\n");
    Write(root / "ui/shapes/action.txt",
        "[RunCallback]\ntitle=\"Run\"\n[[.args]]\ntype=code\n"
        "[RunCallbackWrong]\ntitle=\"Wrong overload\"\nscript_name=RunCallbackNumber\n[[.args]]\ntype=code\n[[.args]]\ntype=string\n"
        "[RunCallbackNumber]\ntitle=\"Typed callback\"\n[[.args]]\ntype=code\n[[.args]]\ntype=integer\n"
        "[UsePredicate]\ntitle=\"Predicate\"\n[[.args]]\ntype=boolexpr\n"
        "[UseBooleanCall]\ntitle=\"Boolean callback\"\n[[.args]]\ntype=boolcall\n"
        "[AddTriggerEvent]\ntitle=\"Add event\"\n[[.args]]\ntype=trigger\n[[.args]]\ntype=eventcall\n"
        "[CustomScriptCode]\ntitle=\"Code\"\n[[.args]]\ntype=scriptcode\n"
        "[YDWEForLoopLocVarMultiple]\ntitle=\"Local loop\"\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[YDWETimerStartMultiple]\ntitle=\"Timer\"\n[[.args]]\ntype=timer\n[[.args]]\ntype=real\n[[.args]]\ntype=periodicoption\n"
        "[YDWETimerStartFlush]\ntitle=\"Clear timer\"\n[[.args]]\ntype=nothing\n"
        "[YDWERegisterTriggerMultiple]\ntitle=\"Register\"\n[[.args]]\ntype=trigger\n"
        "[YDWEExecuteTriggerMultiple]\ntitle=\"Execute\"\n[[.args]]\ntype=trigger\n[[.args]]\ntype=boolean\n"
        "[YDWEEnumUnitsInRangeMultiple]\ntitle=\"Range\"\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n"
        "[KillUnit]\ntitle=\"Kill\"\n[[.args]]\ntype=unit\n"
        "[YDWESetAnyTypeLocalVariable]\ntitle=\"Local set\"\n[[.args]]\ntype=typename\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=Null\n"
        "[YDWESetAnyTypeLocalArray]\ntitle=\"Local array set\"\n[[.args]]\ntype=typename\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=integer\n[[.args]]\ntype=Null\n"
        "[YDWESaveAnyTypeDataByUserData]\ntitle=\"User data set\"\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n"
        "[YDWEFlushAllByUserData]\ntitle=\"User data flush\"\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n"
        "[SaveInteger]\ntitle=\"Save integer\"\n[[.args]]\ntype=hashtable\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[FlushChildHashtable]\ntitle=\"Flush page\"\n[[.args]]\ntype=hashtable\n[[.args]]\ntype=integer\n");
    Write(root / "ui/shapes/call.txt",
        "[YDWEForLoopLocVarIndex]\ntitle=\"Loop index\"\nreturns=integer\n[[.args]]\ntype=scriptcode\n"
        "[YDWECustomScriptCode]\ntitle=\"Code expression\"\nreturns=AnyReturnType\n[[.args]]\ntype=scriptcode\n"
        "[YDWEGetAnyTypeLocalVariable]\ntitle=\"Local get\"\nreturns=AnyReturnType\n[[.args]]\ntype=scriptcode\n"
        "[YDWEGetAnyTypeLocalArray]\ntitle=\"Local array get\"\nreturns=AnyReturnType\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=integer\n"
        "[CreateTimer]\ntitle=\"Timer\"\nreturns=timer\n[[.args]]\ntype=nothing\n"
        "[CreateTrigger]\ntitle=\"Trigger\"\nreturns=trigger\n"
        "[GetEnumUnit]\ntitle=\"Enumerated unit\"\nreturns=unit\n"
        "[LoadInteger]\ntitle=\"Load integer\"\nreturns=integer\n[[.args]]\ntype=hashtable\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n");
}
} // namespace

int main() {
    TriggerCodeEditor::SetFullReplayVerification(true);
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-CodeRecovery-" + std::to_string(GetCurrentProcessId()));
    try {
        Require(!std::filesystem::exists(root), "原创目录已存在，拒绝覆盖。");
        Write(root / "ui/config", "base\nshapes\n");
        WriteYdweShapes(root);
        Write(root / "ui/base/define.txt",
            "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\nstring=0,1,1,String,string\n"
            "player=0,1,1,Player,player\nComparisonOperator=0,0,0,Comparison\nArithmeticOperator=0,0,0,Arithmetic\n"
            "[TriggerParams]\nOperatorEqual=0,ComparisonOperator,==,Equal\nOperatorLess=0,ComparisonOperator,<,Less\n"
            "OperatorAdd=0,ArithmeticOperator,+,Add\nOperatorSubtract=0,ArithmeticOperator,-,Subtract\n");
        Write(root / "ui/base/event.txt", "[MapInitializationEvent]\ntitle=\"Map init\"\n"
            "[TriggerRegisterTimerEventPeriodic]\ntitle=\"Periodic\"\n[[.args]]\ntype=real\n");
        Write(root / "ui/base/condition.txt", "[OperatorCompareInteger]\ntitle=\"Compare\"\n"
            "[[.args]]\ntype=integer\n[[.args]]\ntype=ComparisonOperator\n[[.args]]\ntype=integer\n");
        Write(root / "ui/base/action.txt", "[DoNothing]\ntitle=\"Nothing\"\n"
            "[SetVariable]\ntitle=\"Set\"\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
            "[CommentString]\ntitle=\"Comment\"\n[[.args]]\ntype=scriptcode\n"
            "[DisplayTextToPlayer]\ntitle=\"Text\"\n[[.args]]\ntype=player\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n[[.args]]\ntype=string\n"
            "[IfThenElseMultiple]\ntitle=\"If\"\n"
            "[ForLoopAMultiple]\ntitle=\"Loop A\"\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n");
        Write(root / "ui/base/call.txt", "[Identity]\ntitle=\"Identity\"\nreturns=integer\n[[.args]]\ntype=integer\n"
            "[OperatorInt]\ntitle=\"Arithmetic\"\nreturns=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=ArithmeticOperator\n[[.args]]\ntype=integer\n"
            "[GetTriggerPlayer]\ntitle=\"Triggering player\"\nreturns=player\n");
        auto catalog = Take(UiCatalog::Load(root));
        ParsedRoundTrip(catalog);
        EditedValues(catalog);
        Rejections(catalog);
        SnapshotRestoresDisabledLines(catalog);
        YdweStructuredShapes(catalog);
        YdweStorageIsHonestPartialRecovery(catalog);
        ProjectSnapshotsBindSourceAndDocument(catalog);
        SnapshotVersionsFollowConversionUndo(catalog);
        MultipleSnapshotsKeepIndependentNodes(catalog);
        std::filesystem::remove_all(root);
        std::cout << "code to T recovery: parse, edit, snapshot, rejection and undo passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
        std::cerr << error.what() << '\n'; return 1;
    }
}
