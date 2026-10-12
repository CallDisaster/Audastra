#include "TriggerEmitter.h"

#include "ScriptNames.h"

namespace War3Map::Script::detail {

bool TriggerEmitter::SearchAnyPlayer(const Eca& eca) const {
    for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
        if (ArgValue(eca, i) == "PlayerALL") return true;
        if (const auto* call = ArgCall(eca, i); call != nullptr && SearchAnyPlayer(*call)) return true;
    }
    return false;
}

void TriggerEmitter::PutAction(const Eca& eca, const std::string& name, std::size_t index) {
    PutActionEx(eca, FuncName(name, index));
}

void TriggerEmitter::PutVarCode(const Eca& eca, const std::string& name, std::size_t index, bool condition) {
    const auto* call = ArgCall(eca, index);
    if (call == nullptr) return;
    const std::string child = Numbered(name, index);
    if (condition) PutConditionEx(*call, child);
    else PutActionEx(*call, child);
}

void TriggerEmitter::PutBlockAction(const Eca& eca, const std::string& name, long block) {
    for (std::size_t i = 0; i < eca.children.size(); ++i) {
        const auto& child = EcaAt(eca.children[i]);
        if (Enabled(child) && (block == -1 || Block(child) == block)) PutAction(child, name, i);
    }
}

void TriggerEmitter::PutBlockConditions(const Eca& eca, const std::string& name, long block, bool conjunction) {
    bool first = true;
    for (std::size_t i = 0; i < eca.children.size(); ++i) {
        const auto& child = EcaAt(eca.children[i]);
        if (!Enabled(child) || (block != -1 && Block(child) != block)) continue;
        if (!first) out_.Put(conjunction ? " and " : " or ");
        first = false;
        PutCondition(child, name, i);
    }
    if (first) out_.Put(conjunction ? "true" : "false");
}

void TriggerEmitter::PutBlockEvent(const Eca& eca, const std::string& name, long block, const std::string& triggerVar) {
    for (std::size_t i = 0; i < eca.children.size(); ++i) {
        const auto& child = EcaAt(eca.children[i]);
        if (block == -1 || Block(child) == block) PutEventRegisterEx(child, triggerVar, name, i);
    }
}

void TriggerEmitter::PutCondition(const Eca& eca, const std::string& name, std::size_t index) {
    PutConditionEx(eca, FuncName(name, index));
}

void TriggerEmitter::PutConditionEx(const Eca& eca, const std::string& name) {
    // 比较运算：第一个比较运算符槽前后各一个操作数。
    for (const char* operatorType : {"ComparisonOperator", "EqualNotEqualOperator"}) {
        for (std::size_t i = 1; i + 1 < eca.arguments.size(); ++i) {
            if (DeclaredType(eca, i) != operatorType) continue;
            out_.Put("(");
            PutVar(eca, name, i - 1);
            out_.Put(" ");
            PutVar(eca, name, i);
            out_.Put(" ");
            PutVar(eca, name, i + 1);
            out_.Put(")");
            return;
        }
    }
    const Gui gui = GuiOf(eca);
    switch (gui) {
    case Gui::TestTriggerConditions:
        out_.Put("true");
        return;
    case Gui::GetBooleanAnd:
    case Gui::GetBooleanOr:
        out_.Put("(");
        PutVarCode(eca, name, 0, true);
        out_.Put(gui == Gui::GetBooleanAnd ? " and " : " or ");
        PutVarCode(eca, name, 1, true);
        out_.Put(")");
        return;
    case Gui::AndMultiple:
    case Gui::OrMultiple:
        out_.Put("(");
        PutBlockConditions(eca, name, 0, gui == Gui::AndMultiple);
        out_.Put(")");
        return;
    default:
        break;
    }
    const bool bare = gui == Gui::GetBooleanVariableValue;
    if (!bare) out_.Put(ScriptName(eca) + "(");
    for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
        if (i > 0) out_.Put(", ");
        PutVar(eca, name, i);
    }
    if (!bare) out_.Put(")");
}

