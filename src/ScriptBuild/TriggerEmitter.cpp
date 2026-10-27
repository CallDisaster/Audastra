#include "TriggerEmitter.h"

#include "ScriptNames.h"

#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/LegacyTriggerReferences.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace War3Map::Script::detail {
namespace {

using Trigger::EcaNode;

const std::unordered_map<std::string_view, Gui>& GuiTable() {
    static const std::unordered_map<std::string_view, Gui> table = {
        {"SetVariable", Gui::SetVariable}, {"GetBooleanVariableValue", Gui::GetBooleanVariableValue},
        {"OperatorString", Gui::OperatorString}, {"IfThenElse", Gui::IfThenElse},
        {"IfThenElseMultiple", Gui::IfThenElseMultiple}, {"ForLoopA", Gui::ForLoopA}, {"ForLoopB", Gui::ForLoopB},
        {"ForLoopVar", Gui::ForLoopVar}, {"ForLoopAMultiple", Gui::ForLoopAMultiple},
        {"ForLoopBMultiple", Gui::ForLoopBMultiple}, {"ForLoopVarMultiple", Gui::ForLoopVarMultiple},
        {"ReturnAction", Gui::ReturnAction}, {"RunTriggerActions", Gui::RunTriggerActions},
        {"TestTriggerConditions", Gui::TestTriggerConditions}, {"GetBooleanAnd", Gui::GetBooleanAnd},
        {"GetBooleanOr", Gui::GetBooleanOr}, {"AndMultiple", Gui::AndMultiple}, {"OrMultiple", Gui::OrMultiple},
        {"WaitForCondition", Gui::WaitForCondition}, {"CommentString", Gui::CommentString},
        {"CustomScriptCode", Gui::CustomScriptCode}, {"AddTriggerEvent", Gui::AddTriggerEvent},
        {"ForGroupMultiple", Gui::ForGroupMultiple}, {"ForForceMultiple", Gui::ForForceMultiple},
        {"EnumDestructablesInRectAllMultiple", Gui::EnumDestructablesInRectAllMultiple},
        {"EnumDestructablesInCircleBJMultiple", Gui::EnumDestructablesInCircleBJMultiple},
        {"EnumItemsInRectBJMultiple", Gui::EnumItemsInRectBJMultiple}, {"GetTriggerName", Gui::GetTriggerName},
        {"YDWESetAnyTypeLocalVariable", Gui::YDWESetAnyTypeLocalVariable},
        {"YDWEGetAnyTypeLocalVariable", Gui::YDWEGetAnyTypeLocalVariable},
        {"YDWESetAnyTypeLocalArray", Gui::YDWESetAnyTypeLocalArray},
        {"YDWEGetAnyTypeLocalArray", Gui::YDWEGetAnyTypeLocalArray},
        {"YDWESaveAnyTypeDataByUserData", Gui::YDWESaveAnyTypeDataByUserData},
        {"YDWELoadAnyTypeDataByUserData", Gui::YDWELoadAnyTypeDataByUserData},
        {"YDWEHaveSavedAnyTypeDataByUserData", Gui::YDWEHaveSavedAnyTypeDataByUserData},
        {"YDWEFlushAnyTypeDataByUserData", Gui::YDWEFlushAnyTypeDataByUserData},
        {"YDWEFlushAllByUserData", Gui::YDWEFlushAllByUserData},
        {"YDWETimerStartMultiple", Gui::YDWETimerStartMultiple}, {"YDWETimerStartFlush", Gui::YDWETimerStartFlush},
        {"YDWERegisterTriggerMultiple", Gui::YDWERegisterTriggerMultiple},
        {"YDWERegisterTriggerFlush", Gui::YDWERegisterTriggerFlush},
        {"YDWEForLoopLocVarMultiple", Gui::YDWEForLoopLocVarMultiple},
        {"YDWEForLoopLocVarIndex", Gui::YDWEForLoopLocVarIndex}, {"YDWECustomScriptCode", Gui::YDWECustomScriptCode},
        {"YDWEEnumUnitsInRangeMultiple", Gui::YDWEEnumUnitsInRangeMultiple},
        {"YDWEActivateTrigger", Gui::YDWEActivateTrigger}, {"GetEnumUnit", Gui::GetEnumUnit},
        {"GetFilterUnit", Gui::GetFilterUnit}, {"TriggerSleepAction", Gui::TriggerSleepAction},
        {"PolledWait", Gui::PolledWait}, {"YDWERegionMultiple", Gui::YDWERegionMultiple},
        {"YDWEExecuteTriggerMultiple", Gui::YDWEExecuteTriggerMultiple},
        {"DzTriggerRegisterMouseEventMultiple", Gui::DzTriggerRegisterMouseEventMultiple},
        {"DzTriggerRegisterKeyEventMultiple", Gui::DzTriggerRegisterKeyEventMultiple},
        {"DzTriggerRegisterMouseWheelEventMultiple", Gui::DzTriggerRegisterMouseWheelEventMultiple},
        {"DzTriggerRegisterMouseMoveEventMultiple", Gui::DzTriggerRegisterMouseMoveEventMultiple},
        {"DzTriggerRegisterWindowResizeEventMultiple", Gui::DzTriggerRegisterWindowResizeEventMultiple},
        {"DzFrameSetUpdateCallbackMultiple", Gui::DzFrameSetUpdateCallbackMultiple},
        {"DzFrameSetScriptMultiple", Gui::DzFrameSetScriptMultiple},
    };
    return table;
}

std::string Unquote(std::string value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
    return value;
}

// 以函数形式传入代码 / 条件的父节点（IfThenElse … ForLoopVarMultiple）在原处内联，不单独成函数。
bool InlinesCodeSlots(Gui gui) {
    return gui >= Gui::IfThenElse && gui <= Gui::ForLoopVarMultiple;
}

} // namespace

