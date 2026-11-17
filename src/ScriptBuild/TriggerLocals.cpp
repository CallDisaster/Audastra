#include "TriggerEmitter.h"

#include "ScriptNames.h"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>

namespace War3Map::Script::detail {
namespace {

constexpr std::array<const char*, kLocalTypeEnd> kLocalTypes = {
    "", "integer", "real", "boolean", "string", "timer", "trigger", "unit", "unitcode", "abilcode", "item",
    "itemcode", "group", "player", "location", "destructable", "force", "rect", "region", "sound", "effect",
    "unitpool", "itempool", "quest", "questitem", "timerdialog", "leaderboard", "multiboard", "multiboarditem",
    "trackable", "dialog", "button", "texttag", "lightning", "image", "fogstate", "fogmodifier", "radian", "degree"};

enum LocalType { kInteger = 1, kTimer = 5, kTrigger = 6, kUnit = 7, kGroup = 12, kForce = 16 };

// 计时器闭包中可自动捕获的事件响应函数及其类型。
const std::map<std::string, std::string, std::less<>>& TriggerDataTypes() {
    static const std::map<std::string, std::string, std::less<>> table = {
        {"GetTriggeringTrigger", "trigger"}, {"GetTriggerEventId", "eventid"}, {"GetEventGameState", "gamestate"},
        {"GetWinningPlayer", "player"}, {"GetTriggeringRegion", "region"}, {"GetEnteringUnit", "unit"},
        {"GetLeavingUnit", "unit"}, {"GetTriggeringTrackable", "trackable"}, {"GetClickedButton", "button"},
        {"GetClickedDialog", "dialog"}, {"GetTournamentFinishSoonTimeRemaining", "real"},
        {"GetTournamentFinishNowRule", "integer"}, {"GetTournamentFinishNowPlayer", "player"},
        {"GetSaveBasicFilename", "string"}, {"GetTriggerPlayer", "player"}, {"GetLevelingUnit", "unit"},
        {"GetLearningUnit", "unit"}, {"GetLearnedSkill", "integer"}, {"GetLearnedSkillLevel", "integer"},
        {"GetRevivableUnit", "unit"}, {"GetRevivingUnit", "unit"}, {"GetAttacker", "unit"}, {"GetRescuer", "unit"},
        {"GetDyingUnit", "unit"}, {"GetKillingUnit", "unit"}, {"GetDecayingUnit", "unit"},
        {"GetConstructingStructure", "unit"}, {"GetCancelledStructure", "unit"}, {"GetConstructedStructure", "unit"},
        {"GetResearchingUnit", "unit"}, {"GetResearched", "integer"}, {"GetTrainedUnitType", "integer"},
        {"GetTrainedUnit", "unit"}, {"GetDetectedUnit", "unit"}, {"GetSummoningUnit", "unit"},
        {"GetSummonedUnit", "unit"}, {"GetTransportUnit", "unit"}, {"GetLoadedUnit", "unit"},
        {"GetSellingUnit", "unit"}, {"GetSoldUnit", "unit"}, {"GetBuyingUnit", "unit"}, {"GetSoldItem", "item"},
        {"GetChangingUnit", "unit"}, {"GetChangingUnitPrevOwner", "player"}, {"GetManipulatingUnit", "unit"},
        {"GetManipulatedItem", "item"}, {"GetOrderedUnit", "unit"}, {"GetIssuedOrderId", "integer"},
        {"GetOrderPointX", "real"}, {"GetOrderPointY", "real"}, {"GetOrderPointLoc", "location"},
        {"GetOrderTarget", "widget"}, {"GetOrderTargetDestructable", "destructable"},
        {"GetOrderTargetItem", "item"}, {"GetOrderTargetUnit", "unit"}, {"GetSpellAbilityUnit", "unit"},
        {"GetSpellAbilityId", "integer"}, {"GetSpellAbility", "ability"}, {"GetSpellTargetLoc", "location"},
        {"GetSpellTargetDestructable", "destructable"}, {"GetSpellTargetItem", "item"},
        {"GetSpellTargetUnit", "unit"}, {"GetEventPlayerState", "playerstate"},
        {"GetEventPlayerChatString", "string"}, {"GetEventPlayerChatStringMatched", "string"},
        {"GetTriggerUnit", "unit"}, {"GetEventUnitState", "unitstate"}, {"GetEventDamage", "real"},
        {"GetEventDamageSource", "unit"}, {"GetEventDetectingPlayer", "player"}, {"GetEventTargetUnit", "unit"},
        {"GetTriggerWidget", "widget"}, {"GetTriggerDestructable", "destructable"},
    };
    return table;
}

std::uint32_t Id(Gui gui) { return static_cast<std::uint32_t>(gui); }

bool ClosureMother(std::uint32_t id) {
    return id == Id(Gui::YDWETimerStartMultiple) || id == Id(Gui::YDWERegisterTriggerMultiple)
        || (id <= 0xFFFFu && (DzCallbackSuffix(static_cast<Gui>(id)) != nullptr || IsMoverClosure(static_cast<Gui>(id))));
}

// 闭包页：计时器 / 触发器闭包按句柄，Dz 异步回调与运动器为写好的页表达式（以常量开头）。
std::string ClosurePage(const std::string& handle) {
    return handle.starts_with("0x") ? handle : HandlePage(handle);
}

std::string_view SaveLoadType(std::string_view type) {
    if (type == "StringExt" || type == "imagefile") return "string";
    return type;
}

} // namespace

const char* LocalTypeName(int index) {
    return index > 0 && index < kLocalTypeEnd ? kLocalTypes[static_cast<std::size_t>(index)] : "";
}

void TriggerEmitter::LocalDeclare(int type, const std::string& prefix, const std::string& name) {
    const std::string variable = prefix + "_" + ConvertLocalName(name);
    for (const auto& [existingType, existing] : localVars_) {
        (void)existingType;
        if (existing == variable) return;
    }
    if (localVars_.size() >= 0x100) return;
    localVars_.emplace_back(type, variable);
    out_.Begin();
    out_.Line(std::string("local ") + LocalTypeName(type) + " " + variable);
    out_.End();
}

// KKWE 真局部变量：同一函数内同名只声明一次；类型或是否数组不一致时照先出现的声明，并说明后者会被检查拒绝。
void TriggerEmitter::TrueLocalDeclare(const Eca& eca, bool array) {
    const std::string type = LocalTypeOf(eca, 0);
    const std::string name = ConvertLocalName(ArgValue(eca, 1));
    if (type.empty() || !IsJassIdentifier(name)) {
        Warn("true-local-invalid", "真局部变量“" + ArgValue(eca, 1) + "”缺少有效的类型或名称，不会声明。");
        return;
    }
    const std::string jassType = BaseType(type);
    for (const auto& local : trueLocals_) {
        if (local.name != name) continue;
        if (local.type != jassType || local.array != array) {
            Warn("true-local-conflict", "真局部变量“" + name + "”在同一函数里先后以 " + local.type + (local.array ? " 数组" : "")
                + " 和 " + jassType + (array ? " 数组" : "") + " 使用，只能按前者声明。");
        }
        return;
    }
    const bool handle = StorageFor(jassType).have == "HaveSavedHandle";
    trueLocals_.push_back({name, jassType, array, handle});
    out_.Begin();
    out_.Line("local " + jassType + (array ? " array " : " ") + name);
    out_.End();
}

std::string TriggerEmitter::TrueLocalName(const Eca& eca, bool set) {
    const std::string name = ConvertLocalName(ArgValue(eca, set ? 1 : 0));
    const bool declared = std::any_of(trueLocals_.begin(), trueLocals_.end(),
        [&](const TrueLocal& local) { return local.name == name; });
    if (!declared) {
        Warn("true-local-undeclared", "真局部变量“" + ArgValue(eca, set ? 1 : 0) + "”在这个函数里没有设置过。真局部变量只在设置它的"
            "同一个函数里有效，计时器、触发器和“做动作”类的块内动作都在单独的函数里，读不到外面的值。");
    }
    return name;
}

void TriggerEmitter::PutTrueLocalSet(const Eca& eca, const std::string& name, bool array) {
    const std::string local = TrueLocalName(eca, true);
    out_.Begin();
    out_.Put("set " + local);
    if (array) {
        out_.Put("[");
        PutVar(eca, name, 2);
        out_.Put("]");
    }
    out_.Put(" = ");
    PutVar(eca, name, array ? 3 : 2);
    out_.Line("");
    out_.End();
}

void TriggerEmitter::LocalSearchVar(const Eca& eca, bool searchHash) {
    for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
        const auto* call = ArgCall(eca, i);
        if (call == nullptr) continue;
        const Gui gui = GuiOf(*call);
        if (searchHash && (gui == Gui::YDWEGetAnyTypeLocalVariable || gui == Gui::YDWEGetAnyTypeLocalArray)) {
            localInMainProc_ = true;
        }
        // “如果-那么-否则”等把动作写在参数里，其中设置的真局部变量也属于本函数。
        if (gui == Gui::KKAPISetLocalVariables || gui == Gui::KKAPISetAnyTypeLocalArray)
            TrueLocalDeclare(*call, gui == Gui::KKAPISetAnyTypeLocalArray);
        LocalSearchVar(*call, searchHash);
    }
}

