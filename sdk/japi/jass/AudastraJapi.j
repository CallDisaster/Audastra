// Audastra JAPI 2.0. Include once, before generated module wrappers.
// Requires an installed, verified host adapter for GetLocalizedString.
// No DLL loading trick, game addresses, or third-party communications code is included.
globals
    integer AjEpoch = 0
    integer AjNextRequest = 0
    integer AjCapabilities = 0
    boolean AjReady = false
    string AjExpectedRuntimeIdentity = ""
    integer AjLastStatus = 8
    string AjLastKind = "v"
    integer AjLastInteger = 0
    real AjLastReal = 0.0
    string AjLastText = ""
    boolean AjDecodeOK = false
    boolean AjInDispatch = false
    integer AjEventId = 0
    integer AjEventInstance = 0
    integer AjEventClass = 1
    string AjEventModule = ""
    string AjEventType = ""
    string AjEventPayload = ""
    integer AjNextListener = 0
    integer array AjListenerIds
    string array AjListenerModules
    string array AjListenerTypes
    trigger array AjListenerTriggers
endglobals

function AjFail takes integer status returns boolean
    set AjLastStatus = status
    set AjLastKind = "v"
    set AjLastInteger = 0
    set AjLastReal = 0.0
    set AjLastText = ""
    return false
endfunction

function AjFind takes string text, string delimiter, integer start returns integer
    local integer n = StringLength(text)
    loop
        exitwhen start >= n
        if SubString(text, start, start + 1) == delimiter then
            return start
        endif
        set start = start + 1
    endloop
    return -1
endfunction

function AjCanonicalInt takes string text returns boolean
    return text != "" and I2S(S2I(text)) == text
endfunction

function AjIdentifier takes string text, integer limit, boolean dots returns boolean
    local integer i = 0
    local integer n = StringLength(text)
    local boolean first = true
    local string ch
    if n == 0 or n > limit then
        return false
    endif
    loop
        exitwhen i >= n
        set ch = SubString(text, i, i + 1)
        if ch == "." and dots and not first then
            set first = true
        else
            if first and AjFind("abcdefghijklmnopqrstuvwxyz", ch, 0) < 0 then
                return false
            endif
            if AjFind("abcdefghijklmnopqrstuvwxyz0123456789_-", ch, 0) < 0 then
                return false
            endif
            set first = false
        endif
        set i = i + 1
    endloop
    return not first
endfunction

function AjRealSyntax takes string text returns boolean
    local integer i = 0
    local integer n = StringLength(text)
    local integer digits = 0
    local integer start
    local string ch
    if n == 0 or n > 32 then
        return false
    endif
    if SubString(text, 0, 1) == "-" then
        set i = 1
    endif
    loop
        exitwhen i >= n
        set ch = SubString(text, i, i + 1)
        // 单字符规范整数只能是ASCII数字，避免每一位再扫描十字符表。
        exitwhen not AjCanonicalInt(ch)
        set digits = digits + 1
        set i = i + 1
    endloop
    if SubString(text, i, i + 1) == "." then
        set i = i + 1
        loop
            exitwhen i >= n
            set ch = SubString(text, i, i + 1)
            exitwhen not AjCanonicalInt(ch)
            set digits = digits + 1
            set i = i + 1
        endloop
    endif
    if digits == 0 then
        return false
    endif
    set ch = SubString(text, i, i + 1)
    if ch == "e" or ch == "E" then
        set i = i + 1
        set start = i
        set ch = SubString(text, i, i + 1)
        if ch == "+" or ch == "-" then
            set i = i + 1
        endif
        set digits = 0
        loop
            exitwhen i >= n
            set ch = SubString(text, i, i + 1)
            exitwhen not AjCanonicalInt(ch)
            set digits = digits + 1
            set i = i + 1
        endloop
        if digits == 0 or digits > 2 or S2I(SubString(text, start, i)) > 46 or S2I(SubString(text, start, i)) < -46 then
            return false
        endif
    endif
    return i == n
endfunction

function AjEscape takes string text returns string
    local integer i = 0
    local integer n = StringLength(text)
    local string output = ""
    local string ch
    loop
        exitwhen i >= n
        set ch = SubString(text, i, i + 1)
        if ch == "%" then
            set output = output + "%25"
        elseif ch == ";" then
            set output = output + "%3B"
        else
            set output = output + ch
        endif
        set i = i + 1
    endloop
    return output