void TriggerEmitter::PutForLoop(const Eca& eca, const std::string& name, Gui gui) {
    std::string variable;
    switch (gui) {
    case Gui::ForLoopA:
    case Gui::ForLoopAMultiple:
        variable = "bj_forLoopAIndex";
        break;
    case Gui::ForLoopB:
    case Gui::ForLoopBMultiple:
        variable = "bj_forLoopBIndex";
        break;
    case Gui::YDWEForLoopLocVarMultiple:
        variable = ConvertLocalName("ydul_" + ArgValue(eca, 0));
        break;
    case Gui::ForLoopVar:
    case Gui::ForLoopVarMultiple: {
        const auto* argument = ArgAt(eca, 0);
        if (argument == nullptr || Definition(eca) == nullptr) {
            diagnostics_.push_back({DiagnosticSeverity::Error, "Script.Trigger", "TRIGGER_WRITABLE_VARIABLE_REQUIRED",
                "整数变量循环缺少真实函数定义或可写目标参数。", eca.origin});
            return;
        }
        std::string declaredName;
        const auto status = types_.ValidateWritableVariable(document_, *argument, "integer", &declaredName);
        if (!status.Succeeded()) {
            diagnostics_.insert(diagnostics_.end(), status.diagnostics.begin(), status.diagnostics.end());
            return;
        }
        // 只复用生成的表达式文本；四个位置各自求值，不缓存数组下标或改写 JASS 赋值顺序。
        variable = Capture([&] { out_.Put(VariableName(declaredName)); PutArrayIndex(*argument, name, 0); });
        break;
    }
    default:
        break;
    }
    const bool counted = gui == Gui::ForLoopA || gui == Gui::ForLoopB || gui == Gui::ForLoopAMultiple || gui == Gui::ForLoopBMultiple;
    out_.Begin();
    if (counted) {
        out_.Put("set " + variable + " = ");
        PutVar(eca, name, 0);
        out_.Line("");
        out_.Put("set " + variable + "End = ");
        PutVar(eca, name, 1);
        out_.Line("");
    } else {
        out_.Put("set " + variable + " = ");
        PutVar(eca, name, 1);
        out_.Line("");
    }
    out_.Line("loop");
    out_.Begin();
    if (counted) {
        out_.Line("exitwhen " + variable + " > " + variable + "End");
    } else {
        out_.Put("exitwhen " + variable + " > ");
        PutVar(eca, name, 2);
        out_.Line("");
    }
    out_.End();
    if (gui == Gui::ForLoopA || gui == Gui::ForLoopB) PutVarCode(eca, name, 2, false);
    else if (gui == Gui::ForLoopVar) PutVarCode(eca, name, 3, false);
    else PutBlockAction(eca, name, 0);
    out_.Begin();
    out_.Line("set " + variable + " = " + variable + " + 1");
    out_.End();
    out_.Line("endloop");
    out_.End();
}

void TriggerEmitter::PutCallAction(const Eca& eca, const std::string& name) {
    const auto* callback = CallbackOf(eca);
    if (callback == nullptr && !eca.children.empty()) {
        // 子动作只有块语句或回调块才能生成；不能静默丢掉作者写的动作。
        diagnostics_.push_back({DiagnosticSeverity::Error, "Script.Trigger", "TRIGGER_CHILDREN_UNSUPPORTED",
            (trigger_ != nullptr ? "触发器“" + trigger_->name.text + "”：" : std::string{}) + "动作 " + eca.name.text +
                " 不是块语句，也未在函数库 J 中找到多一个末尾 code 参数的声明，其下的子动作无法生成。", eca.origin});
        return;
    }
    out_.Begin();
    out_.Put("call " + ScriptName(eca) + "(");
    for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
        if (i > 0) out_.Put(", ");
        PutVar(eca, name, i);
    }
    if (callback != nullptr) {
        bool any = false;
        for (const auto index : eca.children) any = any || Enabled(EcaAt(index));
        // 块内没有启用的动作时传 null（函数库约定为清空回调）。
        out_.Put(std::string(eca.arguments.empty() ? "" : ", ") + (any ? "function " + name + "A" : std::string("null")));
    }
    out_.Line(")");
    out_.End();
}