void TriggerEmitter::LocalSearch(Children children, bool searchHash, long block) {
    for (const auto index : children) {
        const auto& eca = EcaAt(index);
        if (!Enabled(eca) || (block != -1 && Block(eca) != block)) continue;
        LocalSearchVar(eca, searchHash);
        switch (GuiOf(eca)) {
        case Gui::IfThenElseMultiple:
        case Gui::ForLoopAMultiple:
        case Gui::ForLoopBMultiple:
        case Gui::ForLoopVarMultiple:
        case Gui::YDWERegionMultiple:
            LocalSearch(eca.children, searchHash, -1);
            break;
        case Gui::YDWEForLoopLocVarMultiple:
            LocalDeclare(kInteger, "ydul", ArgValue(eca, 0));
            LocalSearch(eca.children, searchHash, -1);
            break;
        case Gui::EnumDestructablesInRectAllMultiple:
        case Gui::EnumDestructablesInCircleBJMultiple:
        case Gui::EnumItemsInRectBJMultiple:
            if (searchHash) LocalSearch(eca.children, searchHash, -1);
            break;
        case Gui::ForGroupMultiple:
        case Gui::YDWEEnumUnitsInRangeMultiple:
            LocalDeclare(kGroup, "ydl", "group");
            LocalDeclare(kUnit, "ydl", "unit");
            LocalSearch(eca.children, searchHash, 0);
            break;
        case Gui::YDWETimerStartMultiple:
            LocalDeclare(kTimer, "ydl", "timer");
            LocalSearch(eca.children, searchHash, 0);
            break;
        case Gui::YDWEExecuteTriggerMultiple:
            LocalDeclare(kInteger, "ydl", "triggerstep");
            LocalDeclare(kTrigger, "ydl", "trigger");
            // 参数区的赋值写进被调触发器的页，不算本函数的局部变量；但赋值右边读取的局部变量属于本函数，
            // 只在这里读局部变量的触发器（例如只把参数转交给别的触发器）也要开本次执行的帧，否则读到空值。
            if (searchHash) {
                for (const auto param : eca.children) {
                    const auto& child = EcaAt(param);
                    if (Enabled(child) && Block(child) == 0) LocalSearchVar(child, true);
                }
            }
            LocalSearch(eca.children, false, 0);
            break;
        case Gui::YDWERegisterTriggerMultiple:
            LocalDeclare(kTrigger, "ydl", "trigger");
            LocalSearch(eca.children, searchHash, 0);
            break;
        case Gui::YDWESetAnyTypeLocalVariable:
        case Gui::YDWESetAnyTypeLocalArray:
            if (searchHash) localInMainProc_ = true;
            break;
        case Gui::KKAPISetLocalVariables:
        case Gui::KKAPISetAnyTypeLocalArray:
            TrueLocalDeclare(eca, GuiOf(eca) == Gui::KKAPISetAnyTypeLocalArray);
            break;
        case Gui::MHModuleEvaluate:
            // 本函数的页作为模块的上级页传入（模块写回返回值的地方）；参数区的动作就地生成。
            if (searchHash) localInMainProc_ = true;
            LocalSearch(eca.children, searchHash, -1);
            break;
        case Gui::KKAPIEnumUnitsInRangeMultiple:
            LocalDeclare(kGroup, "KKWE", "Group");
            if (searchHash) LocalSearch(eca.children, searchHash, -1);
            break;
        case Gui::MHMoverApplyUnit:
        case Gui::MHMoverApplyItem:
        case Gui::MHMoverApplyDest:
        case Gui::MHMoverApplyEffect:
        case Gui::MHMoverApplyDoodad:
        case Gui::MHMoverSetOnUpdate:
        case Gui::MHMoverSetOnReach:
            // 与计时器闭包相同：编号先存进局部变量，参数区在本函数里生成；回调动作属于运动器页。
            LocalDeclare(kInteger, "KKWE", "Mover");
            LocalSearch(eca.children, searchHash, 0);
            break;
        case Gui::MHMoverRemove:
            LocalDeclare(kInteger, "KKWE", "Mover");
            break;
        case Gui::DzTriggerRegisterMouseEventMultiple:
        case Gui::DzTriggerRegisterKeyEventMultiple:
        case Gui::DzTriggerRegisterMouseWheelEventMultiple:
        case Gui::DzTriggerRegisterMouseMoveEventMultiple:
        case Gui::DzTriggerRegisterWindowResizeEventMultiple:
        case Gui::DzFrameSetUpdateCallbackMultiple:
        case Gui::DzFrameSetScriptMultiple:
            // 回调之后异步执行，块内局部变量属于闭包页，不影响外层帧。
            break;
        default:
            // 遍历类回调块共用外层局部变量：外层函数需要据块内用法建立局部变量帧。
            if (const auto* callback = CallbackOf(eca); callback != nullptr && callback->sharesLocals && searchHash)
                LocalSearch(eca.children, searchHash, -1);
            break;
        }
    }
}