endfunction

// Host output is canonical: only '%' and ';' are escaped, using uppercase hex.
function AjUnescape takes string text returns string
    local integer i = 0
    local integer n = StringLength(text)
    local string output = ""
    local string ch
    set AjDecodeOK = false
    loop
        exitwhen i >= n
        set ch = SubString(text, i, i + 1)
        if ch == ";" then
            return ""
        elseif ch == "%" then
            set ch = SubString(text, i, i + 3)
            if ch == "%25" then
                set output = output + "%"
            elseif ch == "%3B" then
                set output = output + ";"
            else
                return ""
            endif
            set i = i + 3
        else
            set output = output + ch
            set i = i + 1
        endif
    endloop
    set AjDecodeOK = true
    return output
endfunction

function AjArgInt takes integer value returns string
    return ";i:" + I2S(value)
endfunction

function AjArgHandle takes integer value returns string
    return ";d:" + I2S(value)
endfunction

function AjArgBool takes boolean value returns string
    if value then
        return ";b:1"
    endif
    return ";b:0"
endfunction

function AjArgText takes string value returns string
    return ";s:" + AjEscape(value)
endfunction

// Explicit decimal interface for author-controlled precision, e.g. "0.000000001".
// The host performs strict finite binary32 parsing; never concatenate untrusted tokens here.
function AjArgRealText takes string value returns string
    if AjFind(value, ";", 0) >= 0 then
        return ";r:invalid"
    endif
    return ";r:" + value
endfunction

// R2S uses too few decimal places. Normalize then request nine significant digits.
// Arithmetic in the original VM is binary32: this convenience conversion is approximate.
function AjArgReal takes real value returns string
    local integer exponent = 0
    local string sign = ""
    local string mantissa
    // 经典VM的实数相等有容差，不能用==0把很小的非零值抹掉，也不能用减法!=0判有限数。
    if not (value >= 0.0 or value <= 0.0) then
        return ";r:invalid"
    endif
    if value >= 0.0 and value <= 0.0 then
        return ";r:0"
    endif
    if value < 0.0 then
        set sign = "-"
        set value = -value
    endif
    loop
        exitwhen value < 10.0 or exponent >= 40
        set value = value / 10.0
        set exponent = exponent + 1
    endloop
    loop
        exitwhen value >= 1.0 or exponent <= -46
        set value = value * 10.0
        set exponent = exponent - 1
    endloop
    set mantissa = R2SW(value, 0, 8)
    // 归一化有界；无穷仍会得到非数值文字，不能进入线格式。
    if not AjRealSyntax(mantissa) then
        return ";r:invalid"
    endif
    return ";r:" + sign + mantissa + "e" + I2S(exponent)
endfunction

// Handles host scientific notation without assuming S2R accepts an exponent.
function AjReadReal takes string text returns real
    local integer pos = AjFind(text, "e", 0)
    local integer exponent
    local real value
    if pos < 0 then
        set pos = AjFind(text, "E", 0)
    endif
    if pos < 0 then
        return S2R(text)
    endif
    set value = S2R(SubString(text, 0, pos))
    set exponent = S2I(SubString(text, pos + 1, StringLength(text)))
    loop
        exitwhen exponent <= 0
        set value = value * 10.0
        set exponent = exponent - 1
    endloop
    loop
        exitwhen exponent >= 0
        set value = value / 10.0
        set exponent = exponent + 1
    endloop
    return value
endfunction

