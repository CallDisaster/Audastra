constant integer AUD_ORDINAL_CASE = 0
integer AudOrdinalStep = 0
texttag array AudOrdinalTags
boolean array AudOrdinalSeen
texttag AudOrdinalExpired
texttag AudOrdinalZero
unit AudOrdinalUnit
rect AudOrdinalRect
// AUD_NATIVE_PROBE_FUNCTIONS
function AudOrdinalRecord takes string text returns nothing
    set AudOrdinalStep = AudOrdinalStep + 1
    call PreloadGenClear()
    call PreloadGenStart()
    call Preload("R50_ORDINAL|" + I2S(AUD_ORDINAL_CASE) + "|" + I2S(AudOrdinalStep) + "|" + text)
    call PreloadGenEnd("Logs\\AudastraR50OrdinalFifth\\case" + I2S(AUD_ORDINAL_CASE) + "-" + I2S(AudOrdinalStep) + ".pld")
endfunction

function AudOrdinalSetTag takes texttag tag, string phase returns nothing
    call AudOrdinalRecord(phase + ":before:SetTextTagText")
    call SetTextTagText(tag, "R50 ordinal " + phase, 0.024)
    call AudOrdinalRecord(phase + ":after:SetTextTagText")
    call SetTextTagPos(tag, 0.0, 0.0, 128.0)
    call AudOrdinalRecord(phase + ":after:SetTextTagPos")
    call SetTextTagPosUnit(tag, AudOrdinalUnit, 128.0)
    call AudOrdinalRecord(phase + ":after:SetTextTagPosUnit")
    call SetTextTagColor(tag, 255, 220, 120, 255)
    call AudOrdinalRecord(phase + ":after:SetTextTagColor")
    call SetTextTagVelocity(tag, 0.0, 0.01)
    call AudOrdinalRecord(phase + ":after:SetTextTagVelocity")
    call SetTextTagVisibility(tag, true)
    call AudOrdinalRecord(phase + ":after:SetTextTagVisibility")
    call SetTextTagSuspended(tag, false)
    call AudOrdinalRecord(phase + ":after:SetTextTagSuspended")
    call SetTextTagPermanent(tag, true)
    call AudOrdinalRecord(phase + ":after:SetTextTagPermanent")
    call SetTextTagAge(tag, 0.0)
    call AudOrdinalRecord(phase + ":after:SetTextTagAge")
    call SetTextTagLifespan(tag, 20.0)
    call AudOrdinalRecord(phase + ":after:SetTextTagLifespan")
    call SetTextTagFadepoint(tag, 19.0)
    call AudOrdinalRecord(phase + ":after:SetTextTagFadepoint")
    call DestroyTextTag(tag)
    call AudOrdinalRecord(phase + ":after:DestroyTextTag")
endfunction

function AudOrdinalAuditTags takes string phase returns boolean
    local integer i = 0
    local integer id
    local boolean valid = true
    loop
        exitwhen i >= 100
        set AudOrdinalSeen[i] = false
        set i = i + 1
    endloop
    set i = 0
    loop
        exitwhen i >= 100
        set AudOrdinalTags[i] = CreateTextTag()
        call SetTextTagPermanent(AudOrdinalTags[i], true)
        set id = GetHandleId(AudOrdinalTags[i])
        if id < 0 or id >= 100 then
            set valid = false
        elseif AudOrdinalSeen[id] then
            set valid = false
        else
            set AudOrdinalSeen[id] = true
        endif
        set i = i + 1
    endloop
    if not valid then
        call AudOrdinalRecord("DATA_INCONSISTENT:" + phase)
        return false
    endif
    call AudOrdinalRecord(phase + ":100-distinct")
    set i = 0
    loop
        exitwhen i >= 100
        call DestroyTextTag(AudOrdinalTags[i])
        set i = i + 1
    endloop
    return true
endfunction