void TriggerEmitter::LocalBegin(Children children, bool isTimer, bool searchHash, long block) {
    localVars_.clear();
    trueLocals_.clear();
    if (searchHash) localInMainProc_ = false;
    LocalSearch(children, searchHash, block);
    if (!isTimer && searchHash && localInMainProc_) {
        usesStorage_ = true;
        out_.Begin();
        for (const auto line : LocalInitializeLines()) out_.Line(line);
        out_.End();
    }
}

void TriggerEmitter::LocalEnd(bool isTimer, bool isEnd) {
    if (!isTimer && localInMainProc_) {
        out_.Begin();
        out_.Line("call FlushChildHashtable(YDLOC, " + LocalPage(1) + ")");
        out_.End();
    }
    for (const auto& [type, variable] : localVars_) {
        if (type == kUnit || type == kGroup || type == kTimer || type == kTrigger || type == kForce) {
            out_.Begin();
            out_.Line("set " + variable + " = null");
            out_.End();
        }
    }
    for (const auto& local : trueLocals_) {
        if (!local.handle || local.array) continue;
        out_.Begin();
        out_.Line("set " + local.name + " = null");
        out_.End();
    }
    if (isEnd) {
        localVars_.clear();
        trueLocals_.clear();
        localInMainProc_ = false;
    }
}

