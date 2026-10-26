// 原创手动验收函数；放在 AudastraJapi.j 之后，每个新 VM 只运行一次。
// 返回 0 仅代表此基础检查完成；不是 DLL、同步事件、联机或内存泄漏验收。
// 不创建/修改作者 GUI T。由专用原创测试图显式调用并记录返回值。
function AjRunRealEncodingSmoke takes nothing returns integer
    local string encoded = AjArgReal(0.0)
    local string token = ""
    local real decoded = 0.0
    if encoded != ";r:0" or AjArgReal(-0.0) != ";r:0" then
        return 1
    endif
    set encoded = AjArgReal(1.0 / 1000000000.0)
    set token = SubString(encoded, 3, StringLength(encoded))
    if not AjRealSyntax(token) then
        return 2
    endif
    set decoded = AjReadReal(token)
    if not (decoded > 0.0) or decoded < 0.5 / 1000000000.0 or decoded > 2.0 / 1000000000.0 then
        return 2
    endif
    set encoded = AjArgReal(-1.0 / 1000000000.0)
    set token = SubString(encoded, 3, StringLength(encoded))
    if not AjRealSyntax(token) then
        return 3
    endif
    set decoded = AjReadReal(token)
    if not (decoded < 0.0) or decoded > -0.5 / 1000000000.0 or decoded < -2.0 / 1000000000.0 then
        return 3
    endif
    set encoded = AjArgReal(1000000000.0)
    set token = SubString(encoded, 3, StringLength(encoded))
    if not AjRealSyntax(token) then
        return 4
    endif
    set decoded = AjReadReal(token)
    if decoded < 900000000.0 or decoded > 1100000000.0 then
        return 4
    endif
    return 0
endfunction

function AjRunStringBridgeSmoke takes nothing returns integer
    local string retained = ""
    local integer unused = 0
    if AjRunRealEncodingSmoke() != 0 then
        return 11
    endif
    if not AjInitialize() then
        return 1
    endif
    if AjCapabilities != 3 then
        return 2
    endif
    set retained = AjCallText("host.version", "")
    if AjLastStatus != 0 or retained != "2.0.0" then
        return 3
    endif
    // 运行时只自带界面模块 audastraui；地图不能再装入其他模块。
    set unused = AjCallInt("module.count", "")
    if AjLastStatus != 0 or unused != 1 then
        return 4
    endif
    if not AjModuleLoad("audastraui") or SubString(AjCallText("module.info", AjArgText("audastraui")), 0, 17) != "audastraui|1.0.0|" then
        return 4
    endif
    if AjCallVoid("module.load", AjArgText("demo")) or AjLastStatus != 12 then
        return 5
    endif
    if AjBindEvents() or AjLastStatus != 12 then
        return 6
    endif
    // TEXT 编码应先通过，再由 host.status 的 INTEGER 参数签名拒绝。
    if AjRequest("host.status", AjArgText("中文;%|"), "s") or AjLastStatus != 5 then
        return 7
    endif
    if AjCallText("host.status", AjArgInt(-17)) != "unknown status" or AjLastStatus != 0 then
        return 8
    endif
    if retained != "2.0.0" then
        return 9
    endif
    if not AjCallVoid("host.pump", "") then
        return 10
    endif
    return 0
endfunction