// 回调块：块内动作写成独立函数。遍历类在调用时立即执行，与“选取单位组做动作”的 A 函数一样共用外层局部变量；
// 其余之后才由函数库调用，拥有自己的一帧局部变量（与“运行触发器”动作函数的规则相同）。
void TriggerEmitter::PutCallbackFunction(const Eca& eca, const std::string& name) {
    bool any = false;
    for (const auto index : eca.children) any = any || Enabled(EcaAt(index));
    if (!any) return;
    if (CallbackOf(eca)->sharesLocals) {
        out_.Line("function " + name + "A takes nothing returns nothing");
        LocalBegin(eca.children, false, false);
        PutBlockAction(eca, name, -1);
        LocalEnd(false, true);
        out_.Line("endfunction");
        out_.Line("");
        return;
    }
    const LocvarState saved = locvar_;
    const bool savedEnum = enumUnitsInRange_;
    locvar_ = {};
    enumUnitsInRange_ = false;
    out_.Line("function " + name + "A takes nothing returns nothing");
    LocalBegin(eca.children, false, true);
    if (localInMainProc_) {
        // 无法从声明判断回调是立即执行（遍历类）还是之后执行（注册类），只能统一独立；明确提示差异。
        Warn("callback-local-scope", "回调块“" + eca.name.text + "”内使用了局部变量：它在之后才执行，有自己的一组局部变量，"
            "读不到也改不了外层触发器的同名局部变量。需要在内外之间传值时请改用全局变量。");
    }
    PutBlockAction(eca, name, -1);
    LocalEnd(false, true);
    out_.Line("endfunction");
    out_.Line("");
    locvar_ = saved;
    enumUnitsInRange_ = savedEnum;
}