void TriggerEmitter::CheckSaveLoad(const std::string& varName, const std::string& typeName) {
    const std::string type(SaveLoadType(typeName));
    const auto [found, inserted] = saveLoadTypes_.emplace(varName, type);
    if (!inserted && found->second != type) {
        Warn("local-type-conflict", "局部变量“" + varName + "”先后以 " + found->second + " 和 " + type + " 类型使用。");
    }
}

StorageNatives TriggerEmitter::Storage(const std::string& type) {
    usesStorage_ = true;
    auto natives = StorageFor(type, nullSafeHandleSet_);
    // 函数库自定义类型的 TriggerTypes 第 5 项给出 JASS 基础类型（如 frame=…,integer）；基础类型可直接存取时照此生成，
    // 与 YDTrigger 头文件对这类类型的展开一致。其余（如 dzeffectgroup=…,handle）保留 YDTRIGGER_<type> 宏，由函数库头文件定义。
    if (!natives.known) {
        const auto base = BaseType(type);
        if (base != type) {
            auto resolved = StorageFor(base, nullSafeHandleSet_);
            if (resolved.known) natives = std::move(resolved);
        }
    }
    if (!natives.known) {
        Warn("storage-type-unsupported", type == "handle"
            ? "局部变量用在 handle 参数里没有存取方式；先存进具体类型（如单位）的变量或直接用全局表达式。仍生成原有 YDTRIGGER_handle 宏，未定义时语法检查会报错。"
            : "YDTrigger 局部变量 / 自定义值的“" + type + "”类型没有内置存取方式，生成 YDTRIGGER_" + type
                + " 宏；函数库头文件未定义它时语法检查会报未声明。");
    }
    return natives;
}

void TriggerEmitter::PutStorageKey(const StorageKey& key) {
    out_.Put(detail::StorageKey(key.name));
    if (key.index != nullptr) {
        out_.Put(" + (");
        (*key.index)();
        out_.Put(")");
    }
}

void TriggerEmitter::PutStorageLoad(std::string_view table, const std::string& type, const std::string& page, const StorageKey& key) {
    out_.Put(Storage(type).load + "(" + std::string(table) + ", " + page + ", ");
    PutStorageKey(key);
    out_.Put(")");
}

