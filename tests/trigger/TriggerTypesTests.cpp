#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

// 参数类型系统与“把函数参数换成其他来源”的回归。夹具为原创合成的 UI 定义与 WTG：
// 按 YDWE 的公开写法定义 typename / objecttype 选择器、Null 参数、AnyReturnType 读取函数与原版 SetVariable。
namespace {

using namespace War3Map::Trigger;
using Editor = TriggerDocumentEditor;
using War3Map::StableId;

void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error("trigger types: " + std::string(message));
}

class Writer {
public:
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 0xffU));
        }
    }
    void Text(std::string_view value) {
        for (const char c : value) bytes.push_back(static_cast<std::byte>(c));
        bytes.push_back(std::byte{0});
    }
    // 标量参数：类型、值、非函数、无下标。
    void Scalar(std::int32_t type, std::string_view value) {
        I32(type);
        Text(value);
        I32(0);
        I32(0);
    }
    std::vector<std::byte> bytes;
};

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::filesystem::path CreateUi() {
    const auto root = std::filesystem::temp_directory_path() /
        ("War3MapEditor64-TriggerTypes-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    const auto ui = root / "ui";
    WriteText(ui / "config", "base\r\n");
    WriteText(ui / "base" / "define.txt",
        "[TriggerTypes]\r\n"
        "real=0,1,1,实数\r\nunit=0,1,1,单位\r\nstring=0,1,1,字符串\r\n"
        "integer=0,1,1,整数\r\nhandle=0,0,0,句柄\r\nhashtable=0,1,0,哈希表\r\ntrigger=0,1,1,触发器\r\n"
        "tablealias=0,1,0,表别名,hashtable\r\ncyclea=0,0,0,A,cycleb\r\ncycleb=0,0,0,B,cyclea\r\n"
        "degree=0,1,1,角度,real\r\nunitcode=0,1,1,单位类型,integer\r\n"
        "itemcode=0,1,1,物品类型,integer\r\nabilcode=0,1,1,技能类型,integer\r\n"
        "buffcode=0,1,1,效果类型,integer\r\ntechcode=0,1,1,科技类型,integer\r\n"
        "doodadcode=0,1,1,装饰物类型,integer\r\ndestructablecode=0,1,1,可破坏物类型,integer\r\n"
        "heroskillcode=0,1,1,英雄技能,integer\r\nabilityalias=0,1,1,技能别名,abilcode\r\n"
        "abilityalias2=0,1,1,二级别名,abilityalias\r\ncustomcode=0,1,1,普通整数,integer\r\n"
        "scriptcode=1,0,0,代码,string\r\n"
        "typename=1,0,0,参数类型,string\r\nobjecttype=1,0,0,物体类型,string\r\n"
        "StringExt=0,0,0,长文本,string\r\nmodelfile=0,0,0,模型文件,string,Model,1\r\nplainalias=0,0,0,无标记别名,string\r\n"
        "[TriggerParams]\r\n"
        "typename01_real=0,typename,1,实数\r\ntypename07_unit=0,typename,7,单位\r\n"
        "objecttype02_unitcode=0,objecttype,2,单位\r\nPresetHalf=0,real,0.5,一半\r\n");
    WriteText(ui / "base" / "action.txt",
        "[SetVariable]\r\ndescription=\"设置 ${变量} = ${值}\"\r\n[[.args]]\r\ntype=AnyGlobal\r\n[[.args]]\r\ntype=Null\r\n"
        "[YDWESetAnyTypeLocalVariable]\r\ndescription=\"[${类型}] loc_${名} = ${值}\"\r\n"
        "[[.args]]\r\ntype=typename\r\n[[.args]]\r\ntype=scriptcode\r\n[[.args]]\r\ntype=Null\r\n"
        "[UseReal]\r\ndescription=\"用 ${实数}\"\r\n[[.args]]\r\ntype=real\r\n"
        "[UseInteger]\r\ndescription=\"用 ${整数}\"\r\n[[.args]]\r\ntype=integer\r\n");
    WriteText(ui / "base" / "call.txt",
        "[YDWEGetAnyTypeLocalVariable]\r\ndescription=\"loc_${名}\"\r\nreturns=AnyReturnType\r\n[[.args]]\r\ntype=scriptcode\r\n"
        "[GetDegree]\r\nreturns=degree\r\n"
        "[GetRealProperty]\r\ndescription=\"[${类型}]${物体} 的属性\"\r\nreturns=real\r\n"
        "[[.args]]\r\ntype=objecttype\r\n[[.args]]\r\ntype=Null\r\n"
        "[GetUnitName]\r\nreturns=string\r\n"
        "[GetHandleId]\r\nreturns=integer\r\ndescription=\"${句柄} ID\"\r\n[[.args]]\r\ntype=handle\r\n"
        "[CreateHashtable]\r\nreturns=hashtable\r\n");
    return root;
}

// 触发器 T：
//   0 SetVariable(Hero, loc_u)                 Null 跟随变量 Hero → unit
//   1 [单位] loc_u = Hero                      Null 跟随 typename07_unit → unit
//   2 [实数] loc_v = 0.5                        Null → real
//   3 UseReal(GetDegree())                     degree 可用于 real
//   4 UseReal(GetRealProperty(unitcode, hfoo)) Null 跟随 objecttype02_unitcode → unitcode
std::vector<std::byte> Wtg() {
    Writer w;
    for (const char c : std::string_view("WTG!")) w.bytes.push_back(static_cast<std::byte>(c));
    w.I32(7);
    w.I32(1);
    w.I32(1); w.Text("Main"); w.I32(0);
    w.I32(2);
    w.I32(1);
    w.Text("Hero"); w.Text("unit"); w.I32(1); w.I32(0); w.I32(1); w.I32(0); w.Text("");
    w.I32(1);
    w.Text("T"); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1);
    w.I32(5);
    const auto call = [&w](std::string_view name, const auto& arguments) {
        w.I32(2); w.Text(""); w.I32(1);
        w.I32(3); w.Text(name); w.I32(1);
        arguments();
        w.I32(0); // 嵌套函数没有子块
        w.I32(0); // 无下标
    };
    w.I32(2); w.Text("SetVariable"); w.I32(1);
    w.Scalar(1, "Hero");
    call("YDWEGetAnyTypeLocalVariable", [&w]() { w.Scalar(3, "u"); });
    w.I32(0);
    w.I32(2); w.Text("YDWESetAnyTypeLocalVariable"); w.I32(1);
    w.Scalar(0, "typename07_unit"); w.Scalar(3, "u"); w.Scalar(1, "Hero");
    w.I32(0);
    w.I32(2); w.Text("YDWESetAnyTypeLocalVariable"); w.I32(1);
    w.Scalar(0, "typename01_real"); w.Scalar(3, "v"); w.Scalar(0, "PresetHalf");
    w.I32(0);
    w.I32(2); w.Text("UseReal"); w.I32(1);
    call("GetDegree", []() {});
    w.I32(0);
    w.I32(2); w.Text("UseReal"); w.I32(1);
    call("GetRealProperty", [&w]() { w.Scalar(0, "objecttype02_unitcode"); w.Scalar(3, "hfoo"); });
    w.I32(0);
    return std::move(w.bytes);
}

