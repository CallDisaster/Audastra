-- 第 1 课：显示文字
-- 一个 Lua 文件就是一个模块：返回一张表，别的文件用 require 拿到它。

local jass = require "jass.common"

local M = {}

function M.show(player)
    -- GetPlayerName 返回字符串，Lua 用 .. 拼接文本。
    local name = jass.GetPlayerName(player)
    jass.DisplayTimedTextToPlayer(player, 0, 0, 10, "你好，" .. name .. "！这行字来自 lessons\\hello.lua。")
end

return M