void TriggerEmitter::PutStorageSave(std::string_view table, const std::string& type, const std::string& page, const StorageKey& key,
    const std::function<void()>& value) {
    out_.Put("call " + Storage(type).save + "(" + std::string(table) + ", " + page + ", ");
    PutStorageKey(key);
    out_.Put(", ");
    value();
    out_.Line(")");
}

// 读取局部变量：闭包参数区读外层页，闭包内读闭包句柄页，否则读本次执行的页（1：主动作函数，2：其他函数）。
void TriggerEmitter::LocvarGet(const std::string& typeName, const std::string& varName, const LocvarState& state) {
    const StorageKey key{varName};
    if ((state.motherId & kParamsFlag) != 0 && state.prevHandle) {
        registerVar_[state.name][varName] = typeName;
        PutStorageLoad(kLocalTable, typeName, ClosurePage(*state.prevHandle), key);
    } else if (ClosureMother(state.motherId)) {
        registerVar_[state.name][varName] = typeName;
        PutStorageLoad(kLocalTable, typeName, ClosurePage(state.handle.value_or("")), key);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageLoad(kLocalTable, typeName, FramePage(), key);
    }
}

void TriggerEmitter::LocvarSet(const std::string& typeName, std::uint32_t id, const std::string& varName,
    const LocvarState& state, const std::function<void()>& value) {
    out_.Begin();
    const StorageKey key{varName};
    // “运行模块”参数区里直接设置的局部变量写进被调模块的页（与 KKWE 相同，读取仍读本函数的页）。
    if (id == 0 && moduleParamPage_) {
        PutStorageSave(kLocalTable, typeName, *moduleParamPage_, key, value);
        out_.End();
        return;
    }
    if (id == 0) id = state.motherId;
    if ((id & 0xFFFFu) == Id(Gui::YDWEExecuteTriggerMultiple)) {
        PutStorageSave(kLocalTable, typeName, LocalPage(5), key, value);
    } else if ((id & kParamsFlag) != 0 || ClosureMother(id)) {
        PutStorageSave(kLocalTable, typeName, ClosurePage(state.handle.value_or("")), key, value);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageSave(kLocalTable, typeName, FramePage(), key, value);
    }
    out_.End();
    if ((state.motherId & kParamsFlag) != 0) {
        if (const auto found = registerVar_.find(state.name); found != registerVar_.end()) found->second.erase(varName);
    }
}

void TriggerEmitter::LocvarGetArray(const std::string& typeName, const std::string& varName, const LocvarState& state,
    const std::function<void()>& index) {
    const StorageKey key{varName, &index};
    if ((state.motherId & kParamsFlag) != 0 && state.prevHandle) {
        registerVar_[state.name][varName] = typeName;
        PutStorageLoad(kLocalTable, typeName, ClosurePage(*state.prevHandle), key);
    } else if (ClosureMother(state.motherId)) {
        registerVar_[state.name][varName] = typeName;
        PutStorageLoad(kLocalTable, typeName, ClosurePage(state.handle.value_or("")), key);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageLoad(kLocalTable, typeName, FramePage(), key);
    }
}

void TriggerEmitter::LocvarSetArray(const std::string& typeName, std::uint32_t id, const std::string& varName,
    const LocvarState& state, const std::function<void()>& index, const std::function<void()>& value) {
    out_.Begin();
    const StorageKey key{varName, &index};
    if (id == 0 && moduleParamPage_) {
        PutStorageSave(kLocalTable, typeName, *moduleParamPage_, key, value);
        out_.End();
        return;
    }
    if (id == 0) id = state.motherId;
    if ((id & 0xFFFFu) == Id(Gui::YDWEExecuteTriggerMultiple)) {
        PutStorageSave(kLocalTable, typeName, LocalPage(5), key, value);
    } else if ((id & kParamsFlag) != 0 || ClosureMother(id)) {
        PutStorageSave(kLocalTable, typeName, ClosurePage(state.handle.value_or("")), key, value);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageSave(kLocalTable, typeName, FramePage(), key, value);
    }
    out_.End();
}

void TriggerEmitter::LocvarSetAction(const Eca& eca, const std::string& name, std::uint32_t id, bool array) {
    const auto type = LocalTypeOf(eca, 0);
    if (type.empty()) {
        Warn("local-type-missing", "局部变量动作缺少有效的类型参数。");
        return;
    }
    if (array) {
        LocvarSetArray(type, id, ArgValue(eca, 1), locvar_,
            [&] { PutVar(eca, name, 2); }, [&] { PutVar(eca, name, 3); });
    } else {
        LocvarSet(type, id, ArgValue(eca, 1), locvar_, [&] { PutVar(eca, name, 2); });
    }
}