// Each response carries its own status. No process-global last-error query can overwrite it.
// Locals retain the outer request identity even if the native invokes AjDispatch reentrantly.
function AjRequest takes string command, string arguments, string expectedKind returns boolean
    local integer request
    local string wire
    local string response
    local integer p1
    local integer p2
    local integer p3
    local string epochText
    local string idText
    local string statusText
    local string kind
    local string value
    local integer responseEpoch
    local integer status
    if not AjReady and command != "host.hello" then
        return AjFail(8)
    endif
    if AjNextRequest >= 2147483647 then
        return AjFail(3)
    endif
    set AjNextRequest = AjNextRequest + 1
    set request = AjNextRequest
    set wire = "ajapi:2;" + I2S(AjEpoch) + ";" + I2S(request) + ";" + command + arguments
    if StringLength(wire) > 512 then
        return AjFail(3)
    endif
    set response = GetLocalizedString(wire)
    if StringLength(response) > 512 or SubString(response, 0, 8) != "ajapi:2;" then
        return AjFail(16)
    endif
    set p1 = AjFind(response, ";", 8)
    set p2 = AjFind(response, ";", p1 + 1)
    set p3 = AjFind(response, ";", p2 + 1)
    if p1 < 0 or p2 < 0 or p3 < 0 then
        return AjFail(1)
    endif
    set epochText = SubString(response, 8, p1)
    set idText = SubString(response, p1 + 1, p2)
    set statusText = SubString(response, p2 + 1, p3)
    if not AjCanonicalInt(epochText) or not AjCanonicalInt(idText) or not AjCanonicalInt(statusText) then
        return AjFail(1)
    endif
    set responseEpoch = S2I(epochText)
    set status = S2I(statusText)
    if responseEpoch <= 0 or S2I(idText) != request or status < 0 or status > 17 then
        return AjFail(1)
    endif
    if command != "host.hello" and responseEpoch != AjEpoch then
        return AjFail(15)
    endif
    set kind = SubString(response, p3 + 1, p3 + 2)
    if SubString(response, p3 + 2, p3 + 3) != ":" then
        return AjFail(1)
    endif
    set value = SubString(response, p3 + 3, StringLength(response))
    if status != 0 then
        if kind != "v" or value != "" then
            return AjFail(1)
        endif
        return AjFail(status)
    endif
    if kind != expectedKind then
        return AjFail(13)
    endif
    set AjLastKind = kind
    set AjLastInteger = 0
    set AjLastReal = 0.0
    set AjLastText = ""
    if kind == "s" then
        if StringLength(value) > 416 then
            return AjFail(3)
        endif
        set AjLastText = AjUnescape(value)
        if not AjDecodeOK then
            return AjFail(4)
        endif
    elseif kind == "i" or kind == "d" or kind == "b" then
        if not AjCanonicalInt(value) then
            return AjFail(5)
        endif
        set AjLastInteger = S2I(value)
        if kind == "b" and AjLastInteger != 0 and AjLastInteger != 1 then
            return AjFail(5)
        endif
    elseif kind == "r" then
        if not AjRealSyntax(value) then
            return AjFail(5)
        endif
        set AjLastText = value
        set AjLastReal = AjReadReal(value)
    elseif kind != "v" or value != "" then
        return AjFail(5)
    endif
    if command == "host.hello" then
        set AjEpoch = responseEpoch
    endif
    set AjLastStatus = 0
    return true
endfunction

function AjInitialize takes nothing returns boolean
    if AjReady then
        return true
    endif
    if not AjRequest("host.hello", "", "i") then
        return false
    endif
    set AjCapabilities = AjLastInteger
    set AjReady = true
    if AjExpectedRuntimeIdentity != "" then
        if not AjRequest("host.runtimeidentity", "", "s") then
            set AjReady = false
            return false
        endif
        if AjLastText != AjExpectedRuntimeIdentity then
            set AjReady = false
            return AjFail(14)
        endif
    endif
    return true
endfunction

function AjCallInt takes string command, string arguments returns integer
    if AjRequest(command, arguments, "i") then
        return AjLastInteger
    endif
    return 0
endfunction

function AjCallHandle takes string command, string arguments returns integer
    if AjRequest(command, arguments, "d") then
        return AjLastInteger
    endif
    return 0
endfunction

function AjCallBool takes string command, string arguments returns boolean
    return AjRequest(command, arguments, "b") and AjLastInteger != 0
endfunction

function AjCallReal takes string command, string arguments returns real
    if AjRequest(command, arguments, "r") then
        return AjLastReal
    endif
    return 0.0
endfunction

function AjCallText takes string command, string arguments returns string
    if AjRequest(command, arguments, "s") then
        return AjLastText
    endif
    return ""
endfunction

function AjCallVoid takes string command, string arguments returns boolean
    return AjRequest(command, arguments, "v")
endfunction

function AjModuleLoad takes string name returns boolean
    return AjCallVoid("module.load", AjArgText(name))
endfunction

function AjModuleUnload takes string name returns boolean
    return AjCallVoid("module.unload", AjArgText(name))
endfunction

function AjPump takes nothing returns boolean
    return AjCallVoid("host.pump", "")
endfunction

