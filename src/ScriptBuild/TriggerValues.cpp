#include "TriggerEmitter.h"

#include "ScriptNames.h"

#include "War3MapCore/Script/MapScript.h"

namespace War3Map::Script::detail {
namespace {

// 预生成全局变量（gg_*）前缀与其 JASS 类型；名称部分按对象名规则转换。
struct GeneratedPrefix {
    std::string_view prefix;
    std::string_view type;
    bool convert;
};

constexpr GeneratedPrefix kGenerated[] = {
    {"gg_trg_", "trigger", true}, {"gg_rct_", "rect", true}, {"gg_snd_", "sound", true},
    {"gg_cam_", "camerasetup", true}, {"gg_unit_", "unit", false}, {"gg_item_", "item", false},
    {"gg_dest_", "destructable", false},
};

} // namespace

std::string TriggerEmitter::Capture(const std::function<void()>& put) {
    const std::size_t start = out_.Size();
    put();
    std::string text = out_.Cut(start);
    // 在行首取值时会带上缩进。
    const auto first = text.find_first_not_of(" \t");
    const auto last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

std::string TriggerEmitter::ArgValue(const Eca& eca, std::size_t index) const {
    const auto* argument = ArgAt(eca, index);
    return argument == nullptr ? std::string{} : argument->value.text;
}

std::string TriggerEmitter::VariableName(std::string_view value) const {
    for (const auto& generated : kGenerated) {
        if (value.rfind(generated.prefix, 0) == 0) {
            return generated.convert
                ? std::string(generated.prefix) + ConvertObjectName(value.substr(generated.prefix.size()))
                : std::string(value);
        }
    }
    return "udg_" + std::string(value);
}

std::string TriggerEmitter::VariableType(std::string_view value) const {
    for (const auto& generated : kGenerated) {
        if (value.rfind(generated.prefix, 0) == 0) return std::string(generated.type);
    }
    const auto found = variables_.find(std::string(value));
    return found == variables_.end() ? std::string{} : found->second->typeName.text;
}

// 预设值：TriggerParams 第三列；反引号表示字符串引号。
std::string TriggerEmitter::PresetValue(std::string_view preset) const {
    const auto* definition = catalog_.Find(Trigger::UiDefinitionKind::Define, preset, "TriggerParams");
    if (definition == nullptr || definition->revisions.empty()) return {};
    for (const auto& [key, value] : definition->Active().fields) {
        if (key != "value") continue;
        const auto parts = SplitDefineValue(value);
        if (parts.size() < 3) return {};
        std::string script = parts[2];
        for (auto& c : script) {
            if (c == '`') c = '"';
        }
        return script;
    }
    return {};
}

std::string TriggerEmitter::BaseType(const std::string& type) const {
    const auto* definition = catalog_.Find(Trigger::UiDefinitionKind::Define, type, "TriggerTypes");
    if (definition == nullptr || definition->revisions.empty()) return type;
    for (const auto& [key, value] : definition->Active().fields) {
        if (key != "value") continue;
        const auto parts = SplitDefineValue(value);
        return parts.size() >= 5 && !parts[4].empty() ? parts[4] : type;
    }
    return type;
}

void TriggerEmitter::PutNativeValue(const Arg& arg, const std::string& slotType) {
    const std::string& value = arg.value.text;
    switch (arg.type) {
    case 0: {
        const std::string preset = PresetValue(value);
        if (preset.empty()) Warn("preset-missing", "预设值“" + value + "”不在函数库定义中，按原文输出。");
        out_.Put(preset.empty() ? value : preset);
        return;
    }
    case 1:
        out_.Put(VariableName(value));
        return;
    case 3: {
        if (slotType == "scriptcode") {
            out_.Put(value);
            return;
        }
        const auto kind = types_.LiteralKind(slotType);
        const auto validated = types_.ValidateLiteral(slotType, value);
        if (!validated.Succeeded() && (kind == Trigger::TriggerTypeSystem::Literal::ObjectCode
            || kind == Trigger::TriggerTypeSystem::Literal::Integer || kind == Trigger::TriggerTypeSystem::Literal::Other)) {
            for (auto diagnostic : validated.diagnostics) {
                diagnostic.origin = arg.origin;
                // 写出是哪个触发器里的哪个值，作者才能找到要改的地方。
                diagnostic.message += "（" + (trigger_ != nullptr ? "触发器“" + trigger_->name.text + "”，" : std::string{})
                    + slotType + " 固定值：" + value.substr(0, 80) + "）";
                diagnostics_.push_back(std::move(diagnostic));
            }
            return;
        }
        const std::string base = BaseType(slotType);
        if (kind == Trigger::TriggerTypeSystem::Literal::String || base == "string" || slotType == "StringExt") {
            out_.Put(QuoteJassString(value));
        } else if (kind == Trigger::TriggerTypeSystem::Literal::ObjectCode) {
            out_.Put("'" + value + "'");
        } else {
            out_.Put(value);
        }
        return;
    }
    default:
        out_.Put(value);
        return;
    }
}

void TriggerEmitter::PutArrayIndex(const Arg& arg, const std::string& name, std::size_t index) {
    if (!arg.indexArgument || *arg.indexArgument >= document_.argumentNodes.size()) return;
    const auto& indexArg = document_.argumentNodes[*arg.indexArgument];
    out_.Put("[");
    if (indexArg.type == 2 && indexArg.callEca && *indexArg.callEca < document_.ecaNodes.size()) {
        // 数组下标里的函数调用按整数槽处理。
        PutCallValue("integer", document_.ecaNodes[*indexArg.callEca], name, index);
    } else {
        PutNativeValue(indexArg, "integer");
        PutArrayIndex(indexArg, name, index);
    }
    out_.Put("]");
}

void TriggerEmitter::PutVar(const Eca& eca, const std::string& name, std::size_t index, bool endLine) {
    const auto* argument = ArgAt(eca, index);
    if (argument == nullptr) {
        Warn("argument-missing", "“" + eca.name.text + "”缺少第 " + std::to_string(index + 1) + " 个参数。");
        return;
    }
    const Slot slot = SlotOf(eca, index);
    if (slot == Slot::Value && !(argument->type == 2 && ResolvedType(eca, index) == "boolexpr")) {
        PutVarOther(eca, index, *argument, name, index);
        return;
    }
    const auto* call = ArgCall(eca, index);
    if (call != nullptr && call->name.text == "Null") {
        out_.Put("null");
        return;
    }
    const std::string function = Numbered(name, index);
    if (endLine || slot == Slot::EventCall || slot == Slot::Value) out_.Put(function);
    else if (slot == Slot::Code) out_.Put("function " + function);
    else if (DeclaredType(eca, index) == "boolcall") out_.Put(function + "()");
    else out_.Put("Condition(function " + function + ")");
}

void TriggerEmitter::PutVarOther(const Eca& owner, std::size_t slot, const Arg& arg, const std::string& name,
    std::size_t index) {
    if (arg.type == 2 && arg.callEca && *arg.callEca < document_.ecaNodes.size()) {
        PutCallValue(ResolvedType(owner, slot), document_.ecaNodes[*arg.callEca], name, index);
        return;
    }
    PutNativeValue(arg, ResolvedType(owner, slot));
    PutArrayIndex(arg, name, index);
}

void TriggerEmitter::PutCallValue(const std::string& slotType, const Eca& call, const std::string& name,
    std::size_t index) {
    const Owned owned(*this, call);
    const std::string child = Numbered(name, index);
    if (call.arguments.size() == 3 && DeclaredType(call, 1) == "ArithmeticOperator") {
        out_.Put("(");
        PutVar(call, child, 0);
        out_.Put(" ");
        PutVar(call, child, 1);
        out_.Put(" ");
        PutVar(call, child, 2);
        out_.Put(")");
        return;
    }
    switch (GuiOf(call)) {
    case Gui::GetEnumUnit:
        out_.Put(enumUnitsInRange_ ? "ydl_unit" : "GetEnumUnit()");
        return;
    case Gui::GetFilterUnit:
        out_.Put(enumUnitsInRange_ ? "ydl_unit" : "GetFilterUnit()");
        return;
    case Gui::YDWECustomScriptCode:
        PutVar(call, child, 0, true);
        return;
    case Gui::YDWEGetAnyTypeLocalVariable:
        LocvarGet(slotType, ArgValue(call, 0), locvar_);
        return;
    case Gui::YDWEGetAnyTypeLocalArray:
        LocvarGetArray(slotType, ArgValue(call, 0), locvar_, [&] { PutVar(call, child, 1); });
        return;
    case Gui::YDWELoadAnyTypeDataByUserData: {
        const auto keyType = LocalTypeOf(call, 0);
        if (keyType.empty()) return;
        out_.Put(Storage(slotType).load + "(" + std::string(kUserDataTable) + ", ");
        PutUserDataTable(keyType, call, name, 1);
        out_.Put(", " + detail::StorageKey(ArgValue(call, 2)) + ")");
        return;
    }
    case Gui::YDWEHaveSavedAnyTypeDataByUserData: {
        const auto keyType = LocalTypeOf(call, 0);
        const auto valueType = LocalTypeOf(call, 2);
        if (keyType.empty() || valueType.empty()) return;
        out_.Put(Storage(valueType).have + "(" + std::string(kUserDataTable) + ", ");
        PutUserDataTable(keyType, call, name, 1);
        out_.Put(", " + detail::StorageKey(ArgValue(call, 3)) + ")");
        return;
    }
    case Gui::YDWEForLoopLocVarIndex:
        out_.Put("ydul_" + ConvertLocalName(ArgValue(call, 0)));
        return;
    case Gui::GetTriggerName:
        out_.Put("\"" + (trigger_ != nullptr ? trigger_->name.text : std::string("未知")) + "\"");
        return;
    case Gui::OperatorString:
        out_.Put("(");
        PutVar(call, child, 0);
        out_.Put(" + ");
        PutVar(call, child, 1);
        out_.Put(")");
        return;
    default:
        break;
    }
    // 保留实际Property宏调用供注入器识别，再由同源YDTrigger.h展开。
    // 提前展开为EXExecuteScript会丢失cfg登记的依赖；注释不能充当代码依赖。
    const std::string_view callName = call.name.text;
    if (callName == "YDWEGetObjectPropertyInteger" || callName == "YDWEGetObjectPropertyReal" ||
        callName == "YDWEGetObjectPropertyString") {
        const std::string objectType = Capture([&] { PutVar(call, child, 0); });
        if (ObjectPropertyTable(objectType).empty())
            Warn("object-property-type", "物体属性的类型“" + objectType + "”无法识别。");
    }
    // 事件响应捕获按脚本名匹配；值位置的调用与 YDWE 一致写定义名（如 GetKillingUnitBJ）。
    if (LocvarTriggerData(ScriptName(call))) return;
    out_.Put(call.name.text + "(");
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        if (i > 0) out_.Put(", ");
        PutVar(call, child, i);
    }
    out_.Put(")");
}

} // namespace War3Map::Script::detail
