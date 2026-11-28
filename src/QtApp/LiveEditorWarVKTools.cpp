#include "LiveEditorWarVKTools.h"

#include "EditorProject.h"
#include "McpToolInternal.h"

#include <algorithm>
#include <cmath>

namespace War3Map::QtApp {
namespace {
using MapData::WarVKMapSettings;

// 光照循环开关：default 为不设置（保持 WarVK 默认），on/off 写出 b:1 / b:0。
QString SwitchText(const std::optional<bool>& value) { return !value ? QStringLiteral("default") : *value ? QStringLiteral("on") : QStringLiteral("off"); }

QJsonObject SwitchSchema() { return {{"type", "string"}, {"enum", QJsonArray{"default", "on", "off"}}}; }

QJsonObject NumberSchema(double minimum, double maximum) { return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}}; }

QJsonObject SettingsJson(QtProject::EditorProject& project) {
    const auto settings = project.QueryWarVKMapSettings();
    if (!settings.Succeeded() || !settings.value) return Mcp::Failure(settings);
    const auto& value = *settings.value;
    QJsonArray far;
    for (const auto& code : value.farDoodadTypes) far.append(QString::fromStdString(code));
    QJsonArray commands;
    const auto lines = MapData::WarVKInitCommands(value);
    if (lines.Succeeded() && lines.value) for (const auto& line : *lines.value) commands.append(QString::fromStdString(line));
    QJsonObject result{{"ok", true}, {"member", QString::fromUtf8(MapData::kWarVKSettingsMember.data(),
            static_cast<qsizetype>(MapData::kWarVKSettingsMember.size()))},
        {"timing", value.timing == MapData::WarVKInitTiming::MainStart ? "main" : "timer"},
        {"far_doodad_types", far}, {"far_doodad_limit", static_cast<int>(MapData::kWarVKMaxFarDoodadTypes)},
        {"sky_model", QString::fromStdString(value.skyModel)},
        {"sky_model_limit_bytes", static_cast<int>(MapData::kWarVKSkyModelMaxBytes)},
        {"commands", commands},
        {"protocol_query", QString::fromUtf8(MapData::kWarVKProtocolQuery.data(), static_cast<qsizetype>(MapData::kWarVKProtocolQuery.size()))},
        {"last_error_query", QString::fromUtf8(MapData::kWarVKLastErrorQuery.data(), static_cast<qsizetype>(MapData::kWarVKLastErrorQuery.size()))},
        {"revision", QString::number(project.editRevision_.Revision())}};
    result.insert("camera_limits", value.cameraLimits
        ? QJsonValue(QJsonObject{{"enabled", true}, {"max_distance", value.cameraLimits->maxDistance},
            {"near_clip", value.cameraLimits->nearClip}})
        : QJsonValue(QJsonObject{{"enabled", false}}));
    result.insert("sun_direction", value.sunDirection
        ? QJsonObject{{"enabled", true}, {"azimuth", value.sunDirection->azimuth}, {"elevation", value.sunDirection->elevation}}
        : QJsonObject{{"enabled", false}});
    result.insert("sun_color", value.sunColor
        ? QJsonObject{{"enabled", true}, {"red", value.sunColor->red}, {"green", value.sunColor->green},
            {"blue", value.sunColor->blue}, {"intensity", value.sunColor->intensity}}
        : QJsonObject{{"enabled", false}});
    result.insert("celestial_motion", SwitchText(value.celestialMotion));
    result.insert("time_color_grading", SwitchText(value.timeColorGrading));
    // 实际写出值：设了太阳方向/颜色而对应开关不设置时按关写出。
    result.insert("effective_celestial_motion", SwitchText(MapData::WarVKEffectiveCelestialMotion(value)));
    result.insert("effective_time_color_grading", SwitchText(MapData::WarVKEffectiveTimeColorGrading(value)));
    result.insert("cloud_sea", value.cloudSea
        ? QJsonObject{{"enabled", true}, {"top", value.cloudSea->top}, {"cover", value.cloudSea->cover}}
        : QJsonObject{{"enabled", false}});
    QJsonArray warnings;
    for (const auto& warning : MapData::WarVKSettingsWarnings(value))
        warnings.append(QJsonObject{{"code", QString::fromStdString(warning.code)}, {"message", QString::fromStdString(warning.message)}});
    // 当前地形的地面范围与云海盒；保存时按待写出的地形重新计算。
    const auto ground = project.QueryWarVKMapGround();
    if (ground.Succeeded() && ground.value) {
        result.insert("map_ground", QJsonObject{{"west", ground.value->west}, {"south", ground.value->south},
            {"east", ground.value->east}, {"north", ground.value->north}, {"lowest", ground.value->lowest}});
        if (value.cloudSea) {
            const auto box = MapData::WarVKCloudSeaBoxFor(*value.cloudSea, *ground.value);
            if (box.Succeeded() && box.value) {
                const auto& b = *box.value;
                result.insert("cloud_sea_box", QJsonObject{{"center", QJsonArray{b.centerX, b.centerY, b.centerZ}},
                    {"size", QJsonArray{b.sizeX, b.sizeY, b.sizeZ}}, {"bottom", b.bottom}, {"feather", b.feather},
                    {"query", QString::fromStdString(MapData::WarVKCloudSeaQuery(b))}});
            } else {
                for (const auto& problem : box.diagnostics)
                    warnings.append(QJsonObject{{"code", QString::fromStdString(problem.code)},
                        {"message", QString::fromStdString(problem.message)}});
            }
        }
    } else if (value.cloudSea) {
        for (const auto& problem : ground.diagnostics)
            warnings.append(QJsonObject{{"code", QString::fromStdString(problem.code)}, {"message", QString::fromStdString(problem.message)}});
    }
    result.insert("warnings", warnings);
    if (!value.skyModel.empty()) {
        // 是否已在地图里（含待保存导入）；不在时也可能是游戏自带路径，只报告，不拒绝。
        const auto key = QtProject::EditorProject::AssetKey(QString::fromStdString(value.skyModel));
        const auto assets = project.AssetEntries();
        result.insert("sky_model_in_map", std::any_of(assets.begin(), assets.end(), [&](const auto& entry) {
            return QtProject::EditorProject::AssetKey(entry.path) == key;
        }));
    }
    return result;
}