// Module/type filters are exact or '*'. Register at most 64 live listeners.
// IDs monotonically increase, so an old ID can never unsubscribe a replacement listener.
function AjOn takes string moduleName, string eventType, code callback returns integer
    local integer slot = 1
    local triggeraction action
    if AjInDispatch or AjNextListener >= 2147483647 or callback == null then
        return 0
    endif
    if (moduleName != "*" and not AjIdentifier(moduleName, 32, false)) or (eventType != "*" and not AjIdentifier(eventType, 31, true)) then
        return 0
    endif
    loop
        exitwhen slot > 64 or AjListenerIds[slot] == 0
        set slot = slot + 1
    endloop
    if slot > 64 then
        return 0
    endif
    set AjListenerTriggers[slot] = CreateTrigger()
    if AjListenerTriggers[slot] == null then
        return 0
    endif
    set action = TriggerAddAction(AjListenerTriggers[slot], callback)
    if action == null then
        call DestroyTrigger(AjListenerTriggers[slot])
        set AjListenerTriggers[slot] = null
        return 0
    endif
    set action = null
    set AjNextListener = AjNextListener + 1
    set AjListenerIds[slot] = AjNextListener
    set AjListenerModules[slot] = moduleName
    set AjListenerTypes[slot] = eventType
    return AjNextListener
endfunction

function AjOff takes integer listenerId returns boolean
    local integer slot = 1
    if AjInDispatch or listenerId <= 0 then
        return false
    endif
    loop
        exitwhen slot > 64
        if AjListenerIds[slot] == listenerId then
            call DestroyTrigger(AjListenerTriggers[slot])
            loop
                exitwhen slot >= 64
                set AjListenerTriggers[slot] = AjListenerTriggers[slot + 1]
                set AjListenerIds[slot] = AjListenerIds[slot + 1]
                set AjListenerModules[slot] = AjListenerModules[slot + 1]
                set AjListenerTypes[slot] = AjListenerTypes[slot + 1]
                set slot = slot + 1
            endloop
            set AjListenerTriggers[64] = null
            set AjListenerIds[64] = 0
            set AjListenerModules[64] = ""
            set AjListenerTypes[64] = ""
            return true
        endif
        set slot = slot + 1
    endloop
    return false
endfunction

// Reserve this exact function name in optimizers. No sleeps, waits or yields in callbacks.
// One host call delivers ONE event, and acknowledgement follows every author callback.
function AjDispatch takes nothing returns nothing
    local string eventText
    local integer p1
    local integer p2
    local integer p3
    local integer p4
    local integer p5
    local integer slot = 1
    local boolean acknowledged
    if AjInDispatch or not AjRequest("host.event", "", "s") then
        return
    endif
    set eventText = AjLastText
    set p1 = AjFind(eventText, "|", 0)
    set p2 = AjFind(eventText, "|", p1 + 1)
    set p3 = AjFind(eventText, "|", p2 + 1)
    set p4 = AjFind(eventText, "|", p3 + 1)
    set p5 = AjFind(eventText, "|", p4 + 1)
    if p1 < 0 or p2 < 0 or p3 < 0 or p4 < 0 or p5 < 0 then
        return
    endif
    set AjInDispatch = true
    set AjEventId = S2I(SubString(eventText, 0, p1))
    set AjEventInstance = S2I(SubString(eventText, p1 + 1, p2))
    set AjEventClass = S2I(SubString(eventText, p2 + 1, p3))
    set AjEventModule = SubString(eventText, p3 + 1, p4)
    set AjEventType = SubString(eventText, p4 + 1, p5)
    set AjEventPayload = SubString(eventText, p5 + 1, StringLength(eventText))
    if AjEventId != 0 then
        loop
            exitwhen slot > 64
            if AjListenerIds[slot] != 0 and (AjListenerModules[slot] == "*" or AjListenerModules[slot] == AjEventModule) and (AjListenerTypes[slot] == "*" or AjListenerTypes[slot] == AjEventType) then
                call TriggerExecute(AjListenerTriggers[slot])
            endif
            set slot = slot + 1
        endloop
    endif
    set acknowledged = AjCallVoid("host.ack", AjArgInt(AjEventId))
    set AjInDispatch = false
    set AjEventId = 0
    set AjEventInstance = 0
    set AjEventClass = 1
    set AjEventModule = ""
    set AjEventType = ""
    set AjEventPayload = ""
endfunction

function AjBindEvents takes nothing returns boolean
    return AjCallVoid("host.bind", AjArgText("AjDispatch"))
endfunction
