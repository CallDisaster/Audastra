assert(_VERSION == "Lua 5.3", "expected target Lua 5.3")
local calls, nextId, groups, evaluated = {}, 0, 0, 0
local assignmentOrder, stringOrder, stringResults = {}, {}, {}
local function record(name, ...) calls[#calls + 1] = {name, ...} end
local function handle(kind) nextId = nextId + 1; return {kind = kind, id = nextId} end
function CreateTrigger() local value = handle("trigger"); value.enabled = true; record("CreateTrigger"); return value end
function Condition(callback) assert(type(callback) == "function"); return callback end
function TriggerAddCondition(trigger, callback) trigger.condition = callback; return handle("triggercondition") end
function TriggerAddAction(trigger, callback) trigger.action = callback; return handle("triggeraction") end
function DisableTrigger(trigger) trigger.enabled = false end
function ConditionalTriggerExecute(trigger)
    evaluated = evaluated + 1
    if trigger.enabled and trigger.condition() then trigger.action() end
end
function TriggerRegisterTimerEvent(trigger, timeout, periodic)
    assert(timeout == 1 and periodic == false); trigger.event = {timeout, periodic}; return handle("event")
end
function CreateGroup() groups = groups + 1; local result = handle("group"); result.members = {}; record("CreateGroup"); return result end
function I2R(value) return value + 0.0 end
function I2S(value) return string.format("%d", value) end
function FourCC(text)
    assert(#text == 4); local value = 0
    for i = 1, 4 do value = value * 256 + text:byte(i) end
    return value < 2147483648 and value or value - 4294967296
end
function GetUnitTypeId(unit) assert(unit ~= nil, "short circuit lost"); record("GetUnitTypeId"); return unit.typeId end
function Player(index) record("Player", index); return {kind = "player", index = index} end
function CreateUnit(owner, rawcode, x, y, facing)
    record("CreateUnit", owner, rawcode, x, y, facing)
    local result = handle("unit"); result.typeId = rawcode; return result
end
function CreateItem(rawcode, x, y) record("CreateItem", rawcode, x, y); return handle("item") end
function CreateTimer() record("CreateTimer"); return handle("timer") end
function DestroyTimer(timer) timer.destroyed = true end
function TimerStart(timer, seconds, periodic, callback)
    assert(seconds == 1 and periodic == false and type(callback) == "function")
    record("TimerStart"); timer.callback = callback
end
function ExecuteFunc(name) record("ExecuteFunc", name); assert(type(_G[name]) == "function"); _G[name]() end
function GetHandleId(value) return value.id end
function GetRandomInt(low, high) assert((low == 1 or low == 2) and high == 6); record("GetRandomInt", low); return low == 1 and 4 or 2 end
function GetRandomReal(low, high) assert(low == 0 and high == 1); record("GetRandomReal"); return 0.125 end
function ArrayIndexProbe() assignmentOrder[#assignmentOrder + 1] = "index"; return udg_I end
function ArrayValueProbe()
    assignmentOrder[#assignmentOrder + 1] = "value"
    udg_I = udg_I + 1
    return udg_Count
end
function StringProbe(name) stringOrder[#stringOrder + 1] = name; return stringResults[name] end

assert(loadfile(arg[1], "t", _G))()
assert(#calls == 0, "natives must not run at file load")
-- 未初始化数组的普通读取是typed零/null；首次[-1]写保留已披露的未证异常边界。
assert(udg_Values[-1] == 0 and udg_Values[8192] == 0 and udg_Labels[-1] == nil)
local ok, errorText = pcall(function() udg_Values[-1] = 17 end)
assert(not ok and errorText:find("LUA_ARRAY_UNALLOCATED_NEGATIVE_ONE", 1, true))
assert(udg_Values[0] == 0)
udg_Values[8192] = 19 -- 普通正越界会建立逻辑存储，但不写元素。
udg_Values[-1] = 21
assert(udg_Values[8192] == 0 and udg_Values[-1] == 0 and udg_Values[0] == 0)
InitGlobals()
assert(groups == 2)
assert(udg_Label == nil and udg_Labels[0] == nil and udg_Labels[2] == nil, "null string changed to empty text")
InitCustomTriggers()
assert(udg_Count == 0 and udg_Flag == false and evaluated == 0, "registration ran callbacks")
local passed = 0
local function test(number, assertion)
    _G["Trig_F" .. (100 + number) .. "_Actions"]()
    assertion()
    passed = passed + 1
    print(string.format("G%02d passed", number))
end
test(1, function() assert(udg_Count == 9) end)
test(2, function() assert(udg_Quotient == -2 and udg_Quotient ~= (-9 // 4)) end)
test(3, function() assert(udg_Value == 2.25) end)
test(4, function() assert(udg_Value == 2.0) end)
test(5, function() assert(udg_Label == "Count=9") end)
udg_U = nil
local before = #calls
test(6, function() assert(udg_Flag == false and #calls == before) end)
local living = handle("unit"); udg_U = living
test(7, function() assert(udg_U == nil and living.kind == "unit" and #calls == before) end)
test(8, function() assert(udg_Values[0] == 17 and udg_Count == 0) end)
test(9, function() assert(udg_Checks[0] == false and udg_Checks[1] == false and udg_Flag == false) end)
test(10, function()
    assert(udg_Groups[0] ~= udg_Groups[1]); udg_Groups[0].members[1] = living
    assert(#udg_Groups[1].members == 0 and udg_Groups[2] == nil)
end)
udg_Count, udg_Limit = 0, 2
test(11, function() assert(udg_Count == 1 and udg_I == 1 and udg_Limit == 0) end)
before = #calls
test(12, function()
    assert(calls[before + 1][1] == "Player" and calls[before + 2][1] == "CreateUnit" and calls[before + 3][1] == "CreateItem")
    local unit, item = calls[before + 2], calls[before + 3]
    assert(unit[3] == FourCC("hfoo") and unit[4] == 128 and unit[5] == 64 and unit[6] == 270)
    assert(item[2] == FourCC("ratf") and item[3] == 160 and item[4] == 64)
end)
udg_Count, udg_Flag = -1, false
ConditionalTriggerExecute(gg_trg_F113); assert(udg_Flag == false)
udg_Count = 0; ConditionalTriggerExecute(gg_trg_F113)
test(13, function() assert(udg_Flag == true and gg_trg_F113.event ~= nil) end)
udg_Flag = false
test(14, function() assert(udg_Flag == false); udg_T.callback(); assert(udg_Flag == true); DestroyTimer(udg_T) end)
test(15, function() assert(udg_Count == 11 and calls[#calls][1] == "ExecuteFunc" and calls[#calls][2] == "ResearchStart") end)
test(16, function() assert(udg_Flag == true and udg_U == udg_V and udg_HandleNumber == udg_U.id) end)
before = #calls
test(17, function()
    assert(calls[before + 1][1] == "GetRandomInt" and calls[before + 2][1] == "GetRandomReal" and #calls == before + 2)
    assert(udg_Roll == 4 and udg_Chance == 0.125)
end)
udg_Count, udg_Flag = -1, false
RunInitializationTriggers(); assert(udg_Flag == false)
udg_Count = 0; RunInitializationTriggers(); assert(udg_Flag == true)
test(18, function() assert(evaluated == 4) end)
assert(udg_Values[-1] == 0 and udg_Values[8192] == 0, "ordinary out-of-range read must return typed zero")
udg_Count, udg_Quotient = 9, 4
Trig_F121_Actions(); assert(udg_Value == 2.25, "real operator must promote integer operands")
before = #calls
Trig_F122_Actions(); assert(udg_Quotient == 2 and #calls == before + 2
    and calls[before + 1][2] == 1 and calls[before + 2][2] == 2, "operands must run once in order")
Trig_F123_Actions(); assert(udg_Label == "\"\\中文\r\n\0019", "literal bytes changed")
for _, sample in ipairs({{1, 0, "LUA_INTEGER_DIVIDE_ZERO"}, {-2147483648, -1, "LUA_INTEGER_OVERFLOW"}}) do
    udg_Count, udg_Quotient = sample[1], sample[2]
    ok, errorText = pcall(Trig_F124_Actions)
    assert(not ok and errorText:find(sample[3], 1, true))
end
udg_Count, udg_Flag = 0, false; Trig_F125_Actions(); assert(udg_Count == 0 and udg_Flag == false)
udg_Count = 1; Trig_F125_Actions(); assert(udg_Count == 20 and udg_Flag == true)
udg_Flag = false; Trig_F126_Actions(); assert(udg_Flag == false)
udg_T.callback(); assert(udg_Flag == true); DestroyTimer(udg_T)
assert(passed == 18)
print("18/18 original stub samples passed; native/game/multiplayer not validated")

local semanticCases = 0
local function semantic(name, body)
    body(); semanticCases = semanticCases + 1
    print("classic semantic sample passed: " .. name)
end
semantic("i32 arithmetic wraps at each boundary", function()
    for _, sample in ipairs({
        {127, 2147483647, 1, -2147483648}, {127, -2147483648, -1, 2147483647},
        {128, -2147483648, 1, 2147483647}, {128, 2147483647, -1, -2147483648},
        {129, 2147483647, 2147483647, 1}, {129, -2147483648, -1, -2147483648},
        {129, -2147483648, 2, 0}, {129, 65536, 65536, 0}
    }) do
        udg_Count, udg_Quotient = sample[2], sample[3]
        _G["Trig_F" .. sample[1] .. "_Actions"]()
        assert(udg_Count == sample[4] and math.type(udg_Count) == "integer")
    end
    udg_Count, udg_Quotient = 2147483647.0, 2147483647.0
    Trig_F129_Actions(); assert(udg_Count == 1, "integer inputs were multiplied as imprecise doubles")
    udg_Count = -2147483648; Trig_F130_Actions(); assert(udg_Count == -2147483648)
    udg_Count = 2147483647; Trig_F131_Actions()
    assert(udg_Count == -1073741824, "normalization happened after division instead of at inner addition")
end)
semantic("ordinary division truncates toward zero", function()
    for _, sample in ipairs({{-9, 4, -2}, {9, -4, -2}, {-9, -4, 2}, {-1, 4, 0},
        {1, -4, 0}, {-2147483648, 1, -2147483648}, {2147483647, -1, -2147483647}}) do
        udg_Count, udg_Quotient = sample[1], sample[2]
        Trig_F124_Actions(); assert(udg_Quotient == sample[3] and math.type(udg_Quotient) == "integer")
    end
end)
semantic("null concatenation preserves null versus empty text", function()
    udg_Label = "text"; Trig_F132_Actions(); assert(udg_Label == "text")
    Trig_F133_Actions(); assert(udg_Label == "text")
    Trig_F134_Actions(); assert(udg_Label == nil)
    Trig_F135_Actions(); assert(udg_Label == "")
    udg_Label = nil; Trig_F132_Actions(); assert(udg_Label == nil)
    Trig_F133_Actions(); assert(udg_Label == nil)
end)
semantic("concatenation evaluates both operands once in order", function()
    stringOrder, stringResults = {}, {right = "right-only"}
    Trig_F140_Actions()
    assert(udg_Label == "right-only" and #stringOrder == 2 and stringOrder[1] == "left" and stringOrder[2] == "right")
    stringOrder, stringResults = {}, {}
    Trig_F140_Actions(); assert(udg_Label == nil and #stringOrder == 2)
    stringOrder, stringResults = {}, {left = "", right = ""}
    Trig_F140_Actions(); assert(udg_Label == "" and #stringOrder == 2)
end)
semantic("typed out-of-range reads", function()
    for _, index in ipairs({-2147483648, -2, -1, 8192, 2147483647}) do
        udg_I = index; Trig_F137_Actions(); assert(udg_Count == 0)
        Trig_F139_Actions()
        assert(udg_Label == nil and udg_Value == 0 and udg_Flag == false and udg_Groups[index] == nil)
    end
    udg_Labels[8191] = ""; udg_I = 8191; Trig_F139_Actions(); assert(udg_Label == "")
    udg_Labels[8191] = nil; Trig_F139_Actions(); assert(udg_Label == nil)
end)
semantic("array index is evaluated once and frozen before RHS", function()
    assignmentOrder = {}; udg_I, udg_Count = 3, 77
    Trig_F136_Actions()
    assert(#assignmentOrder == 2 and assignmentOrder[1] == "index" and assignmentOrder[2] == "value")
    assert(udg_I == 4 and udg_Values[3] == 77 and udg_Values[4] == 0)
    assignmentOrder = {}; udg_I, udg_Count = 8191, 79
    Trig_F138_Actions(); assert(udg_Values[8191] == 79 and udg_I == 8192 and #assignmentOrder == 1)
end)
semantic("out-of-range write runs RHS but leaves elements unchanged", function()
    udg_Values[0], udg_Values[8191] = 13, 17
    for _, index in ipairs({-2147483648, -2, -1, 8192, 2147483647}) do
        assignmentOrder = {}; udg_I, udg_Count = index, 91
        Trig_F138_Actions()
        assert(#assignmentOrder == 1 and assignmentOrder[1] == "value" and udg_I == index + 1)
        assert(udg_Values[index] == 0 and udg_Values[0] == 13 and udg_Values[8191] == 17)
        udg_Checks[index], udg_Reals[index], udg_Labels[index], udg_Groups[index] = true, 2.5, "text", living
        assert(udg_Checks[index] == false and udg_Reals[index] == 0 and udg_Labels[index] == nil and udg_Groups[index] == nil)
    end
end)
print(string.format("%d classic semantic groups passed; stock VM evidence is static, natives are stubs", semanticCases))
