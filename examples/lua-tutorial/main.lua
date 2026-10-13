-- Audastra Lua 教程地图 · 入口文件
--
-- 在编辑器“触发器 → Lua 脚本”里编辑这些文件，按 F9 测试地图。
--
-- 这张地图怎么加载 Lua：
--   触发器“Lua 入口”在地图初始化时执行一行自定义代码
--       call Cheat("exec-lua: \"main\"")
--   它会加载地图根目录的 main.lua（就是本文件）。这是 YDWE 的约定，YDWE 做的地图也这样写。
--
-- 怎么调用游戏函数：
--   local jass = require "jass.common"   -- common.j 里的全部 native，例如 jass.CreateUnit
--   注意：blizzard.j 里的 BJ 函数（如 BJDebugMsg、CreateNUnitsAtLoc）在 Lua 里不能直接用，
--   请改用对应的 native。
--
-- 子目录里的文件用点号引用：lessons\hello.lua 写作 require "lessons.hello"。
--
-- 进游戏后在聊天框输入下面的命令看各课效果：
--   -hello   第 1 课：显示文字
--   -unit    第 2 课：创建单位（四字码写法 |hfoo|）
--   -timer   第 3 课：计时器
--   -hi      第 4 课：热重载演示（改 lessons\reload.lua 里的文字，再点编辑器的“热重载”）

local jass = require "jass.common"

local hello = require "lessons.hello"
local units = require "lessons.units"
local timers = require "lessons.timers"
local reload = require "lessons.reload"

-- 第 0 课：注册聊天命令。Lua 函数可以直接作为触发器动作（code 参数）。
local commands = {
    ["-hello"] = function(player) hello.show(player) end,
    ["-unit"] = function(player) units.spawn(player) end,
    ["-timer"] = function(player) timers.start(player) end,
    -- 通过模块表调用：热重载替换 reload.greeting 后，这里会自动用上新函数。
    ["-hi"] = function(player) reload.greeting(player) end,
}

local trigger = jass.CreateTrigger()
for i = 0, 11 do
    for text in pairs(commands) do
        jass.TriggerRegisterPlayerChatEvent(trigger, jass.Player(i), text, true)
    end
end
jass.TriggerAddAction(trigger, function()
    local handler = commands[jass.GetEventPlayerChatString()]
    if handler then handler(jass.GetTriggerPlayer()) end
end)

-- print 的内容会显示在编辑器底部的问题/输出里，方便调试。
print("Lua 教程地图已加载：输入 -hello、-unit、-timer、-hi 试试。")
jass.DisplayTimedTextToPlayer(jass.Player(0), 0, 0, 30,
    "|cff7fe0c0Lua 教程地图已加载|r：输入 -hello、-unit、-timer、-hi 试试。")