TriggerEmitter::TriggerEmitter(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    ScriptText& out, std::vector<Diagnostic>& diagnostics, bool debug, bool nullSafeHandleSet,
    const Trigger::TriggerTypeSystem* sharedTypes)
    : document_(document), catalog_(catalog),
      ownedTypes_(sharedTypes ? std::optional<Trigger::TriggerTypeSystem>{}
                             : std::optional<Trigger::TriggerTypeSystem>{std::in_place, catalog}),
      types_(sharedTypes ? *sharedTypes : *ownedTypes_), out_(out), diagnostics_(diagnostics), debug_(debug),
      nullSafeHandleSet_(nullSafeHandleSet) {
    if (&types_.Catalog() != &catalog_) throw std::invalid_argument("共享类型目录与当前函数库不一致。");
    for (const auto& variable : document_.variables) variables_.emplace(variable.name.text, &variable);
    for (const auto& trigger : document_.triggers) {
        if (trigger.type == 0) triggerSymbols_.emplace(TriggerVariable(trigger), TriggerSymbolTarget{TriggerVariable(trigger), false});
    }
    // 旧形式只在不和任何 WE 写法撞名时才认；对上多个触发器时取文档里第一个。
    for (const auto& [symbol, targets] : Trigger::LegacyTriggerSymbols(document_)) {
        const auto trigger = std::find_if(document_.triggers.begin(), document_.triggers.end(),
            [&](const Trigger::TriggerRecord& row) { return row.id == targets.front(); });
        triggerSymbols_.emplace(symbol, TriggerSymbolTarget{TriggerVariable(*trigger), true});
    }
}

const EcaNode& TriggerEmitter::EcaAt(std::uint32_t index) const { return document_.ecaNodes.at(index); }

const Trigger::ArgumentNode* TriggerEmitter::ArgAt(const Eca& eca, std::size_t index) const {
    if (index >= eca.arguments.size()) return nullptr;
    const auto argument = eca.arguments[index];
    return argument < document_.argumentNodes.size() ? &document_.argumentNodes[argument] : nullptr;
}

