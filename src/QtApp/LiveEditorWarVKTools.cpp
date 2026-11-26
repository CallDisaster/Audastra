#include "LiveEditorWarVKTools.h"

#include "EditorProject.h"
#include "McpToolInternal.h"

#include <cmath>

namespace War3Map::QtApp {
namespace {
using MapData::WarVKMapSettings;

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
        {"commands", commands},
        {"protocol_query", QString::fromUtf8(MapData::kWarVKProtocolQuery.data(), static_cast<qsizetype>(MapData::kWarVKProtocolQuery.size()))},
        {"last_error_query", QString::fromUtf8(MapData::kWarVKLastErrorQuery.data(), static_cast<qsizetype>(MapData::kWarVKLastErrorQuery.size()))},
        {"revision", QString::number(project.editRevision_.Revision())}};
    result.insert("camera_limits", value.cameraLimits
        ? QJsonValue(QJsonObject{{"enabled", true}, {"max_distance", value.cameraLimits->maxDistance},
            {"near_clip", value.cameraLimits->nearClip}})
        : QJsonValue(QJsonObject{{"enabled", false}}));
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
    if (const auto valid = MapData::ValidateWarVKMapSettings(settings); !valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    return Result<WarVKMapSettings>::Success(std::move(settings));
}

QJsonObject Rawcode() { return {{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}; }
} // namespace

std::vector<LiveWarVKTool> LiveWarVKTools() {
    std::vector<LiveWarVKTool> tools;
    tools.push_back({"map_warvk_get", "读取地图设置 → WarVK：镜头远距上限、近裁剪、执行时机、远景装饰物类型及保存时写出的指令。",
        Mcp::ToolPermission::Query, {}, {}, SettingsJson, {}});
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
            {"items", Rawcode()}}}};
    tools.push_back({"map_warvk_set", "修改地图设置 → WarVK（只改给出的项；camera_limits.enabled=false 取消镜头限制，far_doodad_types 整表替换，"
        "超过 16 个拒绝）。一次调用是地图设置撤销栈上的一条记录。", Mcp::ToolPermission::Edit, set, {}, {},
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
    return tools;
}

} // namespace War3Map::QtApp
