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
    // 云海雾盒 id（localFog.createBox 的返回值）；WarVK 未接管或旧宿主为 0。
    if (context.in.warvk->cloudSea) context.out.Line("integer                 AudastraWarVKCloudSea      = 0");
}

void WriteWarVKInitFunction(MapContext& context) {
    const auto* settings = Active(context);
    if (!settings) return;
    const auto origin = OriginSpan{std::string(MapData::kWarVKSettingsMember), 0, 0, {}};
    const auto commands = MapData::WarVKInitCommands(*settings);
    std::optional<std::string> cloudSea;
    if (commands.Succeeded() && settings->cloudSea) {
        if (!context.in.warvkGround) {
            context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "WARVK_CLOUD_SEA_TERRAIN",
                "设了云海但读不到地图地形（war3map.w3e），无法确定云海范围；未写出任何 WarVK 指令。", origin});
            return;
        }
        const auto box = MapData::WarVKCloudSeaBoxFor(*settings->cloudSea, *context.in.warvkGround);
        if (!box.Succeeded() || !box.value) {
            for (auto diagnostic : box.diagnostics) {
                diagnostic.origin = origin;
                context.diagnostics.push_back(std::move(diagnostic));
            }
            return;
        }
        cloudSea = MapData::WarVKCloudSeaQuery(*box.value);
    }
    if (!commands.Succeeded() || !commands.value) {
        context.diagnostics.insert(context.diagnostics.end(), commands.diagnostics.begin(), commands.diagnostics.end());
        context.diagnostics.push_back({DiagnosticSeverity::Error, "Script.Map", "MAP_WARVK_SETTINGS_INVALID",
            "WarVK 设置无效，未写出任何 WarVK 指令；请在 地图设置 → WarVK 中修正。", origin});
        return;
    }
    for (auto warning : MapData::WarVKSettingsWarnings(*settings)) {
        warning.origin = origin;
        context.diagnostics.push_back(std::move(warning));
    }
    // 体积光打开后雾盒归体积光，云海会变成整图体积雾。受管段写在自定义代码与触发器之后，此时已写出的正文
    // 里出现 volumetric.setEnabled 就提醒；只查字面文本，拼接出的指令串和导入的脚本库查不到。
    if (cloudSea && context.out.Text().find("volumetric.setEnabled") != std::string::npos)
        context.diagnostics.push_back({DiagnosticSeverity::Warning, "MapData.WarVK", "WARVK_CLOUD_SEA_VOLUMETRIC",
            "地图脚本里出现 volumetric.setEnabled：体积光打开后，云海雾盒归体积光，会变成覆盖整图、密度等于覆盖度的体积雾，"
            "不再是云海。只检查自定义脚本和触发器里的字面文本。", origin});
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
    // 云海：返回雾盒 id 的查询，照常记录结果码；不写 volumetric.setEnabled（体积光打开后雾盒归体积光）。
    if (cloudSea) {
        out.Line("set AudastraWarVKCloudSea = GetLocalizedHotkey( " + QuoteJassString(*cloudSea) + " )");
        out.Line("set AudastraWarVKErrors[" + std::to_string(commands.value->size()) + "] = GetLocalizedHotkey( "
            + QuoteJassString(MapData::kWarVKLastErrorQuery) + " )");
    }
    if (!settings->skyModel.empty()) out.Line("call SetSkyModel( " + QuoteJassString(settings->skyModel) + " )");
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
