// AudastraUI 函数库的脚本部分，地图用到 AudastraUI.cfg 里的函数时注入。
// 界面库运行时的通道还没有接入：AudastraUIIsReady 恒为假，框架相关的调用什么都不做，
// 取值返回 0 或空文字，界面事件不会发生。界面描述生成的“创建”触发器因此直接返回，地图照常运行。
// 文字格式化是纯脚本实现，位数规则同编辑器预览（最多 3 位、去掉末尾的 0）。
library AudastraUI

    globals
        private constant string NOT_CONNECTED = "AudastraUI runtime is not connected"
    endglobals

    // ── 运行库状态 ──

    function AudastraUIIsReady takes nothing returns boolean
        return false
    endfunction

    function AudastraUILastError takes nothing returns string
        return NOT_CONNECTED
    endfunction

    // ── 取框架与创建 ──

    function AudastraUIGameUI takes nothing returns integer
        return 0
    endfunction

    function AudastraUIWorldFrame takes nothing returns integer
        return 0
    endfunction

    function AudastraUIFindFrame takes string name, integer context returns integer
        return 0
    endfunction

    function AudastraUICreateFrame takes string templateName, integer parent, integer level, integer context returns integer
        return 0
    endfunction

    function AudastraUICreateSimpleFrame takes string templateName, integer parent, integer context returns integer
        return 0
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

    function AudastraUIGetTriggerFrame takes nothing returns integer
        return 0
    endfunction

    function AudastraUIGetTriggerIndex takes nothing returns integer
        return 0
    endfunction

    function AudastraUIGetTriggerKind takes nothing returns integer
        return 0
    endfunction

    function AudastraUIGetTriggerKey takes nothing returns string
        return ""
    endfunction

    function AudastraUITriggerRegisterEvent takes trigger whichTrigger, string key returns nothing
    endfunction

    function AudastraUIWatchFrame takes integer frame, integer kind, string key, integer index returns nothing
    endfunction

    // ── 设置框架 ──

    function AudastraUILoadToc takes string path returns nothing
    endfunction

    function AudastraUISetPoint takes integer frame, integer point, integer target, integer targetPoint, real x, real y returns nothing
    endfunction

    function AudastraUISetAbsPoint takes integer frame, integer point, real x, real y returns nothing
    endfunction

    function AudastraUISetAllPoints takes integer frame, integer target returns nothing
    endfunction

    function AudastraUIClearPoints takes integer frame returns nothing
    endfunction

    function AudastraUISetSize takes integer frame, real width, real height returns nothing
    endfunction

    function AudastraUISetWidth takes integer frame, real width returns nothing
    endfunction

    function AudastraUISetHeight takes integer frame, real height returns nothing
    endfunction

    function AudastraUISetScale takes integer frame, real scale returns nothing
    endfunction

    function AudastraUIShow takes integer frame, boolean show returns nothing
    endfunction

    function AudastraUISetAlpha takes integer frame, integer alpha returns nothing
    endfunction

    function AudastraUISetLevel takes integer frame, integer level returns nothing
    endfunction

    function AudastraUISetParent takes integer frame, integer parent returns nothing
    endfunction

    function AudastraUISetText takes integer frame, string text returns nothing
    endfunction

    function AudastraUISetFont takes integer frame, string font, real height, integer flags returns nothing
    endfunction

    function AudastraUISetTextAlign takes integer frame, integer horizontal, integer vertical returns nothing
    endfunction

    function AudastraUISetColor takes integer frame, integer red, integer green, integer blue, integer alpha returns nothing
    endfunction

    function AudastraUISetTexture takes integer frame, string path, boolean tile returns nothing
    endfunction

    function AudastraUISetValue takes integer frame, real value returns nothing
    endfunction

    function AudastraUISetLimits takes integer frame, real minimum, real maximum returns nothing
    endfunction

    function AudastraUISetModel takes integer frame, string path, integer kind returns nothing
    endfunction

    function AudastraUISetEnabled takes integer frame, boolean enabled returns nothing
    endfunction

    function AudastraUIDestroy takes integer frame returns nothing
    endfunction

endlibrary