// 只改请求里出现的项；camera_limits.enabled 为 false 表示取消镜头限制，far_doodad_types 为整表替换。
Result<WarVKMapSettings> Merge(WarVKMapSettings settings, const QJsonObject& arguments) {
    const auto fail = [](const char* code, std::string message) {
        return Result<WarVKMapSettings>::Failure("MapData.WarVK", code, std::move(message));
    };
    if (arguments.contains("timing")) {
        const auto timing = arguments.value("timing").toString();
        if (timing == "timer") settings.timing = MapData::WarVKInitTiming::AfterInitialization;
        else if (timing == "main") settings.timing = MapData::WarVKInitTiming::MainStart;
        else return fail("WARVK_TIMING_INVALID", "timing 只能是 timer 或 main。");
    }
    if (arguments.contains("camera_limits")) {
        const auto object = arguments.value("camera_limits").toObject();
        const auto enabled = object.value("enabled");
        if (!enabled.isBool()) return fail("WARVK_CAMERA_NUMBER", "camera_limits 须含 enabled。");
        if (!enabled.toBool()) settings.cameraLimits.reset();
        else {
            const auto distance = object.value("max_distance"), nearClip = object.value("near_clip");
            if (!distance.isDouble() || !nearClip.isDouble())
                return fail("WARVK_CAMERA_NUMBER", "启用镜头限制时须给出数值 max_distance 与 near_clip。");
            settings.cameraLimits = MapData::WarVKCameraLimits{distance.toDouble(), nearClip.toDouble()};
        }
    }
    if (arguments.contains("far_doodad_types")) {
        std::vector<std::string> types;
        for (const auto& value : arguments.value("far_doodad_types").toArray()) {
            if (!value.isString()) return fail("WARVK_FAR_TYPE_INVALID", "far_doodad_types 只接受四字符物编 ID 字符串。");
            auto next = MapData::SetWarVKFarDoodadType(std::move(types), value.toString().toStdString(), true);
            if (!next.Succeeded() || !next.value) return {std::nullopt, next.diagnostics};
            types = std::move(*next.value);
        }
        settings.farDoodadTypes = std::move(types);
    }
    if (arguments.contains("sky_model")) {
        const auto sky = arguments.value("sky_model");
        if (!sky.isString()) return fail("WARVK_SKY_MODEL_INVALID", "sky_model 须为字符串；空串取消天空模型。");
        // 正斜杠统一为原版路径用的反斜杠。
        settings.skyModel = sky.toString().trimmed().replace(QLatin1Char('/'), QLatin1Char('\\')).toStdString();
    }
    if (arguments.contains("sun_direction")) {
        const auto object = arguments.value("sun_direction").toObject();
        const auto enabled = object.value("enabled");
        if (!enabled.isBool()) return fail("WARVK_SUN_NUMBER", "sun_direction 须含 enabled。");
        if (!enabled.toBool()) settings.sunDirection.reset();
        else {
            const auto azimuth = object.value("azimuth"), elevation = object.value("elevation");
            if (!azimuth.isDouble() || !elevation.isDouble())
                return fail("WARVK_SUN_NUMBER", "启用太阳方向时须给出数值 azimuth 与 elevation（度）。");
            settings.sunDirection = MapData::WarVKSunDirection{azimuth.toDouble(), elevation.toDouble()};
        }
    }
    if (arguments.contains("sun_color")) {
        const auto object = arguments.value("sun_color").toObject();
        const auto enabled = object.value("enabled");
        if (!enabled.isBool()) return fail("WARVK_SUN_NUMBER", "sun_color 须含 enabled。");
        if (!enabled.toBool()) settings.sunColor.reset();
        else {
            const auto red = object.value("red"), green = object.value("green"), blue = object.value("blue");
            const auto intensity = object.value("intensity");
            if (!red.isDouble() || !green.isDouble() || !blue.isDouble() || !intensity.isDouble())
                return fail("WARVK_SUN_NUMBER", "启用太阳颜色时须给出数值 red、green、blue（0–1）与 intensity。");
            settings.sunColor = MapData::WarVKSunColor{red.toDouble(), green.toDouble(), blue.toDouble(), intensity.toDouble()};
        }
    }
    if (arguments.contains("cloud_sea")) {
        const auto object = arguments.value("cloud_sea").toObject();
        const auto enabled = object.value("enabled");
        if (!enabled.isBool()) return fail("WARVK_CLOUD_SEA_NUMBER", "cloud_sea 须含 enabled。");
        if (!enabled.toBool()) settings.cloudSea.reset();
        else {
            const auto top = object.value("top"), cover = object.value("cover");
            if (!top.isDouble() || !cover.isDouble())
                return fail("WARVK_CLOUD_SEA_NUMBER", "启用云海时须给出数值 top（世界 z）与 cover（0–1）。");
            settings.cloudSea = MapData::WarVKCloudSea{top.toDouble(), cover.toDouble()};
        }
    }
    for (const auto& [name, target] : {std::pair{"celestial_motion", &settings.celestialMotion},
             std::pair{"time_color_grading", &settings.timeColorGrading}}) {
        if (!arguments.contains(QLatin1String(name))) continue;
        const auto text = arguments.value(QLatin1String(name)).toString();
        if (text == "default") target->reset();
        else if (text == "on" || text == "off") *target = text == "on";
        else return fail("WARVK_LIGHTING_SWITCH", std::string(name) + " 只能是 default、on 或 off。");
    }
    if (const auto valid = MapData::ValidateWarVKMapSettings(settings); !valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    return Result<WarVKMapSettings>::Success(std::move(settings));
}

QJsonObject Rawcode() { return {{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}; }
} // namespace

std::vector<LiveWarVKTool> LiveWarVKTools() {
    std::vector<LiveWarVKTool> tools;
    tools.push_back({"map_warvk_get", "读取地图设置 → WarVK：镜头远距上限、近裁剪、执行时机、远景装饰物类型、天空模型、太阳方向与颜色、"
        "昼夜光照开关（含实际写出值）、云海、保存时写出的指令、提醒（warnings），以及当前地形的地面范围与云海盒。",
        Mcp::ToolPermission::Query, {}, {}, [](QtProject::EditorProject& project, const QJsonObject&) { return SettingsJson(project); }, {}});
    QJsonObject set{
        {"timing", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"timer", "main"}}}},
        {"camera_limits", QJsonObject{{"type", "object"}, {"additionalProperties", false},
            {"properties", QJsonObject{
                {"enabled", QJsonObject{{"type", "boolean"}}},
                {"max_distance", QJsonObject{{"type", "number"}, {"minimum", MapData::kWarVKCameraMaxDistanceMin},
                    {"maximum", MapData::kWarVKCameraMaxDistanceMax}}},
                {"near_clip", QJsonObject{{"type", "number"}, {"minimum", MapData::kWarVKCameraNearClipMin},
                    {"maximum", MapData::kWarVKCameraNearClipMax}}}}},
            {"required", QJsonArray{"enabled"}}}},
        {"far_doodad_types", QJsonObject{{"type", "array"}, {"maxItems", static_cast<int>(MapData::kWarVKMaxFarDoodadTypes)},
            {"items", Rawcode()}}},
        {"sky_model", QJsonObject{{"type", "string"}, {"maxLength", static_cast<int>(MapData::kWarVKSkyModelMaxBytes)}}},
        {"sun_direction", QJsonObject{{"type", "object"}, {"additionalProperties", false},
            {"properties", QJsonObject{
                {"enabled", QJsonObject{{"type", "boolean"}}},
                {"azimuth", QJsonObject{{"type", "number"}, {"minimum", 0}, {"exclusiveMaximum", MapData::kWarVKSunAzimuthMax}}},
                {"elevation", NumberSchema(MapData::kWarVKSunElevationMin, MapData::kWarVKSunElevationMax)}}},
            {"required", QJsonArray{"enabled"}}}},
        {"sun_color", QJsonObject{{"type", "object"}, {"additionalProperties", false},
            {"properties", QJsonObject{
                {"enabled", QJsonObject{{"type", "boolean"}}},
                {"red", NumberSchema(0, 1)}, {"green", NumberSchema(0, 1)}, {"blue", NumberSchema(0, 1)},
                {"intensity", NumberSchema(0, MapData::kWarVKSunIntensityMax)}}},
            {"required", QJsonArray{"enabled"}}}},
        {"celestial_motion", SwitchSchema()},
        {"time_color_grading", SwitchSchema()},
        {"cloud_sea", QJsonObject{{"type", "object"}, {"additionalProperties", false},
            {"properties", QJsonObject{
                {"enabled", QJsonObject{{"type", "boolean"}}},
                {"top", NumberSchema(-MapData::kWarVKCoordinateMax, MapData::kWarVKCoordinateMax)},
                {"cover", NumberSchema(0, 1)}}},
            {"required", QJsonArray{"enabled"}}}}};
    tools.push_back({"map_warvk_set", "修改地图设置 → WarVK（只改给出的项；camera_limits.enabled=false 取消镜头限制，far_doodad_types 整表替换，"
        "超过 16 个拒绝；sky_model 为天空模型路径，空串取消，超过 259 字节拒绝，正斜杠按反斜杠写；"
        "sun_direction 的 azimuth 同单位朝向（0 东、90 北），elevation 为仰角，写出光线方向向量；sun_color 各分量 0–1、强度 0–10；"
        "celestial_motion / time_color_grading 为昼夜光照循环开关，default 为不设置：设了太阳方向/颜色时不设置按 off 写出，"
        "on 会让日夜循环每帧覆盖方向/颜色（返回 warnings），time_color_grading 同时开关整个昼夜系统；"
        "cloud_sea 为云海，top 为盒顶世界 z、cover 为覆盖度 0–1，盒子按地图中心、1.5 倍地图尺寸和最低地面以下 100 计算，"
        "顶高不高于盒底或盒高超过水平尺寸时保存拒绝，见 map_warvk_get 的 map_ground 与 cloud_sea_box）。"
        "一次调用是地图设置撤销栈上的一条记录。", Mcp::ToolPermission::Edit, set, {}, {},
        [](QtProject::EditorProject& project, const QJsonObject& arguments) -> QJsonObject {
            const auto current = project.QueryWarVKMapSettings();
            if (!current.Succeeded() || !current.value) return Mcp::Failure(current);
            const auto merged = Merge(*current.value, arguments);
            if (!merged.Succeeded() || !merged.value) return Mcp::Failure(merged);
            const auto status = project.EditWarVKMapSettings(*merged.value, QObject::tr("AI 修改 WarVK 设置"));
            if (!status.Succeeded()) return Mcp::Failure(status);
            return SettingsJson(project);
        }});
    tools.push_back({"map_warvk_far_doodad_set", "把一个装饰物类型标为或取消 WarVK 远景（同物编装饰物的“远景”勾选）；已标 16 个时第 17 个拒绝。",
        Mcp::ToolPermission::Edit, QJsonObject{{"rawcode", Rawcode()}, {"far", QJsonObject{{"type", "boolean"}}}},
        QJsonArray{"rawcode", "far"}, {},
        [](QtProject::EditorProject& project, const QJsonObject& arguments) -> QJsonObject {
            const auto status = project.SetWarVKFarDoodadType(arguments.value("rawcode").toString().toStdString(),
                arguments.value("far").toBool());
            if (!status.Succeeded()) return Mcp::Failure(status);
            return SettingsJson(project);
        }});
    AppendSurfaceMaterialTools(tools);
    return tools;
}

} // namespace War3Map::QtApp
