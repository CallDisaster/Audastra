#pragma once

#include "ScriptText.h"
#include "YDTriggerStorage.h"

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace War3Map::Script::detail {

// WE 内建与 YDTrigger 扩展的特殊 ECA（其余函数按目录中的脚本名普通调用）。
enum class Gui : std::uint16_t {
    None,
    SetVariable, GetBooleanVariableValue, OperatorString, IfThenElse, IfThenElseMultiple,
    ForLoopA, ForLoopB, ForLoopVar, ForLoopAMultiple, ForLoopBMultiple, ForLoopVarMultiple,
    ReturnAction, RunTriggerActions, TestTriggerConditions, GetBooleanAnd, GetBooleanOr,
    AndMultiple, OrMultiple, WaitForCondition, CommentString, CustomScriptCode, AddTriggerEvent,
    ForGroupMultiple, ForForceMultiple, EnumDestructablesInRectAllMultiple,
    EnumDestructablesInCircleBJMultiple, EnumItemsInRectBJMultiple, GetTriggerName,
    YDWESetAnyTypeLocalVariable, YDWEGetAnyTypeLocalVariable, YDWESetAnyTypeLocalArray,
    YDWEGetAnyTypeLocalArray, YDWESaveAnyTypeDataByUserData, YDWELoadAnyTypeDataByUserData,
    YDWEHaveSavedAnyTypeDataByUserData, YDWEFlushAnyTypeDataByUserData, YDWEFlushAllByUserData,
    YDWETimerStartMultiple, YDWETimerStartFlush, YDWERegisterTriggerMultiple,
    YDWERegisterTriggerFlush, YDWEForLoopLocVarMultiple, YDWEForLoopLocVarIndex,
    YDWECustomScriptCode, YDWEEnumUnitsInRangeMultiple, YDWEActivateTrigger, GetEnumUnit,
    GetFilterUnit, TriggerSleepAction, PolledWait, YDWERegionMultiple, YDWEExecuteTriggerMultiple,
};

// 参数槽的类别：影响参数是写成值，还是写成外部函数引用。
enum class Slot : std::int8_t { Value = -1, EventCall = 0, BoolExpr = 1, Code = 2 };

// 局部变量存储（locvar）的上下文：所在的计时器 / 触发器闭包及其句柄表达式。
struct LocvarState {
    std::uint32_t motherId = 0;
    std::string name;
    std::optional<std::string> handle;
    std::optional<std::string> prevHandle;
    std::uint32_t lastMotherId = 0;
};

constexpr std::uint32_t kParamsFlag = 0x10000;

// 把一个 GUI 触发器写成 JASS。行为对照 YDWE 的可观察输出独立实现：外部函数命名 <名>Func%03d / <名>%03d、
// 局部变量声明与释放、计时器 / 触发器闭包的参数捕获。YDTrigger 的存储、执行触发器、物体属性与
// “任意玩家”循环等宏直接写成展开后的原生调用（见 YDTriggerStorage.h），不依赖 Wave。
class TriggerEmitter {
public:
    TriggerEmitter(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
        ScriptText& out, std::vector<Diagnostic>& diagnostics, bool debug = false, bool nullSafeHandleSet = false,
        const Trigger::TriggerTypeSystem* sharedTypes = nullptr);
    // 生成器只在一个调用栈内使用，借用的类型目录须覆盖整个生成过程。
    TriggerEmitter(const TriggerEmitter&) = delete;
    TriggerEmitter& operator=(const TriggerEmitter&) = delete;

    void EmitGuiTrigger(const Trigger::TriggerRecord& trigger);
    [[nodiscard]] std::string TriggerVariable(const Trigger::TriggerRecord& trigger) const;
    [[nodiscard]] std::string InitTriggerName(const Trigger::TriggerRecord& trigger) const;
    [[nodiscard]] bool HasDisableRegister(const Trigger::TriggerRecord& trigger) const;
    [[nodiscard]] bool HasMapInitEvent(const Trigger::TriggerRecord& trigger) const;
    [[nodiscard]] static bool Enabled(const Trigger::EcaNode& eca) { return eca.enabled != 0; }
    // 已写出的触发器用到了 YDTrigger 存储（局部变量或自定义值），需要引入 YDTriggerSaveLoadSystem 库。
    [[nodiscard]] bool UsesStorage() const noexcept { return usesStorage_; }

private:
    using Eca = Trigger::EcaNode;
    using Arg = Trigger::ArgumentNode;
    using Children = std::span<const std::uint32_t>;

    // —— 文档与目录查询（TriggerEmitter.cpp）——
    [[nodiscard]] const Eca& EcaAt(std::uint32_t index) const;
    [[nodiscard]] const Arg* ArgAt(const Eca& eca, std::size_t index) const;
    [[nodiscard]] const Eca* ArgCall(const Eca& eca, std::size_t index) const;
    [[nodiscard]] Gui GuiOf(const Eca& eca) const;
    [[nodiscard]] const Trigger::UiDefinitionRevision* Definition(const Eca& eca) const;
    [[nodiscard]] std::string ScriptName(const Eca& eca) const;
    [[nodiscard]] std::string DeclaredType(const Eca& eca, std::size_t index) const;
    [[nodiscard]] std::string ResolvedType(const Eca& eca, std::size_t index) const;
    [[nodiscard]] int TypeNameIndex(const Eca& eca, std::size_t index) const;
    [[nodiscard]] Slot SlotOf(const Eca& eca, std::size_t index) const;
    // 函数库声明多一个末尾 code 参数的动作：块内动作写成独立函数作为该参数传入。
    [[nodiscard]] const Trigger::UiCallbackAction* CallbackOf(const Eca& eca) const;
    [[nodiscard]] static long Block(const Eca& eca) { return eca.childId.value_or(-1); }
    [[nodiscard]] static std::string Numbered(const std::string& name, std::size_t index);
    [[nodiscard]] static std::string FuncName(const std::string& name, std::size_t index);
    void Warn(std::string code, std::string message) const;

    // —— 外部函数与触发器结构（TriggerEmitter.cpp）——
    void PutExternFunc(const Eca& eca, const std::string& name);
    void PutExternTopDown(const Eca& eca, const std::string& name);
    void PutExternItems(const Eca& eca, const std::string& name);
    void PutTriggerExterns(const Trigger::TriggerRecord& trigger, const std::string& name);
    void PutTriggerInit(const Trigger::TriggerRecord& trigger, const std::string& name);
    void PutEventRegisterEx(const Eca& eca, const std::string& triggerVar, const std::string& name, std::size_t index);

    // —— 动作与条件（TriggerActions.cpp）——
    void PutAction(const Eca& eca, const std::string& name, std::size_t index);
    void PutActionEx(const Eca& eca, const std::string& name);
    void PutForLoop(const Eca& eca, const std::string& name, Gui gui);
    void PutCallAction(const Eca& eca, const std::string& name);
    void PutCallbackFunction(const Eca& eca, const std::string& name);
    void PutCondition(const Eca& eca, const std::string& name, std::size_t index);
    void PutConditionEx(const Eca& eca, const std::string& name);
    void PutVarCode(const Eca& eca, const std::string& name, std::size_t index, bool condition);
    void PutBlockAction(const Eca& eca, const std::string& name, long block);
    void PutBlockConditions(const Eca& eca, const std::string& name, long block, bool conjunction);
    void PutBlockEvent(const Eca& eca, const std::string& name, long block, const std::string& triggerVar);
    [[nodiscard]] bool SearchAnyPlayer(const Eca& eca) const;

    // —— 参数值（TriggerValues.cpp）——
    void PutVar(const Eca& eca, const std::string& name, std::size_t index, bool endLine = false);
    void PutVarOther(const Eca& owner, std::size_t slot, const Arg& arg, const std::string& name, std::size_t index);
    void PutCallValue(const std::string& slotType, const Eca& call, const std::string& name, std::size_t index);
    void PutNativeValue(const Arg& arg, const std::string& slotType);
    void PutArrayIndex(const Arg& arg, const std::string& name, std::size_t index);
    [[nodiscard]] std::string VariableName(std::string_view value) const;
    [[nodiscard]] std::string VariableType(std::string_view value) const;
    [[nodiscard]] std::string PresetValue(std::string_view preset) const;
    [[nodiscard]] std::string BaseType(const std::string& type) const;
    [[nodiscard]] std::string ArgValue(const Eca& eca, std::size_t index) const;

    // —— 局部变量存储（TriggerLocals.cpp）——
    void LocalBegin(Children children, bool isTimer, bool searchHash);
    void LocalEnd(bool isTimer, bool isEnd);
    void LocalSearch(Children children, bool searchHash, long block);
    void LocalSearchVar(const Eca& eca, bool searchHash);
    void LocalDeclare(int type, const std::string& prefix, const std::string& name);
    void LocvarGet(const std::string& typeName, const std::string& varName, const LocvarState& state);
    void LocvarSet(const std::string& typeName, std::uint32_t id, const std::string& varName,
        const LocvarState& state, const std::function<void()>& value);
    void LocvarGetArray(const std::string& typeName, const std::string& varName, const LocvarState& state,
        const std::function<void()>& index);
    void LocvarSetArray(const std::string& typeName, std::uint32_t id, const std::string& varName,
        const LocvarState& state, const std::function<void()>& index, const std::function<void()>& value);
    void LocvarSetAction(const Eca& eca, const std::string& name, std::uint32_t id, bool array);
    void LocvarParams(const Eca& eca, const std::string& name, long block, const std::string& handle, Gui gui);
    [[nodiscard]] bool LocvarTriggerData(const std::string& functionName);
    void LocvarSleepAfter();
    void LocvarReturnBefore();
    void LocvarFlush(Gui gui);
    void PutUserData(const Eca& eca, const std::string& name, Gui gui);
    void CheckSaveLoad(const std::string& varName, const std::string& typeName);
    // YDTrigger 存储的展开写法：table 为 YDLOC / YDHT，page 为页表达式；index 非空时键为“常量 + (index)”。
    struct StorageKey {
        std::string name;
        const std::function<void()>* index = nullptr;
    };
    // 存储类型对应的原生函数；YDTrigger 不支持的类型给出警告（仍按宏拼接规则写出名称）。
    [[nodiscard]] StorageNatives Storage(const std::string& type);
    void PutStorageKey(const StorageKey& key);
    void PutStorageLoad(std::string_view table, const std::string& type, const std::string& page, const StorageKey& key);
    void PutStorageSave(std::string_view table, const std::string& type, const std::string& page, const StorageKey& key,
        const std::function<void()>& value);
    void PutUserDataTable(const std::string& type, const Eca& eca, const std::string& name, std::size_t index);
    // 把 put 写出的片段取回为文本（用于需要按参数值选择展开形式的宏）。
    [[nodiscard]] std::string Capture(const std::function<void()>& put);

    const Trigger::TriggerDocument& document_;
    const Trigger::UiCatalog& catalog_;
    const std::optional<Trigger::TriggerTypeSystem> ownedTypes_;
    const Trigger::TriggerTypeSystem& types_;
    ScriptText& out_;
    std::vector<Diagnostic>& diagnostics_;
    std::unordered_map<std::string, const Trigger::VariableRecord*> variables_;

    const Trigger::TriggerRecord* trigger_ = nullptr;
    LocvarState locvar_;
    std::map<std::string, std::map<std::string, std::string>> registerVar_;
    std::vector<std::pair<int, std::string>> localVars_;
    std::map<std::string, std::string> saveLoadTypes_;
    bool localInMainProc_ = false;
    bool enumUnitsInRange_ = false;
    bool usesStorage_ = false;
    bool debug_ = false;
    bool nullSafeHandleSet_ = false;
};

// YDTrigger 类型名表（typename01_integer … 的序号）。
[[nodiscard]] const char* LocalTypeName(int index);
constexpr int kLocalTypeEnd = 39;

} // namespace War3Map::Script::detail
