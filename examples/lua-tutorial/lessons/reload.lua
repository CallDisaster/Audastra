-- 第 4 课：热重载
-- 1. 按 F9 测试地图，进游戏后输入 -hi，看到下面的文字。
-- 2. 不关游戏，回到编辑器：在 Lua 脚本页修改 M.text 里引号中的文字，点“应用”。
-- 3. 点工具栏的“热重载”，再在游戏里输入 -hi，文字已经变了。
--
-- 热重载会重新执行本文件，用新函数替换旧模块表里的函数；
-- 表里的数据（例如 M.count）保持原值，所以计数不会清零。
-- 本文件顶层如果注册了触发器，重新执行时会再注册一次，所以注册写在 main.lua 里。

local jass = require "jass.common"

local M = { count = 0 }

function M.text()
    return "改我试试！"
end

function M.greeting(player)
    M.count = M.count + 1
    jass.DisplayTimedTextToPlayer(player, 0, 0, 10, "第 " .. M.count .. " 次：" .. M.text())
end

return M