const EcaNode* TriggerEmitter::ArgCall(const Eca& eca, std::size_t index) const {
    const auto* argument = ArgAt(eca, index);
    if (argument == nullptr || argument->type != 2 || !argument->callEca) return nullptr;
    return *argument->callEca < document_.ecaNodes.size() ? &document_.ecaNodes[*argument->callEca] : nullptr;
}

Gui TriggerEmitter::GuiOf(const Eca& eca) const {
    const auto found = GuiTable().find(eca.name.text);
    return found == GuiTable().end() ? Gui::None : found->second;
}

const Trigger::UiDefinitionRevision* TriggerEmitter::Definition(const Eca& eca) const {
    const auto* definition = catalog_.Find(eca.type, eca.name.text);
    return definition == nullptr || definition->revisions.empty() ? nullptr : &definition->Active();
}

std::string TriggerEmitter::ScriptName(const Eca& eca) const {
    if (const auto* definition = Definition(eca)) {
        for (const auto& [key, value] : definition->fields) {
            if ((key == "script_name" || key == "script") && !value.empty()) return Unquote(value);
        }
    }
    return eca.name.text;
}

std::string TriggerEmitter::DeclaredType(const Eca& eca, std::size_t index) const {
    const auto* definition = Definition(eca);
    if (definition == nullptr || index >= definition->arguments.size()) return {};
    return Unquote(definition->arguments[index].type);
}

// 参数槽的实际类型：SetVariable 的值随变量类型；typename 驱动的“任意类型”槽随所选类型。
std::string TriggerEmitter::ResolvedType(const Eca& eca, std::size_t index) const {
    const auto gui = GuiOf(eca);
    if (gui == Gui::SetVariable && index == 1) {
        if (const auto* variable = ArgAt(eca, 0)) return VariableType(variable->value.text);
    }
    // YDTrigger 的“任意类型”槽：类型取自前面某个预设参数名中第一个下划线之后的部分
    // （typename07_unit → unit，objecttype00_abilcode → abilcode）。
    std::size_t source = index;
    const std::string_view name = eca.name.text;
    if (gui == Gui::YDWESetAnyTypeLocalVariable && index == 2) source = 0;
    else if (gui == Gui::YDWESetAnyTypeLocalArray && index == 3) source = 0;
    else if (gui == Gui::YDWESaveAnyTypeDataByUserData && index == 1) source = 0;
    else if (gui == Gui::YDWESaveAnyTypeDataByUserData && index == 4) source = 3;
    else if (index == 1 && (gui == Gui::YDWELoadAnyTypeDataByUserData || gui == Gui::YDWEHaveSavedAnyTypeDataByUserData ||
                               gui == Gui::YDWEFlushAnyTypeDataByUserData || gui == Gui::YDWEFlushAllByUserData ||
                               name == "YDWEGetObjectPropertyInteger" || name == "YDWEGetObjectPropertyReal" ||
                               name == "YDWEGetObjectPropertyString")) {
        source = 0;
    }
    if (source != index) {
        const std::string preset = ArgValue(eca, source);
        if (const auto underscore = preset.find('_'); underscore != std::string::npos) return preset.substr(underscore + 1);
    }
    return DeclaredType(eca, index);
}

// typename 预设名的第 9、10 个字符是两位类型序号（typename07_unit → 7）。
int TriggerEmitter::TypeNameIndex(const Eca& eca, std::size_t index) const {
    const std::string value = ArgValue(eca, index);
    if (value.size() < 10 || value[8] < '0' || value[8] > '9' || value[9] < '0' || value[9] > '9') return 0;
    return (value[8] - '0') * 10 + (value[9] - '0');
}

std::string TriggerEmitter::LocalTypeOf(const Eca& eca, std::size_t index) const {
    const int number = TypeNameIndex(eca, index);
    if (number <= 0) return {};
    if (number < kLocalTypeEnd) return LocalTypeName(number);
    const std::string value = ArgValue(eca, index);
    const auto underscore = value.find('_');
    if (underscore != 10 || underscore + 1 >= value.size()) return {};
    const auto name = value.substr(underscore + 1);
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }) ? name : std::string{};
}