// 闭包参数：先写用户显式设置的参数，再把闭包体里读取、但未显式传入的局部变量从外层拷贝进去。
void TriggerEmitter::LocvarParams(const Eca& eca, const std::string& name, long block, const std::string& handle, Gui gui) {
    std::set<std::string> explicitParams;
    {
        const LocvarState saved = locvar_;
        locvar_.lastMotherId = locvar_.motherId;
        locvar_.prevHandle = locvar_.handle;
        locvar_.motherId = kParamsFlag | Id(gui);
        locvar_.name = name;
        locvar_.handle = handle;
        for (std::size_t i = 0; i < eca.children.size(); ++i) {
            const auto& child = EcaAt(eca.children[i]);
            if (!Enabled(child) || (block != -1 && Block(child) != block)) continue;
            switch (GuiOf(child)) {
            case Gui::YDWESetAnyTypeLocalVariable:
            case Gui::YDWESetAnyTypeLocalArray:
                explicitParams.insert(ArgValue(child, 1));
                LocvarSetAction(child, FuncName(name, i), Id(gui), GuiOf(child) == Gui::YDWESetAnyTypeLocalArray);
                break;
            case Gui::YDWEExecuteTriggerMultiple:
            case Gui::YDWETimerStartMultiple:
            case Gui::YDWERegisterTriggerMultiple:
                Warn("closure-illegal-parameter", "闭包参数区不能再嵌套计时器 / 触发器闭包。");
                break;
            default:
                PutAction(child, name, i);
                break;
            }
        }
        locvar_ = saved;
    }
    LocvarCapture(name, handle, explicitParams);
}

void TriggerEmitter::LocvarCapture(const std::string& name, const std::string& handle, const std::set<std::string>& explicitParams) {
    const auto captured = registerVar_[name];
    for (const auto& [varName, typeName] : captured) {
        if (explicitParams.contains(varName)) continue;
        const LocvarState closure{Id(Gui::YDWETimerStartMultiple), name, handle, std::nullopt, 0};
        const bool eventResponse = TriggerDataTypes().contains(varName);
        LocvarSet(typeName, 0, varName, closure, [&] {
            if (!eventResponse || locvar_.motherId == Id(Gui::YDWETimerStartMultiple)) LocvarGet(typeName, varName, locvar_);
            else out_.Put(varName + "()");
        });
    }
    registerVar_.erase(name);
}

bool TriggerEmitter::LocvarTriggerData(const std::string& functionName) {
    if ((locvar_.motherId & kParamsFlag) != 0) {
        if (locvar_.lastMotherId != Id(Gui::YDWETimerStartMultiple)) return false;
    } else if (locvar_.motherId != Id(Gui::YDWETimerStartMultiple)) {
        return false;
    }
    const auto found = TriggerDataTypes().find(functionName);
    if (found == TriggerDataTypes().end()) return false;
    LocvarGet(found->second, found->first, locvar_);
    return true;
}

void TriggerEmitter::LocvarSleepAfter() {
    if (!ClosureMother(locvar_.motherId)) {
        if (localInMainProc_) out_.Line(LocalResetCall());
    } else {
        Warn("closure-wait", "计时器 / 触发器闭包中不能使用等待。");
    }
}

void TriggerEmitter::LocvarReturnBefore() {
    LocalEnd(ClosureMother(locvar_.motherId), false);
}

void TriggerEmitter::LocvarFlush(Gui gui) {
    const bool timer = gui == Gui::YDWETimerStartFlush;
    const Gui mother = timer ? Gui::YDWETimerStartMultiple : Gui::YDWERegisterTriggerMultiple;
    if (locvar_.motherId != Id(mother)) {
        Warn("closure-flush", timer ? "“清除计时器”只能在计时器闭包中使用。" : "“清除触发器”只能在触发器闭包中使用。");
        return;
    }
    out_.Begin();
    usesStorage_ = true;
    out_.Line("call FlushChildHashtable(YDLOC, " + LocalPage(timer ? 3 : 4) + ")");
    out_.Line(timer ? "call DestroyTimer(GetExpiredTimer())" : "call DestroyTrigger(GetTriggeringTrigger())");
    out_.End();
}

std::string TriggerEmitter::FramePage() const {
    return moduleFunction_ ? ModulePage("Current") : LocalPage(localInMainProc_ ? 1 : 2);
}