void TriggerEmitter::PutActionEx(const Eca& eca, const std::string& name) {
    if (SearchAnyPlayer(eca)) Warn("any-player-action", "“任意玩家”只能用于事件，动作中会生成无效脚本。");
    const Gui gui = GuiOf(eca);
    const auto simple = [&](const std::string& prefix, std::initializer_list<std::size_t> args, const std::string& suffix) {
        out_.Begin();
        out_.Put(prefix);
        bool first = true;
        for (const auto i : args) {
            if (!first) out_.Put(", ");
            first = false;
            PutVar(eca, name, i);
        }
        out_.Line(suffix);
        out_.End();
    };
    switch (gui) {
    case Gui::IfThenElse:
        out_.Begin();
        out_.Put("if (");
        PutVarCode(eca, name, 0, true);
        out_.Line(") then");
        PutVarCode(eca, name, 1, false);
        out_.Line("else");
        PutVarCode(eca, name, 2, false);
        out_.Line("endif");
        out_.End();
        break;
    case Gui::IfThenElseMultiple:
        out_.Begin();
        out_.Put("if (");
        PutBlockConditions(eca, name, 0, true);
        out_.Line(") then");
        PutBlockAction(eca, name, 1);
        out_.Line("else");
        PutBlockAction(eca, name, 2);
        out_.Line("endif");
        out_.End();
        break;
    case Gui::YDWEEnumUnitsInRangeMultiple:
        if (enumUnitsInRange_) {
            Warn("enum-units-nested", "“选取范围内单位做动作（多个）”不能嵌套。");
            break;
        }
        out_.Begin();
        out_.Line("set ydl_group = CreateGroup()");
        out_.Put("call GroupEnumUnitsInRange(ydl_group, ");
        PutVar(eca, name, 0);
        out_.Put(", ");
        PutVar(eca, name, 1);
        out_.Put(", ");
        PutVar(eca, name, 2);
        out_.Line(", null)");
        out_.Line("loop");
        out_.Begin();
        out_.Line("set ydl_unit = FirstOfGroup(ydl_group)");
        enumUnitsInRange_ = true;
        out_.Line("exitwhen ydl_unit == null");
        out_.Line("call GroupRemoveUnit(ydl_group, ydl_unit)");
        out_.End();
        PutBlockAction(eca, name, 0);
        out_.Line("endloop");
        out_.Line("call DestroyGroup(ydl_group)");
        out_.End();
        enumUnitsInRange_ = false;
        break;
    case Gui::ForLoopA:
    case Gui::ForLoopB:
    case Gui::ForLoopVar:
    case Gui::ForLoopAMultiple:
    case Gui::ForLoopBMultiple:
    case Gui::ForLoopVarMultiple:
    case Gui::YDWEForLoopLocVarMultiple:
        PutForLoop(eca, name, gui);
        break;
    case Gui::YDWERegionMultiple:
        out_.Begin();
        out_.Line("// --------------------");
        out_.Put("// ");
        PutVar(eca, name, 0);
        out_.Line("");
        out_.End();
        PutBlockAction(eca, name, 0);
        out_.Begin();
        out_.Line("// --------------------");
        out_.End();
        break;
    case Gui::ReturnAction:
        LocvarReturnBefore();
        out_.Begin();
        out_.Line("return");
        out_.End();
        break;
    case Gui::YDWESetAnyTypeLocalVariable:
        LocvarSetAction(eca, name, 0, false);
        break;
    case Gui::YDWESetAnyTypeLocalArray:
        LocvarSetAction(eca, name, 0, true);
        break;
    case Gui::YDWETimerStartMultiple:
        out_.Begin();
        out_.Put("set ydl_timer = ");
        PutVar(eca, name, 0);
        out_.Line("");
        out_.End();
        LocvarParams(eca, name, 0, "ydl_timer", gui);
        out_.Begin();
        out_.Put("call TimerStart(ydl_timer, ");
        PutVar(eca, name, 1);
        out_.Put(", ");
        PutVar(eca, name, 2);
        out_.Line(", function " + name + "T)");
        out_.End();
        break;
    case Gui::YDWEExecuteTriggerMultiple:
        out_.Begin();
        out_.Put("set ydl_trigger = ");
        PutVar(eca, name, 0);
        out_.Line("");
        out_.Line(LocalExecuteTrigger("ydl_trigger"));
        out_.End();
        usesStorage_ = true;
        LocvarParams(eca, name, 0, "ydl_trigger", gui);
        out_.Begin();
        // YDTriggerExecuteTrigger(trigger, 检查条件)：true 时先求值条件。
        if (const std::string check = Capture([&] { PutVar(eca, name, 1); }); check == "false") {
            out_.Line("call TriggerExecute(ydl_trigger)");
        } else {
            if (check != "true") Warn("execute-trigger-check", "“运行触发器”的检查条件参数应为 true 或 false。");
            out_.Line("call DoNothing()");
            out_.Line(check == "true" ? "if TriggerEvaluate(ydl_trigger) then" : "if not (" + check + ") or TriggerEvaluate(ydl_trigger) then");
            out_.Line("call TriggerExecute(ydl_trigger)");
            out_.Line("endif");
        }
        out_.End();
        break;
    case Gui::YDWETimerStartFlush:
    case Gui::YDWERegisterTriggerFlush:
        LocvarFlush(gui);
        break;
    case Gui::TriggerSleepAction:
    case Gui::PolledWait:
        out_.Begin();
        out_.Put(gui == Gui::PolledWait ? "call PolledWait(" : "call TriggerSleepAction(");
        PutVar(eca, name, 0);
        out_.Line(")");
        LocvarSleepAfter();
        out_.End();
        break;
    case Gui::YDWERegisterTriggerMultiple:
        out_.Begin();
        out_.Put("set ydl_trigger = ");
        PutVar(eca, name, 0);
        out_.Line("");
        out_.End();
        LocvarParams(eca, name, 1, "ydl_trigger", gui);
        PutBlockEvent(eca, name, 0, "ydl_trigger");
        out_.Begin();
        out_.Line("call TriggerAddCondition(ydl_trigger, Condition(function " + name + "Conditions))");
        out_.End();
        break;
    case Gui::YDWESaveAnyTypeDataByUserData:
    case Gui::YDWEFlushAnyTypeDataByUserData:
    case Gui::YDWEFlushAllByUserData:
        out_.Begin();
        PutUserData(eca, name, gui);
        out_.End();
        break;
    case Gui::YDWEActivateTrigger: {
        const std::string trigger = ArgValue(eca, 0);
        const std::string mode = ArgValue(eca, 1);
        if (trigger.rfind("gg_trg_", 0) == 0) {
            const std::string suffix = VariableName(trigger).substr(7);
            out_.Begin();
            if (mode == "OnOffOn") out_.Line("call ExecuteFunc(\"InitTrig_" + suffix + "_Orig\")");
            else if (mode == "OnOffOff") out_.Line("call InitTrig_" + suffix + "_Orig()");
            out_.End();
        }
        break;
    }
    case Gui::SetVariable:
        out_.Begin();
        out_.Put("set ");
        PutVar(eca, name, 0);
        out_.Put(" = ");
        PutVar(eca, name, 1);
        out_.Line("");
        out_.End();
        break;
    case Gui::ForGroupMultiple:
        simple("call ForGroupBJ(", {0}, ", function " + name + "A)");
        break;
    case Gui::ForForceMultiple:
        simple("call ForForce(", {0}, ", function " + name + "A)");
        break;
    case Gui::EnumDestructablesInRectAllMultiple:
        simple("call EnumDestructablesInRectAll(", {0}, ", function " + name + "A)");
        break;
    case Gui::EnumDestructablesInCircleBJMultiple:
        simple("call EnumDestructablesInCircleBJ(", {0, 1}, ", function " + name + "A)");
        break;
    case Gui::EnumItemsInRectBJMultiple:
        simple("call EnumItemsInRectBJ(", {0}, ", function " + name + "A)");
        break;
    case Gui::WaitForCondition:
        out_.Begin();
        out_.Line("loop");
        out_.Begin();
        out_.Put("exitwhen ( ");
        PutVar(eca, name, 0, true);
        out_.Put("()");
        out_.Line(" )");
        out_.Put("call TriggerSleepAction(RMaxBJ(bj_WAIT_FOR_COND_MIN_INTERVAL, ");
        PutVar(eca, name, 1);
        out_.Line("))");
        out_.End();
        out_.Line("endloop");
        out_.End();
        break;
    case Gui::CommentString:
        out_.Begin();
        out_.Put("// ");
        PutVar(eca, name, 0);
        out_.Line("");
        out_.End();
        break;
    case Gui::CustomScriptCode:
        out_.Begin();
        PutVar(eca, name, 0);
        out_.Line("");
        out_.End();
        break;
    case Gui::AddTriggerEvent: {
        const auto* event = ArgCall(eca, 1);
        if (event == nullptr) return;
        out_.Begin();
        out_.Put("call " + ScriptName(*event) + "(");
        PutVar(eca, name, 0);
        const std::string eventName = name + "002";
        for (std::size_t i = 0; i < event->arguments.size(); ++i) {
            out_.Put(", ");
            PutVar(*event, eventName, i, true);
        }
        out_.Line(")");
        out_.End();
        break;
    }
    default:
        PutCallAction(eca, name);
        break;
    }
}

} // namespace War3Map::Script::detail