Slot TriggerEmitter::SlotOf(const Eca& eca, std::size_t index) const {
    const std::string type = DeclaredType(eca, index);
    if (type == "boolexpr" || type == "boolcall") return Slot::BoolExpr;
    if (type == "code") return Slot::Code;
    if (type == "eventcall") return Slot::EventCall;
    return Slot::Value;
}

const Trigger::UiCallbackAction* TriggerEmitter::CallbackOf(const Eca& eca) const {
    if (eca.type != 2 || GuiOf(eca) != Gui::None) return nullptr;
    return catalog_.CallbackActions().Find(eca.name.text);
}

std::string TriggerEmitter::Numbered(const std::string& name, std::size_t index) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%03zu", index + 1);
    return name + buffer;
}

const char* DzCallbackSuffix(Gui gui) {
    switch (gui) {
    case Gui::DzTriggerRegisterMouseEventMultiple: return "M";
    case Gui::DzTriggerRegisterKeyEventMultiple: return "K";
    case Gui::DzTriggerRegisterMouseWheelEventMultiple: return "W";
    case Gui::DzTriggerRegisterMouseMoveEventMultiple: return "MM";
    case Gui::DzTriggerRegisterWindowResizeEventMultiple: return "WR";
    case Gui::DzFrameSetUpdateCallbackMultiple: return "C";
    case Gui::DzFrameSetScriptMultiple: return "F";
    default: return nullptr;
    }
}

std::string TriggerEmitter::FuncName(const std::string& name, std::size_t index) {
    return Numbered(name + "Func", index);
}

void TriggerEmitter::Warn(std::string code, std::string message) const {
    std::string where = trigger_ != nullptr ? "触发器“" + trigger_->name.text + "”：" : std::string{};
    diagnostics_.push_back({DiagnosticSeverity::Warning, "Script.Trigger", std::move(code), where + std::move(message), {}});
}

std::string TriggerEmitter::TriggerVariable(const Trigger::TriggerRecord& trigger) const {
    return Trigger::TriggerDirectoryEditor::TriggerSymbol(trigger.name);
}

std::string TriggerEmitter::InitTriggerName(const Trigger::TriggerRecord& trigger) const {
    return "InitTrig_" + Trigger::TriggerDirectoryEditor::ScriptName(trigger.name);
}

bool TriggerEmitter::HasDisableRegister(const Trigger::TriggerRecord& trigger) const {
    for (const auto index : trigger.ecas) {
        const auto& eca = EcaAt(index);
        if (eca.type == 0 && Enabled(eca) && eca.name.text == "YDWEDisableRegister") return true;
    }
    return false;
}

bool TriggerEmitter::HasMapInitEvent(const Trigger::TriggerRecord& trigger) const {
    for (const auto index : trigger.ecas) {
        const auto& eca = EcaAt(index);
        if (eca.type == 0 && Enabled(eca) && eca.name.text == "MapInitializationEvent") return true;
    }
    return false;
}

// 外部函数：先递归参数中的调用，为条件 / 代码槽生成独立函数，再递归子块。
void TriggerEmitter::PutExternTopDown(const Eca& eca, const std::string& name) {
    const Gui parent = GuiOf(eca);
    for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
        const auto* argument = ArgAt(eca, i);
        const auto* call = ArgCall(eca, i);
        Slot slot = SlotOf(eca, i);
        if (argument != nullptr && argument->type == 2 && ResolvedType(eca, i) == "boolexpr") slot = Slot::BoolExpr;
        else if (slot == Slot::Value && call == nullptr) continue;
        if (call == nullptr || call->name.text == "Null") continue;
        const std::string child = Numbered(name, i);
        PutExternFunc(*call, child);
        if (GuiOf(*call) == Gui::ReturnAction) continue;
        if (parent == Gui::IfThenElse || parent == Gui::GetBooleanAnd || parent == Gui::GetBooleanOr) slot = Slot::Value;
        if (slot == Slot::BoolExpr) {
            out_.Line("function " + child + " takes nothing returns boolean");
            out_.Begin();
            out_.Put("return (");
            PutConditionEx(*call, child);
            out_.Line(")");
            out_.End();
            out_.Line("endfunction");
            out_.Line("");
        } else if (slot == Slot::Code && !InlinesCodeSlots(parent)) {
            out_.Line("function " + child + " takes nothing returns nothing");
            PutActionEx(*call, child);
            out_.Line("endfunction");
            out_.Line("");
        }
    }
    PutExternItems(eca, name);
    if (DzCallbackSuffix(parent) == nullptr && CallbackOf(eca) != nullptr) PutCallbackFunction(eca, name);
    switch (parent) {
    case Gui::ForGroupMultiple:
    case Gui::ForForceMultiple:
    case Gui::EnumDestructablesInRectAllMultiple:
    case Gui::EnumDestructablesInCircleBJMultiple:
    case Gui::EnumItemsInRectBJMultiple:
        out_.Line("function " + name + "A takes nothing returns nothing");
        LocalBegin(eca.children, false, false);
        PutBlockAction(eca, name, -1);
        LocalEnd(false, true);
        out_.Line("endfunction");
        out_.Line("");
        break;
    default:
        break;
    }
}