std::string TriggerEmitter::CallerPage() const {
    if ((locvar_.motherId & kParamsFlag) != 0 && locvar_.prevHandle) return ClosurePage(*locvar_.prevHandle);
    if (ClosureMother(locvar_.motherId)) return ClosurePage(locvar_.handle.value_or(""));
    return FramePage();
}

std::string TriggerEmitter::ModulePage(std::string_view slot) const {
    return "LoadInteger(" + std::string(kLocalTable) + ", " + detail::StorageKey(moduleFunction_.value_or("")) + ", "
        + detail::StorageKey(slot) + ")";
}

// KKWE“运行模块”：以模块函数名的 StringHash 为模块页，先写上级页（调用方的页）和当前页，参数区动作就地生成，
// 运行后清掉模块页。运行时用 MHGame_GetCodeName 取模块函数名，与生成时写进模块体的函数名相同。
void TriggerEmitter::PutModuleEvaluate(const Eca& eca, const std::string& name) {
    const std::string args = Capture([&] {
        PutVar(eca, name, 0);
        out_.Put(", ");
        PutVar(eca, name, 1);
    });
    const std::string page = "StringHash(MHGame_GetCodeName(MHModule_GetCode(" + args + ")))";
    const std::string table(kLocalTable);
    usesStorage_ = true;
    out_.Begin();
    out_.Line("call SaveInteger(" + table + ", " + page + ", " + detail::StorageKey("Parent") + ", " + CallerPage() + ")");
    out_.Line("call SaveInteger(" + table + ", " + page + ", " + detail::StorageKey("Current") + ", " + page + ")");
    out_.End();
    const auto savedParam = moduleParamPage_;
    moduleParamPage_ = page;
    PutBlockAction(eca, name, -1);
    moduleParamPage_ = savedParam;
    out_.Begin();
    out_.Line("call " + ScriptName(eca) + "(" + args + ")");
    out_.Line("call FlushChildHashtable(" + table + ", " + page + ")");
    out_.End();
}

// KKWE“设置当前模块返回值”：写进模块的上级页（运行模块的那一方），对方之后按同名局部变量读取。
// 不在“创建模块”的块内时 KKWE 不生成代码，只给出警告；这里同样处理。
void TriggerEmitter::PutModuleRetval(const Eca& eca, const std::string& name, bool array) {
    const std::string type = LocalTypeOf(eca, 0);
    if (type.empty()) {
        Warn("local-type-missing", "“设置当前模块返回值”缺少有效的类型参数。");
        return;
    }
    if (!moduleFunction_) {
        Warn("module-retval-outside", "“设置当前模块返回值”只能放在“创建模块”的块内，这里不会生成代码（与 KKWE 相同）。");
        return;
    }
    const std::function<void()> index = [&] { PutVar(eca, name, 2); };
    out_.Begin();
    PutStorageSave(kLocalTable, type, ModulePage("Parent"), StorageKey{ArgValue(eca, 1), array ? &index : nullptr},
        [&] { PutVar(eca, name, array ? 3 : 2); });
    out_.End();
}

std::string TriggerEmitter::MoverPage(const std::string& mover) {
    return detail::StorageKey("Mover") + " + " + mover;
}

// KKWE 运动器回调：“应用到…”写刷新、到达两个函数，“设置刷新 / 到达回调”写一个。回调里的局部变量都在
// “StringHash("Mover") + 当前回调的运动器”页上（与 KKWE 相同）；读到但参数区没给的外层局部变量，由应用动作先拷进运动器页。
void TriggerEmitter::PutMoverFunctions(const Eca& eca, const std::string& name, Gui gui) {
    const LocvarState saved = locvar_;
    locvar_.lastMotherId = locvar_.motherId;
    locvar_.prevHandle = locvar_.handle;
    locvar_.motherId = Id(gui);
    locvar_.name = name;
    locvar_.handle = MoverPage("MHMover_GetTriggerMover()");
    ++moverDepth_;
    PutExternTopDown(eca, name);
    const auto body = [&](const std::string& function, long block) {
        out_.Line("function " + function + " takes nothing returns nothing");
        LocalBegin(eca.children, true, false, block);
        PutBlockAction(eca, name, block);
        LocalEnd(true, true);
        out_.Line("endfunction");
        out_.Line("");
    };
    if (gui == Gui::MHMoverSetOnUpdate || gui == Gui::MHMoverSetOnReach) {
        body(name + "Mover", 1);
    } else {
        body(name + "MoverUpdate", 1);
        body(name + "MoverReach", 2);
    }
    --moverDepth_;
    locvar_ = saved;
}

