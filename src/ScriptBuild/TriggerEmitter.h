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
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    DzTriggerRegisterMouseEventMultiple, DzTriggerRegisterKeyEventMultiple, DzTriggerRegisterMouseWheelEventMultiple,
    DzTriggerRegisterMouseMoveEventMultiple, DzTriggerRegisterWindowResizeEventMultiple, DzFrameSetUpdateCallbackMultiple,
    DzFrameSetScriptMultiple, KKGetHandleId, KKWESetObjectTableData, KKWEGetObjectTableData, KKWEFlushObjectTableData,
    // KKWE 生成器自己展开的条目：模块（运行、设置返回值）、真局部变量、选取单位到单位组（多动作），以及 YDWE 跳出循环。
    MHModuleEvaluate, MHModuleSetRetval, MHModuleSetArrayRetval, KKAPISetLocalVariables, KKAPIGetLocalVariables,
    KKAPISetAnyTypeLocalArray, KKAPIGetAnyTypeLocalArray, KKAPIEnumUnitsInRangeMultiple, YDWEExitLoop,
    // KKWE 运动器：应用到单位 / 物品 / 可破坏物 / 特效 / 装饰物（刷新、到达两个回调）、设置刷新 / 到达回调、删除运动器、清除当前运动器。
    MHMoverApplyUnit, MHMoverApplyItem, MHMoverApplyDest, MHMoverApplyEffect, MHMoverApplyDoodad,
    MHMoverSetOnUpdate, MHMoverSetOnReach, MHMoverRemove, MHMoverRemoveTriggerMover,
};

// Dz“异步注册（多项）”：回调函数名后缀（<名><后缀>T），不是这类动作时为空。
[[nodiscard]] const char* DzCallbackSuffix(Gui gui);
// KKWE 运动器闭包（应用到… / 设置刷新、到达回调）：子块 0 为参数区，其余为回调动作。
[[nodiscard]] bool IsMoverClosure(Gui gui);

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
    // 文本Cut/Insert完成后再解析语句锚点，诊断位置不能依赖发射中途的偏移。
    void ResolveDiagnosticOrigins(std::string_view source);