void TriggerEmitter::PutExternItems(const Eca& eca, const std::string& name) {
    for (std::size_t i = 0; i < eca.children.size(); ++i) {
        const auto& child = EcaAt(eca.children[i]);
        if (Enabled(child)) PutExternFunc(child, FuncName(name, i));
    }
}

void TriggerEmitter::PutExternFunc(const Eca& eca, const std::string& name) {
    const Owned owned(*this, eca);
    const Gui gui = GuiOf(eca);
    const char* dz = DzCallbackSuffix(gui);
    if (gui != Gui::YDWETimerStartMultiple && gui != Gui::YDWERegisterTriggerMultiple && dz == nullptr) {
        PutExternTopDown(eca, name);
        return;
    }
    const LocvarState saved = locvar_;
    locvar_.lastMotherId = locvar_.motherId;
    locvar_.prevHandle = locvar_.handle;
    locvar_.motherId = static_cast<std::uint32_t>(gui);
    locvar_.name = name;
    // Dz 异步回调没有句柄：闭包页是回调基础名的 StringHash 常量（与 YDTrigger 一致）。
    locvar_.handle = dz != nullptr ? detail::StorageKey(name)
        : gui == Gui::YDWETimerStartMultiple ? "GetExpiredTimer()" : "GetTriggeringTrigger()";
    PutExternTopDown(eca, name);
    if (dz != nullptr) {
        out_.Line("function " + name + dz + "T takes nothing returns nothing");
        LocalBegin(eca.children, true, false);
        PutBlockAction(eca, name, -1);
    } else if (gui == Gui::YDWETimerStartMultiple) {
        out_.Line("function " + name + "T takes nothing returns nothing");
        LocalBegin(eca.children, true, false);
        PutBlockAction(eca, name, 1);
    } else {
        out_.Line("function " + name + "Conditions takes nothing returns nothing");
        LocalBegin(eca.children, true, false);
        PutBlockAction(eca, name, 2);
    }
    LocalEnd(true, true);
    out_.Line("endfunction");
    out_.Line("");
    locvar_ = saved;
}

void TriggerEmitter::PutTriggerExterns(const Trigger::TriggerRecord& trigger, const std::string& name) {
    const auto externsOf = [&](int type) {
        for (std::size_t i = 0; i < trigger.ecas.size(); ++i) {
            const auto& eca = EcaAt(trigger.ecas[i]);
            if (eca.type == type && Enabled(eca)) PutExternFunc(eca, FuncName(name, i));
        }
    };
    externsOf(0);
    bool anyCondition = false;
    for (const auto index : trigger.ecas) anyCondition = anyCondition || (EcaAt(index).type == 1 && Enabled(EcaAt(index)));
    if (anyCondition) {
        externsOf(1);
        out_.Line("function " + name + "Conditions takes nothing returns boolean");
        out_.Begin();
        out_.Put("return (");
        bool first = true;
        for (std::size_t i = 0; i < trigger.ecas.size(); ++i) {
            const auto& eca = EcaAt(trigger.ecas[i]);
            if (eca.type != 1 || !Enabled(eca)) continue;
            if (!first) out_.Put(" and ");
            first = false;
            PutCondition(eca, name, i);
        }
        out_.Line(")");
        out_.End();
        out_.Line("endfunction");
        out_.Line("");
    }
    externsOf(2);
    out_.Line("function " + name + "Actions takes nothing returns nothing");
    LocalBegin(trigger.ecas, false, true);
    for (std::size_t i = 0; i < trigger.ecas.size(); ++i) {
        const auto& eca = EcaAt(trigger.ecas[i]);
        if (eca.type == 2 && Enabled(eca)) PutAction(eca, name, i);
    }
    LocalEnd(false, true);
    out_.Line("endfunction");
    out_.Line("");
}