// 运动器编号只求值一次（存进 KKWE_Mover），参数区写进它的运动器页，再把回调交给运动器。
void TriggerEmitter::PutMoverClosure(const Eca& eca, const std::string& name, Gui gui) {
    out_.Begin();
    out_.Put("set KKWE_Mover = ");
    PutVar(eca, name, 0);
    out_.Line("");
    out_.End();
    LocvarParams(eca, name, 0, MoverPage("KKWE_Mover"), gui);
    out_.Begin();
    out_.Put("call " + ScriptName(eca) + "(KKWE_Mover, ");
    if (gui == Gui::MHMoverSetOnUpdate || gui == Gui::MHMoverSetOnReach) {
        out_.Line("function " + name + "Mover)");
    } else {
        PutVar(eca, name, 1);
        out_.Line(", function " + name + "MoverUpdate, function " + name + "MoverReach)");
    }
    out_.End();
}

// “删除运动器”连同它的运动器页一起清掉（与 KKWE 相同）；“清除当前运动器”用当前回调的运动器，
// 只在运动器回调里生成，放在其他地方时 KKWE 不生成代码，这里同样处理并提示。
void TriggerEmitter::PutMoverRemove(const Eca& eca, const std::string& name, bool current) {
    if (current && moverDepth_ == 0) {
        Warn("mover-remove-outside", "“清除当前运动器”只能放在运动器的刷新 / 到达动作里，这里不会生成代码（与 KKWE 相同）。");
        return;
    }
    const std::string mover = current ? "MHMover_GetTriggerMover()" : "KKWE_Mover";
    out_.Begin();
    if (!current) {
        out_.Put("set KKWE_Mover = ");
        PutVar(eca, name, 0);
        out_.Line("");
    }
    usesStorage_ = true;
    out_.Line("call FlushChildHashtable(" + std::string(kLocalTable) + ", " + MoverPage(mover) + ")");
    out_.Line("call MHMover_Remove(" + mover + ")");
    out_.End();
}

// 自定义值的表：YDHashAny2I(键类型, 键)。
void TriggerEmitter::PutUserDataTable(const std::string& type, const Eca& eca, const std::string& name, std::size_t index) {
    usesStorage_ = true;
    const auto wrap = UserDataTableWrap(type);
    if (!wrap.known) Warn("storage-key-unsupported", "YDTrigger 自定义值不能以“" + type + "”类型作为键，生成的调用无法通过语法检查。");
    out_.Put(wrap.prefix);
    PutVar(eca, name, index);
    out_.Put(wrap.suffix);
}

void TriggerEmitter::PutUserData(const Eca& eca, const std::string& name, Gui gui) {
    const std::string keyType = LocalTypeOf(eca, 0);
    if (keyType.empty()) return;
    if (gui == Gui::YDWEFlushAllByUserData) {
        out_.Put("call FlushChildHashtable(" + std::string(kUserDataTable) + ", ");
        PutUserDataTable(keyType, eca, name, 1);
        out_.Line(")");
        return;
    }
    const bool save = gui == Gui::YDWESaveAnyTypeDataByUserData;
    const std::string valueType = LocalTypeOf(eca, save ? 3 : 2);
    if (valueType.empty()) return;
    const auto natives = Storage(valueType);
    out_.Put("call " + (save ? natives.save : natives.remove) + "(" + std::string(kUserDataTable) + ", ");
    PutUserDataTable(keyType, eca, name, 1);
    out_.Put(", " + detail::StorageKey(ArgValue(eca, save ? 2 : 3)));
    if (save) {
        out_.Put(", ");
        PutVar(eca, name, 4);
    }
    out_.Line(")");
}

// KKWE 对象表（设置 / 清除）：主键与子键都按所选类型转换为整数，值按第三个类型存取 YDHT。
void TriggerEmitter::PutObjectTableData(const Eca& eca, const std::string& name, Gui gui) {
    const bool save = gui == Gui::KKWESetObjectTableData;
    const auto natives = Storage(LocalTypeOf(eca, 4));
    out_.Put("call " + (save ? natives.save : natives.remove) + "(" + std::string(kUserDataTable) + ", ");
    PutUserDataTable(LocalTypeOf(eca, 0), eca, name, 1);
    out_.Put(", ");
    PutUserDataTable(LocalTypeOf(eca, 2), eca, name, 3);
    if (save) {
        out_.Put(", ");
        PutVar(eca, name, 5);
    }
    out_.Line(")");
}

} // namespace War3Map::Script::detail
