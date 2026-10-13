-- 第 3 课：计时器
-- Lua 里不能用“等待”（TriggerSleepAction），需要延时请用计时器。
-- TimerStart 的最后一个参数直接传 Lua 函数。

local jass = require "jass.common"

local M = {}

function M.start(player)
    local count = 3
    local timer = jass.CreateTimer()
    jass.TimerStart(timer, 1.0, true, function()
        if count > 0 then
            jass.DisplayTimedTextToPlayer(player, 0, 0, 2, "倒计时 " .. count)
            count = count - 1
        else
            jass.DisplayTimedTextToPlayer(player, 0, 0, 5, "计时结束！")
            jass.DestroyTimer(timer)
        end
    end)
end

return M