std::vector<std::byte> Wct() {
    Writer w;
    w.I32(1);
    w.Text("");
    w.I32(0);
    w.I32(1);
    w.I32(0);
    return std::move(w.bytes);
}

const EcaNode& Root(const TriggerDocument& document, std::size_t row) {
    return document.ecaNodes[document.triggers.front().ecas[row]];
}

const ArgumentNode& Arg(const TriggerDocument& document, const EcaNode& eca, std::size_t position) {
    return document.argumentNodes[eca.arguments[position]];
}

EncodedTriggerDocument Encode(const TriggerDocument& document, const UiCatalog& catalog) {
    auto encoded = WtgWctCodec{}.Serialize(document, catalog);
    Check(encoded.Succeeded(), "serialize failed");
    return std::move(*encoded.value);
}

} // namespace

void TestTriggerTypes() {
    const auto root = CreateUi();
    auto catalog = UiCatalog::Load(root);
    Check(catalog.Succeeded(), "catalog did not load");
    WtgWctCodec codec;
    auto parsed = codec.Parse(Wtg(), Wct(), *catalog.value);
    Check(parsed.Succeeded(), parsed.diagnostics.empty() ? "parse failed" : parsed.diagnostics.front().message);
    TriggerDocument& document = *parsed.value;
    const TriggerTypeSystem types(*catalog.value);
    // 取值种类按 TriggerTypes 定义开放：代码名不能用变量/函数/预设，基础类型别名不放宽。
    const auto code = types.AllowedValueKinds("scriptcode");
    Check(!code.variable && !code.function && !code.preset, "scriptcode must only take typed text");
    // 第 7 列为 1 或 YDWE StringExt：按 string 取变量/函数；无标记的不可声明别名保持关闭。
    for (const auto type : {"StringExt", "modelfile"}) {
        const auto text = types.AllowedValueKinds(type);
        Check(text.variable && text.function && !text.preset, "editor base-type text accepts string variables and functions");
        Check(types.Accepts(type, "string"), "string value fits editor base-type text");
    }
    const auto plain = types.AllowedValueKinds("plainalias");
    Check(!plain.variable && !plain.function, "unmarked non-global alias stays closed");
    const auto real = types.AllowedValueKinds("real");
    Check(real.variable && real.function && real.preset, "real accepts variables, functions and presets");
    const auto selector = types.AllowedValueKinds("typename");
    Check(!selector.variable && !selector.function && selector.preset, "typename is preset-only");
    Check(!types.AllowedValueKinds("unit").preset, "unit has no presets");
    const auto unknown = types.AllowedValueKinds("Null");
    Check(unknown.variable && unknown.function && unknown.preset, "unresolved types stay unrestricted");

    // 类型表、显示名、基础类型互通、选择器识别。
    Check(types.DisplayName("degree") == "角度" && types.Find("degree")->baseType == "real", "type table");
    Check(types.Accepts("real", "degree") && types.Accepts("degree", "real"), "degree/real compatible");
    Check(!types.Accepts("real", "unit") && !types.Accepts("unit", "string"), "unrelated types accepted");
    Check(types.Accepts("unit", "AnyReturnType") && types.Accepts("AnyType", "unit"), "AnyReturnType / AnyType");
    for (const auto type : {"hashtable", "trigger", "unit", "timer", "buff", "filterfunc", "tablealias"})
        Check(types.Accepts("handle", type), "native descendant must accept handle parameter");
    Check(types.Accepts("agent", "hashtable") && types.Accepts("widget", "unit") &&
        types.Accepts("ability", "buff") && types.Accepts("boolexpr", "filterfunc"), "exact multi-level parents");
    Check(!types.Accepts("unit", "widget") && !types.Accepts("hashtable", "handle") &&
        !types.Accepts("hashtable", "trigger") && !types.Accepts("trigger", "unit"), "no downcast or sibling cast");
    for (const auto type : {"integer", "real", "string", "UnknownNative", "cyclea"})
        Check(!types.Accepts("handle", type), "scalar/unknown/cyclic type cannot become handle");
    const auto handle = types.AllowedValueKinds("handle");
    Check(handle.variable && handle.function && !handle.preset, "native parent exposes compatible typed sources");
    Check(types.IsTypeSelector("typename") && types.IsTypeSelector("objecttype") && !types.IsTypeSelector("real"),
        "type selectors");
    Check(TriggerTypeSystem::TypeFromTypename("objecttype02_unitcode") == "unitcode", "TypeFromTypename");

    // 四字码是值槽角色而不是 JASS 类型：表达式仍兼容 integer，固定值必须保留四字节身份。
    for (const auto type : {"unitcode", "itemcode", "abilcode", "buffcode", "techcode", "doodadcode",
             "destructablecode", "heroskillcode", "abilityalias", "abilityalias2"}) {
        Check(types.LiteralKind(type) == TriggerTypeSystem::Literal::ObjectCode, "object role survives complete alias chain");
        Check(types.Accepts(type, "integer") && types.Accepts("integer", type), "object expression retains integer type");
        for (const auto value : {"A0E0", "hfoo", "1234"})
            Check(types.ValidateLiteral(type, value).Succeeded(), "valid four-byte object value");
        for (const auto& value : std::vector<std::string>{"", "A0E", "A0E00", "'A0E0'", "\"A0E0\"", "A'00", "A\\00",
                 "A\n00", "null", "nil", "0", std::string("A\0E0", 4), "\xc3\xa9" "AB"})
            Check(!types.ValidateLiteral(type, value).Succeeded(), "object fixed text cannot become code or a decimal workaround");
    }
    Check(types.LiteralKind("customcode") == TriggerTypeSystem::Literal::Integer
        && types.ValidateLiteral("customcode", "1234").Succeeded()
        && !types.ValidateLiteral("customcode", "A0E0").Succeeded(), "code suffix does not grant object role");
    Check(types.ValidateLiteral("integer", "1234").Succeeded()
        && !types.ValidateLiteral("integer", "A0E0").Succeeded()
        && !types.ValidateLiteral("integer", "2147483648").Succeeded(), "ordinary integer remains strict decimal int32");
    for (const auto& [text, expected] : std::vector<std::pair<std::string, std::int32_t>>{
        {"0xFF000000", -16777216}, {"0X80000000", INT32_MIN}, {"$FFFFFFFF", -1}, {"0x7fffffff", INT32_MAX}, {"0x0", 0}}) {
        Check(types.ValidateLiteral("integer", text).Succeeded()
            && TriggerTypeSystem::ParseIntegerLiteral(text) == expected, "hexadecimal uses exact JASS int32 bit pattern");
    }
    for (const auto text : {"0x", "$", "0x100000000", "$100000000", "0x1+1", "0xGG", "0x1);evil()", "1e2"})
        Check(!types.ValidateLiteral("integer", text).Succeeded(), "hex prefix does not admit overflow or expressions");
    Check(types.ValidateLiteral("unit", "null").Succeeded()
        && types.ValidateLiteral("typename", "legacy-selector-text").Succeeded()
        && types.ValidateLiteral("UnknownLegacyType", "legacy-other-text").Succeeded(), "non-object rules remain unchanged");
    for (const auto base : {"string", "unit", "MissingAlias", "abilityalias", ""}) {
        const auto invalidRoot = root / (std::string("bad-object-") + base);
        WriteText(invalidRoot / "ui/config", "base\n");
        WriteText(invalidRoot / "ui/base/define.txt", std::string("[TriggerTypes]\nabilcode=0,1,1,Ability,") + base
            + "\nabilityalias=0,1,1,Alias,abilcode\n");
        auto invalidCatalog = UiCatalog::Load(invalidRoot);
        Check(invalidCatalog.Succeeded(), "malformed object alias fixture loads for diagnostic");
        const TriggerTypeSystem invalidTypes(*invalidCatalog.value);
        Check(invalidTypes.LiteralKind("abilityalias") == TriggerTypeSystem::Literal::Other
            && !invalidTypes.ValidateLiteral("abilityalias", "A0E0").Succeeded(), "invalid object parent never gains rawcode permission");
    }

    // 局部变量：按名称 + 类型区分；只有唯一类型时读取函数才有具体类型。
    const auto locals = types.CollectLocals(document, document.triggers.front());
    Check(locals.size() == 2 && locals[0].name == "u" && locals[0].type == "unit" && locals[1].type == "real",
        "CollectLocals");
    const EcaNode& setVariable = Root(document, 0);
    Check(types.ExpectedType(document, setVariable, 1, locals) == "unit", "SetVariable Null follows variable");
    Check(types.ValueType(document, Arg(document, setVariable, 1), locals) == "unit", "local getter resolved");
    Check(types.ExpectedType(document, Root(document, 1), 2, locals) == "unit", "typename selector");
    Check(types.ExpectedType(document, Root(document, 2), 2, locals) == "real", "typename selector real");
    const EcaNode& property = document.ecaNodes[*Arg(document, Root(document, 4), 0).callEca];
    Check(types.ExpectedType(document, property, 1, locals) == "unitcode", "objecttype selector");
    Check(types.ValueType(document, Arg(document, Root(document, 3), 0), locals) == "degree", "call return type");
    std::vector<LocalVariable> ambiguous = locals;
    ambiguous.push_back({"u", "real", false, false});
    Check(types.ValueType(document, Arg(document, setVariable, 1), ambiguous) == "AnyReturnType",
        "ambiguous local getter must stay AnyReturnType");

    const auto audit = AuditArgumentTypes(document, *catalog.value);
    Check(audit.nullArguments == 4 && audit.resolvedNull == 4 && audit.unresolved == 0 && audit.mismatched == 0,
        "audit counts");

    // 真实误拒形状：GetHandleId 的 handle 形参接受表/触发器声明；标量仍整批拒绝。
    for (const std::string_view inputType : {"hashtable", "trigger", "integer"}) {
        auto candidate = document;
        Check(Editor::AddVariable(candidate, "HandleInput", inputType).Succeeded(), "add typed handle probe");
        auto binding = CaptureTriggerGraphBinding(candidate, *catalog.value);
        Check(binding.Succeeded(), "capture handle probe binding");
        auto get = std::make_shared<GraphEcaValue>();
        get->function = "GetHandleId"; get->kind = UiDefinitionKind::Call;
        GraphArgumentValue variable; variable.scalar = {1, "HandleInput"}; get->arguments.push_back(variable);
        auto use = std::make_shared<GraphEcaValue>(); use->function = "UseInteger";
        GraphArgumentValue call; call.call = get; use->arguments.push_back(call);
        GraphParentExpectation parent{GraphParentKind::TriggerEcas,
            {GraphRecordKind::Trigger, candidate.triggers.front().id, {}}, {}, {}};
        for (const auto index : candidate.triggers.front().ecas)
            parent.orderedChildren.push_back({GraphRecordKind::Eca, candidate.ecaNodes[index].id, {}});
        TriggerGraphEditBatch batch{*binding.value, {GraphInsertSubtree{parent, {}, use}}};
        const auto edited = ApplyTriggerGraphEditBatch(candidate, *catalog.value, batch);
        Check(edited.Succeeded() == (inputType != "integer"), "generic graph enforces native handle ancestry");
        if (!edited.Succeeded()) {
            const auto after = CaptureTriggerGraphBinding(candidate, *catalog.value);
            Check(after.Succeeded() && after.value->currentWtgSha256 == binding.value->currentWtgSha256 &&
                after.value->currentWctSha256 == binding.value->currentWctSha256, "bad handle argument is atomic");
        }
    }

    // AnyReturnType 函数可以挂到任意类型的参数上（原来按返回类型严格相等会拒绝）。
    const EncodedTriggerDocument pristine = Encode(document, *catalog.value);
    const StableId useRealArg = Arg(document, Root(document, 3), 0).id;
    auto detached = Editor::DetachArgumentCall(document, useRealArg);
    Check(detached.Succeeded(), "DetachArgumentCall failed");
    Check(!Editor::FindArgument(document, useRealArg)->callEca.has_value(), "call not detached");
    auto appended = Editor::AppendArgumentCall(document, *catalog.value, useRealArg, "real", "YDWEGetAnyTypeLocalVariable");
    Check(appended.Succeeded(), "AnyReturnType call rejected for real parameter");
    Check(!Editor::AppendArgumentCall(document, *catalog.value, Arg(document, Root(document, 1), 1).id, "unit",
        "GetUnitName").Succeeded(), "string function accepted for unit parameter");
    // 撤销按栈序：先撤回挂接，再撤回摘除，文档逐字节还原；重做后再撤销仍还原。
    Check(Editor::UndoAppend(document, *appended.value).Succeeded(), "UndoAppend");
    Check(Editor::UndoDetachArgumentCall(document, *detached.value).Succeeded(), "UndoDetachArgumentCall");
    const EncodedTriggerDocument restored = Encode(document, *catalog.value);
    Check(restored.wtg == pristine.wtg && restored.wct == pristine.wct, "detach undo is not byte-identical");
    Check(Editor::RedoDetachArgumentCall(document, *detached.value).Succeeded(), "RedoDetachArgumentCall");
    Check(Editor::SetScalarArgument(document, useRealArg, 0, "PresetHalf").Succeeded(), "scalar after detach");
    Check(Arg(document, Root(document, 3), 0).value.text == "PresetHalf", "scalar value");
    Check(codec.Validate(document, *catalog.value).Succeeded(), "detached document invalid");
    Check(Editor::UndoDetachArgumentCall(document, *detached.value).Succeeded(), "undo detach after redo");
    const EncodedTriggerDocument again = Encode(document, *catalog.value);
    Check(again.wtg == pristine.wtg, "redo/undo cycle not byte-identical");

    // 删除中间的语句子树再撤销：节点回到原索引（与删除前逐项相同），此前的尾部回执仍然有效。
    auto tail = Editor::AppendRootEca(document, *catalog.value, document.triggers.front().id, UiDefinitionKind::Action, "UseReal");
    Check(tail.Succeeded(), "AppendRootEca");
    const std::vector<EcaNode> ecasBefore = document.ecaNodes;
    auto removed = Editor::RemoveEcaSubtree(document, Root(document, 3).id);
    Check(removed.Succeeded(), "RemoveEcaSubtree");
    Check(Editor::UndoRemoveEcaSubtree(document, *removed.value).Succeeded(), "UndoRemoveEcaSubtree");
    Check(document.ecaNodes.size() == ecasBefore.size(), "node count after undo");
    for (std::size_t index = 0; index < ecasBefore.size(); ++index) {
        Check(document.ecaNodes[index].id == ecasBefore[index].id &&
                document.ecaNodes[index].arguments == ecasBefore[index].arguments,
            "node not restored to its original index");
    }
    Check(Editor::UndoAppend(document, *tail.value).Succeeded(), "earlier append undo after remove/undo");
    const EncodedTriggerDocument last = Encode(document, *catalog.value);
    Check(last.wtg == pristine.wtg, "remove/undo/append-undo not byte-identical");
    std::filesystem::remove_all(root);
}