function AudOrdinalExpiredPhase takes nothing returns nothing
    local texttag nextTag = CreateTextTag()
    local integer oldId = GetHandleId(AudOrdinalExpired)
    local integer nextId = GetHandleId(nextTag)
    local integer i = 0
    call AudOrdinalRecord("expiry:elapsed=" + R2S(TimerGetElapsed(GetExpiredTimer())) + ":old=" + I2S(oldId) + ":next=" + I2S(nextId))
    call DestroyTimer(GetExpiredTimer())
    if oldId != nextId then
        call AudOrdinalRecord("INCOMPLETE:expiry-not-reused")
        return
    endif
    call DestroyTextTag(nextTag)
    call AudOrdinalSetTag(AudOrdinalExpired, "expired-empty")
    if not AudOrdinalAuditTags("after-expired") then
        return
    endif
    loop
        exitwhen i >= 100
        set AudOrdinalTags[i] = CreateTextTag()
        call SetTextTagPermanent(AudOrdinalTags[i], true)
        set i = i + 1
    endloop
    set AudOrdinalZero = CreateTextTag()
    call AudOrdinalRecord("exhausted:create=" + I2S(GetHandleId(AudOrdinalZero)) + ":live-last=" + I2S(GetHandleId(AudOrdinalTags[99])))
    if GetHandleId(AudOrdinalZero) != 0 then
        call AudOrdinalRecord("INCOMPLETE:no-zero-failure")
        return
    endif
    call AudOrdinalSetTag(AudOrdinalZero, "failed-zero-occupied")
    set AudOrdinalTags[99] = CreateTextTag()
    call AudOrdinalRecord("after-failed-zero:replacement=" + I2S(GetHandleId(AudOrdinalTags[99])))
    set i = 0
    loop
        exitwhen i >= 100
        call DestroyTextTag(AudOrdinalTags[i])
        set i = i + 1
    endloop
    call AudOrdinalSetTag(AudOrdinalZero, "failed-zero-empty")
    if not AudOrdinalAuditTags("after-zero") then
        return
    endif
    call RemoveUnit(AudOrdinalUnit)
    call AudOrdinalRecord("R50_ORDINAL_CASE1_COMPLETE")
    call DisplayTimedTextToPlayer(Player(0), 0, 0, 40, "R50 texttag JASS ordinal probe completed")
endfunction

function AudOrdinalTagPhase takes nothing returns nothing
    local texttag tag = CreateTextTag()
    local texttag replacement
    local texttag second
    call AudOrdinalRecord("destroyed:created=" + I2S(GetHandleId(tag)))
    call DestroyTextTag(tag)
    call AudOrdinalSetTag(tag, "destroyed-empty")
    set replacement = CreateTextTag()
    set second = CreateTextTag()
    call AudOrdinalRecord("after-destroyed:first=" + I2S(GetHandleId(replacement)) + ":second=" + I2S(GetHandleId(second)))
    if GetHandleId(replacement) == GetHandleId(second) then
        call AudOrdinalRecord("DATA_INCONSISTENT:destroyed-pair")
        return
    endif
    call DestroyTextTag(second)
    call DestroyTextTag(replacement)
    if not AudOrdinalAuditTags("after-destroyed") then
        return
    endif
    set AudOrdinalExpired = CreateTextTag()
    call SetTextTagText(AudOrdinalExpired, "R50 expires before stale calls", 0.020)
    call SetTextTagPos(AudOrdinalExpired, 0, 0, 80)
    call SetTextTagVisibility(AudOrdinalExpired, true)
    call SetTextTagSuspended(AudOrdinalExpired, false)
    call SetTextTagPermanent(AudOrdinalExpired, false)
    call SetTextTagAge(AudOrdinalExpired, 0.0)
    call SetTextTagLifespan(AudOrdinalExpired, 0.25)
    call SetTextTagFadepoint(AudOrdinalExpired, 0.1)
    call AudOrdinalRecord("expiry:created=" + I2S(GetHandleId(AudOrdinalExpired)))
    call TimerStart(CreateTimer(), 3.0, false, function AudOrdinalExpiredPhase)
endfunction

