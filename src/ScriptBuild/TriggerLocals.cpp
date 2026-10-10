#include "TriggerEmitter.h"

#include "ScriptNames.h"

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
    return id == Id(Gui::YDWETimerStartMultiple) || id == Id(Gui::YDWERegisterTriggerMultiple);
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
    for (const auto& [existingType, existing] : localVars_) {
        (void)existingType;
        if (existing == name) return;
    }
    if (localVars_.size() >= 0x100) return;
    localVars_.emplace_back(type, name);
    out_.Begin();
    out_.Line(std::string("local ") + LocalTypeName(type) + " " + prefix + "_" + ConvertLocalName(name));
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
        default:
            break;
        }
    }
}

void TriggerEmitter::LocalBegin(Children children, bool isTimer, bool searchHash) {
    localVars_.clear();
    if (searchHash) localInMainProc_ = false;
    LocalSearch(children, searchHash, -1);
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
    for (const auto& [type, name] : localVars_) {
        if (type == kUnit || type == kGroup || type == kTimer || type == kTrigger || type == kForce) {
            out_.Begin();
            out_.Line("set ydl_" + ConvertLocalName(name) + " = null");
            out_.End();
        }
    }
    if (isEnd) {
        localVars_.clear();
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
    if (!natives.known) {
        Warn("storage-type-unsupported", "YDTrigger 局部变量 / 自定义值不支持“" + type + "”类型，生成的调用无法通过语法检查。");
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
        PutStorageLoad(kLocalTable, typeName, HandlePage(*state.prevHandle), key);
    } else if (ClosureMother(state.motherId)) {
        registerVar_[state.name][varName] = typeName;
        PutStorageLoad(kLocalTable, typeName, HandlePage(state.handle.value_or("")), key);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageLoad(kLocalTable, typeName, LocalPage(localInMainProc_ ? 1 : 2), key);
    }
}

void TriggerEmitter::LocvarSet(const std::string& typeName, std::uint32_t id, const std::string& varName,
    const LocvarState& state, const std::function<void()>& value) {
    out_.Begin();
    if (id == 0) id = state.motherId;
    const StorageKey key{varName};
    if ((id & 0xFFFFu) == Id(Gui::YDWEExecuteTriggerMultiple)) {
        PutStorageSave(kLocalTable, typeName, LocalPage(5), key, value);
    } else if ((id & kParamsFlag) != 0 || ClosureMother(id)) {
        PutStorageSave(kLocalTable, typeName, HandlePage(state.handle.value_or("")), key, value);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageSave(kLocalTable, typeName, LocalPage(localInMainProc_ ? 1 : 2), key, value);
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
        PutStorageLoad(kLocalTable, typeName, HandlePage(*state.prevHandle), key);
    } else if (ClosureMother(state.motherId)) {
        registerVar_[state.name][varName] = typeName;
        PutStorageLoad(kLocalTable, typeName, HandlePage(state.handle.value_or("")), key);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageLoad(kLocalTable, typeName, LocalPage(localInMainProc_ ? 1 : 2), key);
    }
}

void TriggerEmitter::LocvarSetArray(const std::string& typeName, std::uint32_t id, const std::string& varName,
    const LocvarState& state, const std::function<void()>& index, const std::function<void()>& value) {
    out_.Begin();
    if (id == 0) id = state.motherId;
    const StorageKey key{varName, &index};
    if ((id & 0xFFFFu) == Id(Gui::YDWEExecuteTriggerMultiple)) {
        PutStorageSave(kLocalTable, typeName, LocalPage(5), key, value);
    } else if ((id & kParamsFlag) != 0 || ClosureMother(id)) {
        PutStorageSave(kLocalTable, typeName, HandlePage(state.handle.value_or("")), key, value);
    } else {
        CheckSaveLoad(varName, typeName);
        PutStorageSave(kLocalTable, typeName, LocalPage(localInMainProc_ ? 1 : 2), key, value);
    }
    out_.End();
}

void TriggerEmitter::LocvarSetAction(const Eca& eca, const std::string& name, std::uint32_t id, bool array) {
    const int type = TypeNameIndex(eca, 0);
    if (type <= 0 || type >= kLocalTypeEnd) {
        Warn("local-type-missing", "局部变量动作缺少有效的类型参数。");
        return;
    }
    if (array) {
        LocvarSetArray(LocalTypeName(type), id, ArgValue(eca, 1), locvar_,
            [&] { PutVar(eca, name, 2); }, [&] { PutVar(eca, name, 3); });
    } else {
        LocvarSet(LocalTypeName(type), id, ArgValue(eca, 1), locvar_, [&] { PutVar(eca, name, 2); });
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
    const int key = TypeNameIndex(eca, 0);
    if (key <= 0 || key >= kLocalTypeEnd) return;
    const std::string keyType = LocalTypeName(key);
    if (gui == Gui::YDWEFlushAllByUserData) {
        out_.Put("call FlushChildHashtable(" + std::string(kUserDataTable) + ", ");
        PutUserDataTable(keyType, eca, name, 1);
        out_.Line(")");
        return;
    }
    const bool save = gui == Gui::YDWESaveAnyTypeDataByUserData;
    const int value = TypeNameIndex(eca, save ? 3 : 2);
    if (value <= 0 || value >= kLocalTypeEnd) return;
    const std::string valueType = LocalTypeName(value);
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

} // namespace War3Map::Script::detail