void TriggerEmitter::PutEventRegisterEx(const Eca& eca, const std::string& triggerVar, const std::string& name,
    std::size_t index) {
    (void)index;
    if (!Enabled(eca)) return;
    if (eca.name.text == "YDWEDisableRegister" || eca.name.text == "MapInitializationEvent") return;
    const Owned owned(*this, eca);
    const std::size_t start = out_.Size();
    out_.Put("    call " + ScriptName(eca) + "(" + triggerVar);
    for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
        out_.Put(", ");
        PutVar(eca, name, i);
    }
    out_.Line(")");
    // “任意玩家”：参数中的 Player(n) 对 16 个玩家各注册一次（YDTrigger/Common/loop.h 的展开）。
    if (SearchAnyPlayer(eca)) {
        std::string line = out_.Cut(start);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        for (int player = 0; player < 16; ++player) out_.Line(ReplaceIdentifier(line, "n", std::to_string(player)));
    }
}

void TriggerEmitter::PutTriggerInit(const Trigger::TriggerRecord& trigger, const std::string& name) {
    const std::string init = InitTriggerName(trigger);
    const std::string variable = TriggerVariable(trigger);
    const bool disableRegister = HasDisableRegister(trigger);
    out_.Line("//===========================================================================");
    out_.Line("function " + init + (disableRegister ? "_Orig" : "") + " takes nothing returns nothing");
    out_.Begin();
    out_.Line("set " + variable + " = CreateTrigger()");
    if (trigger.initiallyClosed != 0) out_.Line("call DisableTrigger(" + variable + ")");
    out_.End();
    // 调试版 YDWE 写 YDWESaveTriggerName（宏展开为 DoNothing()）。
    if (debug_) {
        out_.Begin();
        out_.Line("call DoNothing()");
        out_.End();
    }
    for (std::size_t i = 0; i < trigger.ecas.size(); ++i) {
        const auto& eca = EcaAt(trigger.ecas[i]);
        if (eca.type == 0) PutEventRegisterEx(eca, variable, name, i);
    }
    bool anyCondition = false;
    for (const auto index : trigger.ecas) anyCondition = anyCondition || (EcaAt(index).type == 1 && Enabled(EcaAt(index)));
    out_.Begin();
    if (anyCondition) out_.Line("call TriggerAddCondition(" + variable + ", Condition(function " + name + "Conditions))");
    out_.Line("call TriggerAddAction(" + variable + ", function " + name + "Actions)");
    out_.End();
    out_.Line("endfunction");
    out_.Line("");
    if (disableRegister) {
        out_.Line("function " + init + " takes nothing returns nothing");
        out_.Line("endfunction");
        out_.Line("");
    }
}

void TriggerEmitter::EmitGuiTrigger(const Trigger::TriggerRecord& trigger) {
    trigger_ = &trigger;
    saveLoadTypes_.clear();
    const ScriptOwner saved = out_.Owner();
    out_.SetOwner({trigger.id, 0});
    const std::string name = "Trig_" + Trigger::TriggerDirectoryEditor::ScriptName(trigger.name);
    PutTriggerExterns(trigger, name);
    PutTriggerInit(trigger, name);
    out_.SetOwner(saved);
    trigger_ = nullptr;
}

} // namespace War3Map::Script::detail
