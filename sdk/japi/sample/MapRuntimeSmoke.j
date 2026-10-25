// 原创离线/实机验收样例。仅在专用游戏实例使用；不是作者地图的触发器替代品。
// 打包器在最终 main 第一条语句启动 AI 一次；本样例不负责启动或重复调用 AI。
// 在 main 初始化末尾调用 AjMapSmokeStart 一次，仅延迟开始握手轮询；固定导入名遵循 AGENT_USAGE.md。
globals
    timer AjSmokeTimer = null
    integer AjSmokeTries = 0
    integer AjSmokePhase = 0
    integer AjSmokeCycles = 0
    integer AjSmokeRoot = 0
    integer AjSmokeLabel = 0
    string AjSmokeKept = ""
    boolean AjSmokeTriggerPassed = false
endglobals

function AjMapSmokeFail takes string stage returns nothing
    call PauseTimer(AjSmokeTimer)
    call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, "JAPI MAP FAIL: " + stage + " status=" + I2S(AjLastStatus))
endfunction

function AjMapSmokeTriggerProbe takes nothing returns nothing
    set AjSmokeTriggerPassed = AjCallText("host.version", "") == "2.0.0" and AjLastStatus == 0
endfunction

function AjMapSmokeTick takes nothing returns nothing
    local integer old = 0
    local trigger probe = null
    if AjSmokePhase == 0 then
        set AjSmokeTries = AjSmokeTries + 1
        // 只重试握手，不重复启动 AI；握手前不调用 UI，避免缓存过早的连接失败。
        if not AjInitialize() then
            if AjSmokeTries >= 150 then
                call AjMapSmokeFail("loader/hello timeout")
            endif
            return
        endif
        if not AjModuleLoad("audastraui") then
            call AjMapSmokeFail("module.load")
            return
        endif
        if not AudastraUIIsReady() then
            call AjMapSmokeFail("UI not ready: " + AudastraUILastError())
            return
        endif
        if GetLocalizedString("Audastra ordinary localization") != "Audastra ordinary localization" then
            call AjMapSmokeFail("ordinary localization")
            return
        endif
        set AjSmokeKept = AjCallText("host.version", "")
        if AjSmokeKept != "2.0.0" or AjLastStatus != 0 then
            call AjMapSmokeFail("version return ownership")
            return
        endif
        set probe = CreateTrigger()
        call TriggerAddAction(probe, function AjMapSmokeTriggerProbe)
        call TriggerExecute(probe)
        call DestroyTrigger(probe)
        set probe = null
        if not AjSmokeTriggerPassed then
            call AjMapSmokeFail("trigger context")
            return
        endif
        set AjSmokeRoot = AudastraUIGameUI()
        if AjSmokeRoot == 0 then
            call AjMapSmokeFail("root")
            return
        endif
        call AudastraUILoadToc("AudastraSmoke.toc")
        if AjLastStatus != 0 then
            call AjMapSmokeFail("loadtoc")
            return
        endif
        set AjSmokePhase = 1
    endif
    if AjSmokePhase == 1 then
        set AjSmokeLabel = AudastraUICreateFrame("AjSmokeText", AjSmokeRoot, 0, 0)
        if AjSmokeLabel == 0 or AjLastStatus != 0 then
            call AjMapSmokeFail("create")
            return
        endif
        call AudastraUISetAbsPoint(AjSmokeLabel, 4, 0.4, 0.35)
        if AjLastStatus != 0 then
            call AjMapSmokeFail("position")
            return
        endif
        call AudastraUISetSize(AjSmokeLabel, 0.6, 0.07)
        if AjLastStatus != 0 then
            call AjMapSmokeFail("size")
            return
        endif
        call AudastraUISetText(AjSmokeLabel, "Audastra DLL > JAPI > UI : cycle " + I2S(AjSmokeCycles))
        if AjLastStatus != 0 then
            call AjMapSmokeFail("text")
            return
        endif
        call AudastraUIShow(AjSmokeLabel, true)
        if AjLastStatus != 0 then
            call AjMapSmokeFail("show")
            return
        endif
        set AjSmokePhase = 2
        return
    endif
    if AjSmokePhase == 2 then
        if AjSmokeKept != "2.0.0" or not AudastraUIIsReady() then
            call AjMapSmokeFail("retained string/next timer context")
            return
        endif
        set old = AjSmokeLabel
        call AudastraUIDestroy(old)
        if AjLastStatus != 0 then
            call AjMapSmokeFail("destroy")
            return
        endif
        // 销毁后的句柄必须被拒绝；随后新建不得复活这个句柄。
        call AudastraUIShow(old, true)
        if AjLastStatus == 0 then
            call AjMapSmokeFail("stale handle accepted")
            return
        endif
        set AjSmokeCycles = AjSmokeCycles + 1
        if AjSmokeCycles < 20 then
            set AjSmokePhase = 1
            return
        endif
        set AjSmokeLabel = AudastraUICreateFrame("AjSmokeText", AjSmokeRoot, 0, 0)
        if AjSmokeLabel == 0 or AjSmokeLabel == old or AjLastStatus != 0 then
            call AjMapSmokeFail("final create/generation")
            return
        endif
        call AudastraUISetAbsPoint(AjSmokeLabel, 4, 0.4, 0.35)
        if AjLastStatus != 0 then
            call AjMapSmokeFail("final position")
            return
        endif
        call AudastraUISetSize(AjSmokeLabel, 0.6, 0.07)
        if AjLastStatus != 0 then
            call AjMapSmokeFail("final size")
            return
        endif
        call AudastraUISetText(AjSmokeLabel, "JAPI MAP PASS - 20 UI lifecycles")
        if AjLastStatus != 0 then
            call AjMapSmokeFail("final text")
            return
        endif
        call AudastraUIShow(AjSmokeLabel, true)
        if AjLastStatus != 0 then
            call AjMapSmokeFail("final show")
            return
        endif
        if AjCallText("host.version", "") != "2.0.0" or AjSmokeKept != "2.0.0" then
            call AjMapSmokeFail("final JASS assertions")
            return
        endif
        set AjSmokePhase = 3
        call PauseTimer(AjSmokeTimer)
        call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, "JAPI MAP PASS: loader + DLL + hello + UI + timer/trigger + stale handles")
    endif
endfunction

function AjMapSmokeBegin takes nothing returns nothing
    call DisplayTimedTextToPlayer(Player(0), 0.0, 0.0, 120.0, "JAPI MAP: polling loader/hello; no repeated AI startup")
    call TimerStart(AjSmokeTimer, 0.20, true, function AjMapSmokeTick)
endfunction

function AjMapSmokeStart takes nothing returns nothing
    if AjSmokeTimer != null then
        return
    endif
    set AjSmokeTimer = CreateTimer()
    // AI 已由打包器放在 main 首句调用；此计时器只等待初始化完成后开始握手轮询。
    call TimerStart(AjSmokeTimer, 1.0, false, function AjMapSmokeBegin)
endfunction
