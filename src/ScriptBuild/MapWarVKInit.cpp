// WarVK 渲染设置的受管初始化段（JASS）。设置来自地图内编辑器成员 audastra\warvk.ini（地图设置 → WarVK），
// 每次保存按设置重写；不是作者触发器，也不开放任意脚本文本。
#include "MapSections.h"
#include "ScriptNames.h"

#include "War3MapCore/MapData/WarVKMapSettings.h"

namespace War3Map::Script::detail {
namespace {
const MapData::WarVKMapSettings* Active(const MapContext& context) {
    return context.in.warvk && context.in.warvk->HasCommands() ? context.in.warvk : nullptr;
}
} // namespace

void WriteWarVKGlobals(MapContext& context) {
    if (!Active(context)) return;
    // 协议版本：WarVK 接管时非零，未接管为 0；各条指令结果（0 为接受）按执行顺序存入数组，供测试运行读取。
    context.out.Line("integer                 AudastraWarVKProtocol      = 0");
    context.out.Line("integer array           AudastraWarVKErrors");
}

void WriteWarVKInitFunction(MapContext& context) {
    const auto* settings = Active(context);
    if (!settings) return;
    const auto commands = MapData::WarVKInitCommands(*settings);
    if (!commands.Succeeded() || !commands.value) {
        context.diagnostics.insert(context.diagnostics.end(), commands.diagnostics.begin(), commands.diagnostics.end());
        context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "MAP_WARVK_SETTINGS_INVALID",
            "WarVK 设置无效，未写出任何 WarVK 指令；请在 地图设置 → WarVK 中修正。", OriginSpan{std::string(MapData::kWarVKSettingsMember), 0, 0, {}}});
        return;
    }
    auto& out = context.out;
    const bool timer = settings->timing == MapData::WarVKInitTiming::AfterInitialization;
    out.Line("//===========================================================================");
    out.Line("// Audastra managed: WarVK render settings (Map Settings > WarVK). Rewritten on every save.");
    out.Line("function AudastraWarVKInit takes nothing returns nothing");
    out.Begin();
    out.Line("set AudastraWarVKProtocol = GetLocalizedHotkey( " + QuoteJassString(MapData::kWarVKProtocolQuery) + " )");
    for (std::size_t i = 0; i < commands.value->size(); ++i) {
        out.Line("call Preloader( " + QuoteJassString((*commands.value)[i]) + " )");
        out.Line("set AudastraWarVKErrors[" + std::to_string(i) + "] = GetLocalizedHotkey( "
            + QuoteJassString(MapData::kWarVKLastErrorQuery) + " )");
    }
    if (timer) out.Line("call DestroyTimer( GetExpiredTimer() )");
    out.End();
    out.Line("endfunction");
    out.Line("");
}

void WriteWarVKMainStart(MapContext& context) {
    const auto* settings = Active(context);
    if (settings && settings->timing == MapData::WarVKInitTiming::MainStart) context.out.Line("call AudastraWarVKInit(  )");
}

void WriteWarVKMainEnd(MapContext& context) {
    const auto* settings = Active(context);
    // 原版镜头控制器在初始化完成后才可用：0 秒单次计时器在首帧之后执行受管指令。
    if (settings && settings->timing == MapData::WarVKInitTiming::AfterInitialization)
        context.out.Line("call TimerStart( CreateTimer(), 0.0, false, function AudastraWarVKInit )");
}

} // namespace War3Map::Script::detail