private:
    using Eca = Trigger::EcaNode;
    using Arg = Trigger::ArgumentNode;
    using Children = std::span<const std::uint32_t>;

    // 作用域内写出的文本记到这条语句名下，检查结果据此定位回 T；嵌套时内层优先，离开时恢复。
    class Owned {
    public:
        Owned(TriggerEmitter& emitter, const Eca& eca) : out_(emitter.out_), saved_(out_.Owner()) {
            out_.SetOwner({emitter.trigger_ != nullptr ? emitter.trigger_->id : saved_.trigger, eca.id});
        }
        ~Owned() { out_.SetOwner(saved_); }
        Owned(const Owned&) = delete;
        Owned& operator=(const Owned&) = delete;
    private:
        ScriptText& out_;
        ScriptOwner saved_;
    };

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
    // typename 预设对应的类型名：内置序号查表；函数库扩展的序号（如 typename39_frame）取预设名中的类型名。
    [[nodiscard]] std::string LocalTypeOf(const Eca& eca, std::size_t index) const;
    [[nodiscard]] Slot SlotOf(const Eca& eca, std::size_t index) const;
    // 函数库声明多一个末尾 code 参数的动作：块内动作写成独立函数作为该参数传入。
    [[nodiscard]] const Trigger::UiCallbackAction* CallbackOf(const Eca& eca) const;
    // KKWE“创建模块”：回调块，块内局部变量放在模块自己的页上（见 ModulePage）。
    [[nodiscard]] bool IsModuleCreate(const Eca& eca) const;
    [[nodiscard]] static long Block(const Eca& eca) { return eca.childId.value_or(-1); }
    [[nodiscard]] static std::string Numbered(const std::string& name, std::size_t index);
    [[nodiscard]] static std::string FuncName(const std::string& name, std::size_t index);
    void Warn(std::string code, std::string message) const;

    // —— 外部函数与触发器结构（TriggerEmitter.cpp）——
    void PutExternFunc(const Eca& eca, const std::string& name);
    void PutDzRegister(const Eca& eca, const std::string& name, Gui gui);
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
    void LocalBegin(Children children, bool isTimer, bool searchHash, long block = -1);
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
    // 把闭包体读取、但未显式传入的局部变量从外层拷进闭包页。
    void LocvarCapture(const std::string& name, const std::string& handle, const std::set<std::string>& explicitParams);
    [[nodiscard]] bool LocvarTriggerData(const std::string& functionName);
    void LocvarSleepAfter();
    void LocvarReturnBefore();
    void LocvarFlush(Gui gui);
    void PutUserData(const Eca& eca, const std::string& name, Gui gui);
    void PutObjectTableData(const Eca& eca, const std::string& name, Gui gui);
    // 本函数局部变量所在的页：模块体内为模块的当前页，否则为本次执行的帧（1：主动作函数，2：其他函数）。
    [[nodiscard]] std::string FramePage() const;
    // 调用方的页（运行模块时作为模块的上级页）：闭包内为闭包页，否则同 FramePage。
    [[nodiscard]] std::string CallerPage() const;
    // KKWE 模块的页：YDLOC[StringHash(模块函数名)][StringHash(slot)]，slot 为 Current 或 Parent。
    [[nodiscard]] std::string ModulePage(std::string_view slot) const;
    void PutModuleEvaluate(const Eca& eca, const std::string& name);
    void PutModuleRetval(const Eca& eca, const std::string& name, bool array);
    // KKWE 运动器的局部变量页：StringHash("Mover") + 运动器编号表达式。
    [[nodiscard]] static std::string MoverPage(const std::string& mover);
    void PutMoverFunctions(const Eca& eca, const std::string& name, Gui gui);
    void PutMoverClosure(const Eca& eca, const std::string& name, Gui gui);
    void PutMoverRemove(const Eca& eca, const std::string& name, bool current);
    // KKWE 真局部变量：声明成同名 JASS 局部变量（名称按局部变量名规则转换），数组加 array。
    void TrueLocalDeclare(const Eca& eca, bool array);
    [[nodiscard]] std::string TrueLocalName(const Eca& eca, bool set);
    void PutTrueLocalSet(const Eca& eca, const std::string& name, bool array);
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
    mutable std::vector<std::pair<std::size_t, ScriptOwner>> diagnosticOwners_;
    std::unordered_map<std::string, const Trigger::VariableRecord*> variables_;
    // 触发器参数值 → 脚本全局名：WE 写法映射到自身，旧版本编辑器写过的形式映射到同一个触发器的 WE 写法。
    struct TriggerSymbolTarget {
        std::string symbol;
        bool legacy = false;  // 旧形式：WE 原样写出时生成不出对应声明
    };
    std::unordered_map<std::string, TriggerSymbolTarget> triggerSymbols_;
    mutable std::unordered_set<std::string> legacyWarned_;

    const Trigger::TriggerRecord* trigger_ = nullptr;
    LocvarState locvar_;
    std::map<std::string, std::map<std::string, std::string>> registerVar_;
    std::vector<std::pair<int, std::string>> localVars_; // (类型序号, 完整变量名)
    struct TrueLocal {
        std::string name, type;
        bool array = false, handle = false;
    };
    std::vector<TrueLocal> trueLocals_;
    // 正在写的 KKWE 模块函数名（模块体及其内部生成的函数）；运行模块参数区的局部变量写进被调模块的页。
    std::optional<std::string> moduleFunction_;
    std::optional<std::string> moduleParamPage_;
    // 正在写的运动器回调层数（含其中再生成的函数）：“清除当前运动器”只在其中生成。
    int moverDepth_ = 0;
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
