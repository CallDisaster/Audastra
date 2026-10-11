-- 原创 1.27a Lua 桥接小图；只在显式手工游戏测试中执行，不运行于 CTest。
local common = require('jass.common')
local function check(value, label)
    assert(value, label)
    print('OWN_LUA_OK ' .. label)
end

check(common.S2I('127') == 127 and common.I2S(-17) == '-17', 'integer-string')
check(math.abs(common.SquareRoot(9.0) - 3.0) < 0.0001, 'real')
check(common.ConvertPlayerColor(0) ~= nil, 'zero-enum')
check(common.Player(0) == common.Player(0), 'handle-identity')
local player = common.Player(0)
local unit = common.CreateUnit(player, 0x68666f6f, 0.0, 0.0, 0.0)
check(unit ~= nil and common.GetOwningPlayer(unit) == player, 'unit-agent')
common.SetUnitX(unit, 128.0)
check(math.abs(common.GetUnitX(unit) - 128.0) < 0.001, 'unit-real')
check(type(common.GetUnitName(unit)) == 'string' and #common.GetUnitName(unit) > 0, 'native-return-string')
common.DisplayTimedTextToPlayer(player, 0.0, 0.0, 20.0, 'Audastra owned Lua native fixture')

local group = common.CreateGroup()
common.GroupAddUnit(group, unit)
local enumerated = 0
common.ForGroup(group, function()
    assert(common.GetEnumUnit() == unit, 'ForGroup context')
    enumerated = enumerated + 1
end)
check(enumerated == 1, 'forgroup-code')
common.DestroyGroup(group)

local trigger = common.CreateTrigger()
local conditions, actions = 0, 0
local condition = common.Condition(function()
    conditions = conditions + 1
    return true
end)
common.TriggerAddCondition(trigger, condition)
common.TriggerAddAction(trigger, function() actions = actions + 1 end)
check(common.TriggerEvaluate(trigger) and conditions == 1, 'condition-code')
common.TriggerExecute(trigger)
check(actions == 1, 'action-code')
common.DestroyTrigger(trigger)
common.DestroyBoolExpr(condition)

local deathTrigger = common.CreateTrigger()
common.TriggerRegisterDeathEvent(deathTrigger, unit)
local deaths = 0
common.TriggerAddAction(deathTrigger, function()
    assert(common.GetDyingUnit() == unit, 'unit death context')
    deaths = deaths + 1
end)
common.KillUnit(unit)
check(deaths == 1, 'unit-event')
common.DestroyTrigger(deathTrigger)
common.RemoveUnit(unit)

local timer = common.CreateTimer()
local ticks = 0
common.TimerStart(timer, 0.25, true, function()
    assert(common.GetExpiredTimer() == timer, 'timer context')
    ticks = ticks + 1
    if ticks == 2 then
        common.DestroyTimer(timer)
        check(true, 'timer-self-destroy')
        print('OWN_LUA_FIXTURE_COMPLETE')
    end
end)
return true