function AudOrdinalSetWeather takes weathereffect effect, string phase returns nothing
    call AudOrdinalRecord(phase + ":before:EnableWeatherEffect-true")
    call EnableWeatherEffect(effect, true)
    call AudOrdinalRecord(phase + ":after:EnableWeatherEffect-true")
    call EnableWeatherEffect(effect, false)
    call AudOrdinalRecord(phase + ":after:EnableWeatherEffect-false")
    call RemoveWeatherEffect(effect)
    call AudOrdinalRecord(phase + ":after:RemoveWeatherEffect")
endfunction

function AudOrdinalWeatherPhase takes nothing returns nothing
    local weathereffect zero
    local weathereffect removed
    local weathereffect replacement
    local weathereffect failed
    set AudOrdinalRect = Rect(-256.0, -256.0, 256.0, 256.0)
    set zero = AddWeatherEffect(AudOrdinalRect, 'RAhr')
    set removed = AddWeatherEffect(AudOrdinalRect, 'RAhr')
    call AudOrdinalRecord("weather:live-zero=" + I2S(GetHandleId(zero)) + ":removed=" + I2S(GetHandleId(removed)))
    call RemoveWeatherEffect(removed)
    call AudOrdinalSetWeather(removed, "weather-removed-empty")
    set replacement = AddWeatherEffect(AudOrdinalRect, 'RAhr')
    call AudOrdinalRecord("weather:replacement=" + I2S(GetHandleId(replacement)))
    call AudOrdinalSetWeather(removed, "weather-removed-reused")
    set replacement = AddWeatherEffect(AudOrdinalRect, 'RAhr')
    call AudOrdinalRecord("weather:after-alias-remove=" + I2S(GetHandleId(replacement)))
    call RemoveWeatherEffect(replacement)
    call AudOrdinalRecord("weather:before-invalid-template")
    set failed = AddWeatherEffect(AudOrdinalRect, 0)
    call AudOrdinalRecord("weather:invalid-template-result=" + I2S(GetHandleId(failed)))
    if GetHandleId(failed) != 0 then
        call AudOrdinalRecord("INCOMPLETE:weather-no-zero-failure")
        return
    endif
    call AudOrdinalSetWeather(failed, "weather-failed-zero-occupied")
    set replacement = AddWeatherEffect(AudOrdinalRect, 'RAhr')
    call AudOrdinalRecord("weather:after-zero-alias-remove=" + I2S(GetHandleId(replacement)))
    call RemoveWeatherEffect(replacement)
    call AudOrdinalSetWeather(failed, "weather-failed-zero-empty")
    set zero = AddWeatherEffect(AudOrdinalRect, 'RAhr')
    set replacement = AddWeatherEffect(AudOrdinalRect, 'RAhr')
    call AudOrdinalRecord("weather:final-pair=" + I2S(GetHandleId(zero)) + ":" + I2S(GetHandleId(replacement)))
    if GetHandleId(zero) == GetHandleId(replacement) then
        call AudOrdinalRecord("DATA_INCONSISTENT:weather-pair")
        return
    endif
    call RemoveWeatherEffect(zero)
    call RemoveWeatherEffect(replacement)
    call RemoveRect(AudOrdinalRect)
    call AudOrdinalRecord("R50_ORDINAL_CASE2_COMPLETE")
    call DisplayTimedTextToPlayer(Player(0), 0, 0, 40, "R50 weather JASS ordinal probe completed")
endfunction

function AudastraNativeHandleProbe takes nothing returns nothing
    call AudOrdinalRecord("BEGIN")
    if AUD_ORDINAL_CASE == 1 then
        set AudOrdinalUnit = CreateUnit(Player(0), 'hfoo', 0, 0, 0)
        call AudOrdinalTagPhase()
    elseif AUD_ORDINAL_CASE == 2 then
        call AudOrdinalWeatherPhase()
    else
        call AudOrdinalRecord("INCOMPLETE:select-explicit-case")
    endif
    call DestroyTimer(GetExpiredTimer())
endfunction
