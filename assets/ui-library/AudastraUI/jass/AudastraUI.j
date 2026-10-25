// AudastraUI 函数库的脚本部分，地图用到 AudastraUI.cfg 里的函数时注入。
// 经 Audastra JAPI（打包时随本文件一起注入的 AudastraJapi 库）调用游戏里的内置界面模块 audastraui。
// 没装运行库、游戏版本不受支持或运行库未接通时 AudastraUIIsReady 为假，框架相关的调用什么都不做，
// 取值返回 0 或空文字，AudastraUILastError 说明原因；生成的“创建”触发器按 0.2 秒轮询，最多 150 次，地图照常运行。
// 界面事件只在本机发生（每个玩家的鼠标不同），事件触发器只在操作的玩家电脑上运行；会影响游戏的动作要先同步。
// 文字格式化是纯脚本实现，位数规则同编辑器预览（最多 3 位、去掉末尾的 0）。
library AudastraUI requires AudastraJapi

    globals
        private constant string PREFIX = "audastraui."
        private constant real POLL_PERIOD = 0.02
        private constant integer MAX_ENTRIES = 8190
        // -1 未连接过；0 已连接；其他仅记录上次失败的 JAPI 状态，不阻止重试。
        private integer connectStatus = -1
        private integer failedStatus = 0
        private integer failedRequest = 0
        private timer pollTimer = null
        // 监听表：框架 + 事件类型 → 事件名、序号（同一框架同一类型只留最后一次）。
        private integer watchCount = 0
        private integer array watchFrame
        private integer array watchKind
        private string array watchKey
        private integer array watchIndex
        // 登记表：事件名 → 触发器。
        private integer eventCount = 0
        private trigger array eventTrigger
        private string array eventKey
        private integer triggerFrame = 0
        private integer triggerIndex = 0
        private integer triggerKind = 0
        private string triggerKey = ""
    endglobals

    // ── 运行库连接 ──

    // 只缓存成功；失败后下次调用重新尝试，重试间隔与期限由调用方控制。
    private function Connect takes nothing returns boolean
        if connectStatus != 0 then
            if AjInitialize() and AjModuleLoad("audastraui") then
                set connectStatus = 0
            else
                set connectStatus = AjLastStatus
            endif
        endif
        return connectStatus == 0
    endfunction

    // 记下失败调用的请求号，AudastraUILastError 再向模块要原因。
    private function Check takes nothing returns boolean
        if AjLastStatus == 0 then
            return true
        endif
        set failedStatus = AjLastStatus
        set failedRequest = AjNextRequest
        return false
    endfunction

    // 脚本这边就能判定的错误（参数不对、表已满），没有对应的模块请求。
    private function Refuse takes integer status returns nothing
        set failedStatus = status
        set failedRequest = 0
    endfunction

    private function Run takes string command, string arguments returns boolean
        if not Connect() then
            return false
        endif
        call AjCallVoid(PREFIX + command, arguments)
        return Check()
    endfunction

    private function Frame takes string command, string arguments returns integer
        local integer frame
        if not Connect() then
            return 0
        endif
        set frame = AjCallHandle(PREFIX + command, arguments)
        if Check() then
            return frame
        endif
        return 0
    endfunction

    // ── 运行库状态 ──

    function AudastraUIIsReady takes nothing returns boolean
        local boolean ready
        if not Connect() then
            return false
        endif
        set ready = AjCallBool(PREFIX + "ready", "")
        if not Check() then
            return false
        endif
        if not ready then
            // 模块已记下不可用的原因（入口不完整、游戏界面尚未创建等）。
            set failedStatus = 8
            set failedRequest = AjNextRequest
        endif
        return ready
    endfunction

    function AudastraUILastError takes nothing returns string
        local string text
        // 读取连接错误不能触发重试，否则刚发生的失败可能被恢复成功覆盖。
        if connectStatus != 0 then
            return "AudastraUI 运行库未连接（JAPI 状态 " + I2S(connectStatus) + "）"
        endif
        if failedStatus == 0 then
            return ""
        endif
        set text = AjCallText(PREFIX + "error", AjArgInt(failedRequest))
        if AjLastStatus == 0 and text != "" then
            return text
        endif
        return "AudastraUI 调用失败（JAPI 状态 " + I2S(failedStatus) + "）"
    endfunction

    // ── 取框架与创建 ──

    function AudastraUIGameUI takes nothing returns integer
        return Frame("root", "")
    endfunction

    function AudastraUIWorldFrame takes nothing returns integer
        return Frame("world", "")
    endfunction

    function AudastraUIFindFrame takes string name, integer context returns integer
        return Frame("find", AjArgText(name) + AjArgInt(context))
    endfunction

    function AudastraUICreateFrame takes string templateName, integer parent, integer level, integer context returns integer
        return Frame("create", AjArgText(templateName) + AjArgHandle(parent) + AjArgInt(level) + AjArgInt(context))
    endfunction

    function AudastraUICreateSimpleFrame takes string templateName, integer parent, integer context returns integer
        return Frame("createsimple", AjArgText(templateName) + AjArgHandle(parent) + AjArgInt(context))
    endfunction

    // ── 文字格式化 ──

    // decimals 为 -1 时最多保留 3 位小数并去掉小数点后末尾的 0。
    function AudastraUIFormatReal takes real value, integer decimals returns string
        local string text
        local integer length
        local integer dot = 0
        if decimals >= 0 then
            return R2SW(value, 1, decimals)
        endif
        set text = R2SW(value, 1, 3)
        set length = StringLength(text)
        loop
            exitwhen dot >= length or SubString(text, dot, dot + 1) == "."
            set dot = dot + 1
        endloop
        if dot >= length then
            return text
        endif
        loop
            exitwhen SubString(text, length - 1, length) != "0"
            set length = length - 1
        endloop
        if length - 1 == dot then
            set length = dot
        endif
        set text = SubString(text, 0, length)
        if text == "-0" then
            return "0"
        endif
        return text
    endfunction

    function AudastraUIBooleanText takes boolean value returns string
        if value then
            return "true"
        endif
        return "false"
    endfunction

    // ── 界面事件 ──
    // 模块记录鼠标进入、离开、点击；这里用计时器取回，按监听表找到事件名，再运行登记了这个事件名的触发器。

    function AudastraUIGetTriggerFrame takes nothing returns integer
        return triggerFrame
    endfunction

    function AudastraUIGetTriggerIndex takes nothing returns integer
        return triggerIndex
    endfunction

    function AudastraUIGetTriggerKind takes nothing returns integer
        return triggerKind
    endfunction

    function AudastraUIGetTriggerKey takes nothing returns string
        return triggerKey
    endfunction

    function AudastraUITriggerRegisterEvent takes trigger whichTrigger, string key returns nothing
        local integer i = 0
        if whichTrigger == null or key == "" then
            return
        endif
        loop
            exitwhen i >= eventCount
            if eventTrigger[i] == whichTrigger and eventKey[i] == key then
                return
            endif
            set i = i + 1
        endloop
        if eventCount >= MAX_ENTRIES then
            call Refuse(3)
            return
        endif
        set eventTrigger[eventCount] = whichTrigger
        set eventKey[eventCount] = key
        set eventCount = eventCount + 1
    endfunction

    // 参数先拷到局部：触发器动作里删框架或改监听会重排监听表。
    private function Fire takes integer frame, integer kind, string key, integer index returns nothing
        local integer i = 0
        loop
            exitwhen i >= eventCount
            if eventKey[i] == key and IsTriggerEnabled(eventTrigger[i]) then
                set triggerFrame = frame
                set triggerIndex = index
                set triggerKind = kind
                set triggerKey = key
                if TriggerEvaluate(eventTrigger[i]) then
                    call TriggerExecute(eventTrigger[i])
                endif
            endif
            set i = i + 1
        endloop
    endfunction

    private function Dispatch takes integer frame, integer kind returns nothing
        local integer slot = 0
        loop
            exitwhen slot >= watchCount
            if watchFrame[slot] == frame and watchKind[slot] == kind then
                call Fire(frame, kind, watchKey[slot], watchIndex[slot])
                return
            endif
            set slot = slot + 1
        endloop
    endfunction

    private function Poll takes nothing returns nothing
        local integer pending = AjCallInt(PREFIX + "poll", "")
        local string eventText
        local integer bar
        if not Check() then
            // 运行库停用或游戏界面已不在：停止轮询，下次监听时重新开始。
            call PauseTimer(pollTimer)
            return
        endif
        loop
            exitwhen pending <= 0
            set eventText = AjCallText(PREFIX + "event", "")
            exitwhen not Check() or eventText == ""
            set bar = AjFind(eventText, "|", 0)
            exitwhen bar <= 0
            call Dispatch(S2I(SubString(eventText, 0, bar)), S2I(SubString(eventText, bar + 1, StringLength(eventText))))
            set pending = pending - 1
        endloop
    endfunction

    // 同一框架各事件类型合成的掩码：1 进入、2 离开、4 点击。
    private function MaskOf takes integer frame returns integer
        local integer slot = 0
        local integer mask = 0
        loop
            exitwhen slot >= watchCount
            if watchFrame[slot] == frame then
                set mask = mask + watchKind[slot]
            endif
            set slot = slot + 1
        endloop
        return mask
    endfunction

    private function ForgetFrame takes integer frame returns nothing
        local integer slot = 0
        local integer kept = 0
        loop
            exitwhen slot >= watchCount
            if watchFrame[slot] != frame then
                set watchFrame[kept] = watchFrame[slot]
                set watchKind[kept] = watchKind[slot]
                set watchKey[kept] = watchKey[slot]
                set watchIndex[kept] = watchIndex[slot]
                set kept = kept + 1
            endif
            set slot = slot + 1
        endloop
        set watchCount = kept
    endfunction

    function AudastraUIWatchFrame takes integer frame, integer kind, string key, integer index returns nothing
        local integer slot = 0
        local integer mask
        if frame == 0 or key == "" or (kind != 1 and kind != 2 and kind != 4) then
            call Refuse(5)
            return
        endif
        if not Connect() then
            return
        endif
        loop
            exitwhen slot >= watchCount or (watchFrame[slot] == frame and watchKind[slot] == kind)
            set slot = slot + 1
        endloop
        set mask = MaskOf(frame)
        if slot >= watchCount then
            if watchCount >= MAX_ENTRIES then
                call Refuse(3)
                return
            endif
            set mask = mask + kind
        endif
        if not Run("watch", AjArgHandle(frame) + AjArgInt(mask)) then
            return
        endif
        if slot >= watchCount then
            set watchCount = watchCount + 1
        endif
        set watchFrame[slot] = frame
        set watchKind[slot] = kind
        set watchKey[slot] = key
        set watchIndex[slot] = index
        if pollTimer == null then
            set pollTimer = CreateTimer()
        endif
        call TimerStart(pollTimer, POLL_PERIOD, true, function Poll)
    endfunction

    // ── 设置框架 ──

    function AudastraUILoadToc takes string path returns nothing
        call Run("loadtoc", AjArgText(path))
    endfunction

    function AudastraUISetPoint takes integer frame, integer point, integer target, integer targetPoint, real x, real y returns nothing
        call Run("setpoint", AjArgHandle(frame) + AjArgInt(point) + AjArgHandle(target) + AjArgInt(targetPoint) + AjArgReal(x) + AjArgReal(y))
    endfunction

    function AudastraUISetAbsPoint takes integer frame, integer point, real x, real y returns nothing
        call Run("setabspoint", AjArgHandle(frame) + AjArgInt(point) + AjArgReal(x) + AjArgReal(y))
    endfunction

    function AudastraUISetAllPoints takes integer frame, integer target returns nothing
        call Run("setallpoints", AjArgHandle(frame) + AjArgHandle(target))
    endfunction

    function AudastraUIClearPoints takes integer frame returns nothing
        call Run("clearpoints", AjArgHandle(frame))
    endfunction

    function AudastraUISetSize takes integer frame, real width, real height returns nothing
        call Run("setsize", AjArgHandle(frame) + AjArgReal(width) + AjArgReal(height))
    endfunction

    function AudastraUISetWidth takes integer frame, real width returns nothing
        call Run("setwidth", AjArgHandle(frame) + AjArgReal(width))
    endfunction

    function AudastraUISetHeight takes integer frame, real height returns nothing
        call Run("setheight", AjArgHandle(frame) + AjArgReal(height))
    endfunction

    function AudastraUISetScale takes integer frame, real scale returns nothing
        call Run("setscale", AjArgHandle(frame) + AjArgReal(scale))
    endfunction

    function AudastraUIShow takes integer frame, boolean show returns nothing
        call Run("show", AjArgHandle(frame) + AjArgBool(show))
    endfunction

    function AudastraUISetAlpha takes integer frame, integer alpha returns nothing
        call Run("setalpha", AjArgHandle(frame) + AjArgInt(alpha))
    endfunction

    function AudastraUISetLevel takes integer frame, integer level returns nothing
        call Run("setlevel", AjArgHandle(frame) + AjArgInt(level))
    endfunction

    function AudastraUISetParent takes integer frame, integer parent returns nothing
        call Run("setparent", AjArgHandle(frame) + AjArgHandle(parent))
    endfunction

    function AudastraUISetText takes integer frame, string text returns nothing
        call Run("settext", AjArgHandle(frame) + AjArgText(text))
    endfunction

    function AudastraUISetFont takes integer frame, string font, real height, integer flags returns nothing
        call Run("setfont", AjArgHandle(frame) + AjArgText(font) + AjArgReal(height) + AjArgInt(flags))
    endfunction

    function AudastraUISetTextAlign takes integer frame, integer horizontal, integer vertical returns nothing
        call Run("settextalign", AjArgHandle(frame) + AjArgInt(horizontal) + AjArgInt(vertical))
    endfunction

    function AudastraUISetColor takes integer frame, integer red, integer green, integer blue, integer alpha returns nothing
        call Run("setcolor", AjArgHandle(frame) + AjArgInt(red) + AjArgInt(green) + AjArgInt(blue) + AjArgInt(alpha))
    endfunction

    function AudastraUISetTexture takes integer frame, string path, boolean tile returns nothing
        call Run("settexture", AjArgHandle(frame) + AjArgText(path) + AjArgBool(tile))
    endfunction

    function AudastraUISetValue takes integer frame, real value returns nothing
        call Run("setvalue", AjArgHandle(frame) + AjArgReal(value))
    endfunction

    function AudastraUISetLimits takes integer frame, real minimum, real maximum returns nothing
        call Run("setlimits", AjArgHandle(frame) + AjArgReal(minimum) + AjArgReal(maximum))
    endfunction

    function AudastraUISetModel takes integer frame, string path, integer kind returns nothing
        call Run("setmodel", AjArgHandle(frame) + AjArgText(path) + AjArgInt(kind))
    endfunction

    function AudastraUISetEnabled takes integer frame, boolean enabled returns nothing
        call Run("setenabled", AjArgHandle(frame) + AjArgBool(enabled))
    endfunction

    function AudastraUIDestroy takes integer frame returns nothing
        if Run("destroy", AjArgHandle(frame)) then
            call ForgetFrame(frame)
        endif
    endfunction

endlibrary
