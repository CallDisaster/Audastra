#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

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
} // namespace

int main() {
    TriggerCodeEditor::SetFullReplayVerification(true);
    const auto root = std::filesystem::temp_directory_path() / ("Audastra-CodeRecovery-" + std::to_string(GetCurrentProcessId()));
    try {
        Require(!std::filesystem::exists(root), "原创目录已存在，拒绝覆盖。");
        Write(root / "ui/config", "base\n");
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
        std::filesystem::remove_all(root);
        std::cout << "code to T recovery: parse, edit, snapshot, rejection and undo passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
        std::cerr << error.what() << '\n'; return 1;
    }
}
