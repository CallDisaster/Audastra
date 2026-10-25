// 本方原创的最小探针调用端；探针本身保持黑盒，不编入本脚本。
// 不包含 DLL、JAPI 或 UI；调用前后提示不构成探针已执行的证据。
globals
    timer AjAiProbeTimer = null
endglobals

function AjAiProbeBegin takes nothing returns nothing
    call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, "AUD_PROBE: calling StartCampaignAI once")
    call StartCampaignAI(Player(PLAYER_NEUTRAL_AGGRESSIVE), "warvk_probe.ai")
    call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, "AUD_PROBE: call returned; awaiting black-box marker")
endfunction

function AjAiProbeStart takes nothing returns nothing
    if AjAiProbeTimer != null then
        return
    endif
    set AjAiProbeTimer = CreateTimer()
    call TimerStart(AjAiProbeTimer, 1.0, false, function AjAiProbeBegin)
endfunction
