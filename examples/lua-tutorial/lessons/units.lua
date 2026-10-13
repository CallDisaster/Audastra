-- 第 2 课：创建单位
-- 物体的四字码可以写成 |hfoo|（YDWE 写法），等同于 JASS 里的 'hfoo'。
-- 字符串里的 "|hfoo|" 不会被改动。

local jass = require "jass.common"

local M = {}

local footman = |hfoo|

function M.spawn(player)
    -- 在玩家开始点附近创建一个步兵，面向 270 度。
    local start = jass.GetPlayerStartLocation(player)
    local x = jass.GetStartLocationX(start)
    local y = jass.GetStartLocationY(start)
    local unit = jass.CreateUnit(player, footman, x + 128, y, 270)
    jass.DisplayTimedTextToPlayer(player, 0, 0, 10, "创建了 " .. jass.GetUnitName(unit) .. "（四字码 |hfoo| = " .. footman .. "）")
    return unit
end

return M
