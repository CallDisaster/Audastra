// AudastraUI 函数库的脚本部分，地图用到 AudastraUI.cfg 里的函数时注入。
// 经 Audastra JAPI（打包时随本文件一起注入的 AudastraJapi 库）调用游戏里的内置界面模块 audastraui。
// 没装运行库、游戏版本不受支持或运行库未接通时 AudastraUIIsReady 为假，框架相关的调用什么都不做，
// 取值返回 0 或空文字，AudastraUILastError 说明原因；生成的“创建”触发器按 0.2 秒轮询，最多 150 次，地图照常运行。
// 界面事件只在本机发生（每个玩家的鼠标不同），事件触发器只在操作的玩家电脑上运行；会影响游戏的动作要先同步。
// 数值文字的位数规则同编辑器预览与运行库（最多 3 位、去掉末尾的 0；固定位数半值远离 0；负零写成 0）。
library AudastraUI requires AudastraJapi

    globals
        private constant string PREFIX = "audastraui."
        private constant real POLL_PERIOD = 0.02
        private constant integer MAX_ENTRIES = 8190
        // 每次计时器到期最多派发的界面事件数，剩下的留到下一次。
        private constant integer EVENTS_PER_POLL = 32
        // 数值文字格式里最多的片段数（文字与取值交替）。
        private constant integer MAX_FORMAT_PIECES = 512
        // -1 未连接过；0 已连接；其他仅记录上次失败的 JAPI 状态，不阻止重试。
        private integer connectStatus = -1
        private integer failedStatus = 0
        private integer failedRequest = 0
        // 界面数据面：高频刷新写进这张表，运行库在游戏线程里直接改框架，不拼请求串。
        // -1 还没绑定；0 已绑定；其他为上次绑定失败的 JAPI 状态。
        private hashtable dataTable = null
        private integer dataStatus = -1
        // 整数事件通道：-1 还没协商；0 可用；其他为协商失败的 JAPI 状态，本局用字符串取事件。
        private integer eventStatus = -1
        // 运行库数值文字格式：-1 还没试；0 可用；6 旧版运行库不认识、12 不支持，本局都在脚本里拼文字。
        private integer formatStatus = -1
        // 数值文字格式：parent 为框架。0 方式（1 运行库、2 脚本拼接）、1 片段数；16+k 实数实参、24+k 整数实参、
        // 40+k 实参类型（1 实数、2 整数）、50+k 格式用到实参 k；1000+i 片段的实参（-1 为文字）、2000+i 小数位、3000+i 文字。
        private hashtable formatTable = null
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

    // 第一次推送时绑定数据面。不在游戏线程（9）、重入（11）下次再试；其他失败（旧版运行库未知命令 6、不支持 12、
    // 表无效 5、已绑别的表 8 等）本局一直走字符串请求，不在每次刷新时重试。
    private function Bound takes nothing returns boolean
        if dataStatus == -1 or dataStatus == 9 or dataStatus == 11 then
            if not Connect() then
                return false
            endif
            if dataTable == null then
                set dataTable = InitHashtable()
            endif
            call AjCallVoid(PREFIX + "bindtable", AjArgInt(GetHandleId(dataTable)))
            set dataStatus = AjLastStatus
        endif
        return dataStatus == 0
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

    // 数据面刷新的累计错误：运行库只计数并记最近原因（“计数|原因”），不逐次报错。没走数据面或没出错时为空。
    function AudastraUIDataErrors takes nothing returns string
        local string text
        local integer length
        local integer i = 0
        if dataStatus != 0 then
            return ""
        endif
        set text = AjCallText(PREFIX + "dataerrors", "")
        if AjLastStatus != 0 then
            return ""
        endif
        set length = StringLength(text)
        loop
            exitwhen i >= length or SubString(text, i, i + 1) == "|"
            set i = i + 1
        endloop
        if i == 0 or i >= length then
            return "AudastraUI 数据面错误无法识别：" + text
        endif
        if SubString(text, 0, i) == "0" then
            return ""
        endif
        if i + 1 == length then
            return "AudastraUI 数据面刷新失败 " + SubString(text, 0, i) + " 次"
        endif
        return "AudastraUI 数据面刷新失败 " + SubString(text, 0, i) + " 次，最近一次：" + SubString(text, i + 1, length)
    endfunction

    function AudastraUILastError takes nothing returns string
        local string text
        // 读取连接错误不能触发重试，否则刚发生的失败可能被恢复成功覆盖。
        if connectStatus != 0 then
            return "AudastraUI 运行库未连接（JAPI 状态 " + I2S(connectStatus) + "）"
        endif
        if failedStatus == 0 then
            return AudastraUIDataErrors()
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

    // “-0”“-0.00”这类负零写成正零。
    private function PositiveZero takes string text returns string
        local integer length = StringLength(text)
        local integer i = 1
        local string c
        if SubString(text, 0, 1) != "-" then
            return text
        endif
        loop
            exitwhen i >= length
            set c = SubString(text, i, i + 1)
            if c != "0" and c != "." then
                return text
            endif
            set i = i + 1
        endloop
        return SubString(text, 1, length)
    endfunction

    // decimals 为 -1 时最多保留 3 位小数并去掉小数点后末尾的 0。
    function AudastraUIFormatReal takes real value, integer decimals returns string
        local string text
        local integer length
        local integer dot = 0
        if decimals >= 0 then
            set text = R2SW(value, 1, decimals)
            set length = StringLength(text)
            // 0 位时 R2SW 仍带小数点（“12.” 或 “12.0”），去掉。
            if decimals == 0 then
                loop
                    exitwhen dot >= length or SubString(text, dot, dot + 1) == "."
                    set dot = dot + 1
                endloop
                set text = SubString(text, 0, dot)
            endif
            return PositiveZero(text)
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

    // decimals 大于 0 时在整数后补相应位数的 0。
    function AudastraUIFormatInteger takes integer value, integer decimals returns string
        local string text = I2S(value)
        local integer i = 0
        if decimals > 0 then
            set text = text + "."
            loop
                exitwhen i >= decimals
                set text = text + "0"
                set i = i + 1
            endloop
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

    // 界面模拟计时器到期：只在编辑器的界面模拟里发生，编辑器执行到这里时记下订阅；游戏里什么都不做。
    function AudastraUIRegisterSimulationTimer takes trigger whichTrigger, string description, string timerId returns nothing
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

    // 整数事件通道：事件从绑定表 parent 0 取回，不再每次计时器到期拼请求串。旧版运行库不认识时用字符串取事件，
    // 两种通道共用同一队列，只用其中一种。不在游戏线程（9）、重入（11）下次再试。
    private function EventTable takes nothing returns boolean
        if eventStatus == -1 or eventStatus == 9 or eventStatus == 11 then
            if not Bound() then
                return false
            endif
            call AjCallVoid(PREFIX + "eventtable", "")
            set eventStatus = AjLastStatus
        endif
        return eventStatus == 0
    endfunction

    private function Poll takes nothing returns nothing
        local integer pending
        local string eventText
        local integer bar
        local integer count = 0
        if EventTable() then
            // 7 泵一次输入并取第一个事件，8 取下一个；完成标记 child 0 为 1 时 child 1 为框架、child 2 为事件类型。
            call FlushChildHashtable(dataTable, 0)
            call SaveBoolean(dataTable, 0, 7, true)
            loop
                exitwhen LoadInteger(dataTable, 0, 0) != 1
                call Dispatch(LoadInteger(dataTable, 0, 1), LoadInteger(dataTable, 0, 2))
                set count = count + 1
                exitwhen count >= EVENTS_PER_POLL
                call FlushChildHashtable(dataTable, 0)
                call SaveBoolean(dataTable, 0, 8, true)
            endloop
            return
        endif
        set pending = AjCallInt(PREFIX + "poll", "")
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

    // 路径最后一段没有扩展名时补上，与编辑器预览一致：游戏不会自己补，找不到文件的贴图会画成绿块。
    private function WithExtension takes string path, string extension returns string
        local integer i = StringLength(path)
        local string c
        if i == 0 then
            return path
        endif
        loop
            exitwhen i == 0
            set c = SubString(path, i - 1, i)
            if c == "." then
                return path
            endif
            exitwhen c == "\\" or c == "/"
            set i = i - 1
        endloop
        return path + extension
    endfunction

    function AudastraUISetTexture takes integer frame, string path, boolean tile returns nothing
        call Run("settexture", AjArgHandle(frame) + AjArgText(WithExtension(path, ".blp")) + AjArgBool(tile))
    endfunction

    function AudastraUISetValue takes integer frame, real value returns nothing
        call Run("setvalue", AjArgHandle(frame) + AjArgReal(value))
    endfunction

    function AudastraUISetLimits takes integer frame, real minimum, real maximum returns nothing
        call Run("setlimits", AjArgHandle(frame) + AjArgReal(minimum) + AjArgReal(maximum))
    endfunction

    function AudastraUISetModel takes integer frame, string path, integer kind returns nothing
        call Run("setmodel", AjArgHandle(frame) + AjArgText(WithExtension(path, ".mdx")) + AjArgInt(kind))
    endfunction

    function AudastraUISetEnabled takes integer frame, boolean enabled returns nothing
        call Run("setenabled", AjArgHandle(frame) + AjArgBool(enabled))
    endfunction

    function AudastraUIDestroy takes integer frame returns nothing
        if Run("destroy", AjArgHandle(frame)) then
            call ForgetFrame(frame)
            if formatTable != null then
                call FlushChildHashtable(formatTable, frame)
            endif
        endif
    endfunction

    // ── 高频刷新 ──

    // 每帧都可能变的值（血条宽度、数字、增减益图标、显示）用这一组：运行库支持数据面时只写一次表，
    // 由运行库在游戏线程里按同样的语义改框架；不支持时同对应的设置函数。走数据面时单次写入的错误不记进
    // AudastraUILastError。
    function AudastraUIPushValue takes integer frame, real value returns nothing
        if Bound() then
            call SaveReal(dataTable, frame, 1, value)
        else
            call AudastraUISetValue(frame, value)
        endif
    endfunction

    function AudastraUIPushWidth takes integer frame, real width returns nothing
        if Bound() then
            call SaveReal(dataTable, frame, 2, width)
        else
            call AudastraUISetWidth(frame, width)
        endif
    endfunction

    function AudastraUIPushText takes integer frame, string text returns nothing
        if Bound() then
            call SaveStr(dataTable, frame, 3, text)
        else
            call AudastraUISetText(frame, text)
        endif
    endfunction

    function AudastraUIPushTexture takes integer frame, string path returns nothing
        if Bound() then
            call SaveStr(dataTable, frame, 4, WithExtension(path, ".blp"))
        else
            call AudastraUISetTexture(frame, path, false)
        endif
    endfunction

    function AudastraUIPushShown takes integer frame, boolean show returns nothing
        if Bound() then
            call SaveBoolean(dataTable, frame, 5, show)
        else
            call AudastraUIShow(frame, show)
        endif
    endfunction

    // ── 数值文字 ──
    // 带数值的文字先登记一次格式，之后只推数值、最后提交一次：运行库支持时由它格式化，不在脚本里拼字符串；
    // 不支持时在这里缓存格式与实参，提交时才拼成文字。格式：{0}…{7} 取实参，{k:d} 保留 d（0 到 6）位小数，
    // {{ 与 }} 为括号本身。

    // 单个数字 0..limit，不是数字时为 -1。
    private function Digit takes string c, integer limit returns integer
        local integer d = 0
        loop
            exitwhen d > limit
            if c == I2S(d) then
                return d
            endif
            set d = d + 1
        endloop
        return -1
    endfunction

    private function AddPiece takes integer frame, integer argument, integer decimals, string text returns boolean
        local integer count = LoadInteger(formatTable, frame, 1)
        if count >= MAX_FORMAT_PIECES then
            return false
        endif
        call SaveInteger(formatTable, frame, 1000 + count, argument)
        call SaveInteger(formatTable, frame, 2000 + count, decimals)
        call SaveStr(formatTable, frame, 3000 + count, text)
        call SaveInteger(formatTable, frame, 1, count + 1)
        if argument >= 0 then
            call SaveInteger(formatTable, frame, 50 + argument, 1)
        endif
        return true
    endfunction

    // 拆成文字片段与实参片段；写法不对时返回假。
    private function ParseFormat takes integer frame, string format returns boolean
        local integer length = StringLength(format)
        local integer i = 0
        local integer start = 0
        local integer argument
        local integer decimals
        local string c
        loop
            exitwhen i >= length
            set c = SubString(format, i, i + 1)
            if (c == "{" or c == "}") and SubString(format, i + 1, i + 2) == c then
                if not AddPiece(frame, -1, -1, SubString(format, start, i + 1)) then
                    return false
                endif
                set i = i + 2
                set start = i
            elseif c == "}" then
                return false
            elseif c == "{" then
                if i > start and not AddPiece(frame, -1, -1, SubString(format, start, i)) then
                    return false
                endif
                set argument = Digit(SubString(format, i + 1, i + 2), 7)
                set decimals = -1
                set i = i + 2
                if SubString(format, i, i + 1) == ":" then
                    set decimals = Digit(SubString(format, i + 1, i + 2), 6)
                    set i = i + 2
                    if decimals < 0 then
                        return false
                    endif
                endif
                if argument < 0 or SubString(format, i, i + 1) != "}" or not AddPiece(frame, argument, decimals, "") then
                    return false
                endif
                set i = i + 1
                set start = i
            else
                set i = i + 1
            endif
        endloop
        return start >= length or AddPiece(frame, -1, -1, SubString(format, start, length))
    endfunction

    // 登记格式会清掉之前推过的实参。
    function AudastraUIRegisterTextFormat takes integer frame, string format returns nothing
        if frame == 0 then
            call Refuse(5)
            return
        endif
        if formatTable == null then
            set formatTable = InitHashtable()
        endif
        call FlushChildHashtable(formatTable, frame)
        if not ParseFormat(frame, format) then
            call FlushChildHashtable(formatTable, frame)
            call Refuse(5)
            return
        endif
        if formatStatus != 6 and formatStatus != 12 and Bound() then
            // 直接试，不经 Run：旧版运行库不认识这个命令时只回到脚本拼接，不记成调用失败。
            call AjCallVoid(PREFIX + "textformat", AjArgHandle(frame) + AjArgText(format))
            if AjLastStatus == 0 then
                set formatStatus = 0
                call SaveInteger(formatTable, frame, 0, 1)
                return
            endif
            if AjLastStatus == 6 or AjLastStatus == 12 then
                set formatStatus = AjLastStatus
            endif
        endif
        call SaveInteger(formatTable, frame, 0, 2)
    endfunction

    private function FormatMode takes integer frame, integer index returns integer
        if formatTable == null or index < 0 or index > 7 then
            return 0
        endif
        return LoadInteger(formatTable, frame, 0)
    endfunction

    function AudastraUIPushTextArg takes integer frame, integer index, real value returns nothing
        local integer mode = FormatMode(frame, index)
        if mode == 1 then
            call SaveReal(dataTable, frame, 16 + index, value)
        elseif mode == 2 then
            call SaveReal(formatTable, frame, 16 + index, value)
            call SaveInteger(formatTable, frame, 40 + index, 1)
        else
            call Refuse(5)
        endif
    endfunction

    function AudastraUIPushTextIntArg takes integer frame, integer index, integer value returns nothing
        local integer mode = FormatMode(frame, index)
        if mode == 1 then
            call SaveInteger(dataTable, frame, 24 + index, value)
        elseif mode == 2 then
            call SaveInteger(formatTable, frame, 24 + index, value)
            call SaveInteger(formatTable, frame, 40 + index, 2)
        else
            call Refuse(5)
        endif
    endfunction

    // 脚本拼接：格式用到的实参都推过才写文字，否则保留原来的文字。
    private function RenderFormat takes integer frame returns nothing
        local integer count = LoadInteger(formatTable, frame, 1)
        local integer i = 0
        local integer argument
        local string text = ""
        loop
            exitwhen i > 7
            if LoadInteger(formatTable, frame, 50 + i) == 1 and LoadInteger(formatTable, frame, 40 + i) == 0 then
                call Refuse(5)
                return
            endif
            set i = i + 1
        endloop
        set i = 0
        loop
            exitwhen i >= count
            set argument = LoadInteger(formatTable, frame, 1000 + i)
            if argument < 0 then
                set text = text + LoadStr(formatTable, frame, 3000 + i)
            elseif LoadInteger(formatTable, frame, 40 + argument) == 2 then
                set text = text + AudastraUIFormatInteger(LoadInteger(formatTable, frame, 24 + argument), LoadInteger(formatTable, frame, 2000 + i))
            else
                set text = text + AudastraUIFormatReal(LoadReal(formatTable, frame, 16 + argument), LoadInteger(formatTable, frame, 2000 + i))
            endif
            set i = i + 1
        endloop
        call AudastraUIPushText(frame, text)
    endfunction

    function AudastraUICommitText takes integer frame returns nothing
        local integer mode = FormatMode(frame, 0)
        if mode == 1 then
            call SaveBoolean(dataTable, frame, 6, true)
        elseif mode == 2 then
            call RenderFormat(frame)
        else
            call Refuse(5)
        endif
    endfunction

    // ── 单位 ──

    // 点团队框架里的队员：本机正举着要选单位目标的技能或物品时，把这次点击交给游戏作为目标（等同在世界里点这个单位），
    // 由游戏校验目标、提示错误、退出选目标并下达指令。返回 0 普通选择、没处理；1 已交给游戏的单位点击处理（目标不合法时
    // 游戏同样会提示，返回值分不出来）；2 留作目标不合法，目前不会出现；3 其他输入状态（点地面、建造、菜单等）、没处理；
    // 负数为 JAPI 状态码取负（运行库不可用、旧版运行库没有这项能力等），AudastraUILastError 说明原因。
    function AudastraUIClickUnit takes unit whichUnit returns integer
        local integer result
        if whichUnit == null then
            call Refuse(5)
            return -5
        endif
        if not Connect() then
            if connectStatus > 0 then
                call Refuse(connectStatus)
                return -connectStatus
            endif
            call Refuse(8)
            return -8
        endif
        // 调用失败时 AjCallInt 返回 0，不能当成普通选择。
        set result = AjCallInt(PREFIX + "clickunit", AjArgInt(GetHandleId(whichUnit)))
        if not Check() then
            return -failedStatus
        endif
        if result < 0 or result > 3 then
            call Refuse(17)
            return -17
        endif
        return result
    endfunction

endlibrary
