#include "McpToolInternal.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include <QByteArray>

namespace War3Map::Mcp {
void RegisterMinimapPreviewTool(McpServer& server) {
    ToolDefinition tool;
    tool.name = "query_minimap_preview";
    tool.description = "只读查看独立源地图中实际已保存的小地图底图；不生成、不改地图，不含工作区未保存编辑或图标。缺失单独报告，损坏拒绝。";
    tool.maxResponseBytes = 2U * 1024U * 1024U;
    tool.inputSchema = BasicSchema({
        {"source_path", QJsonObject{{"type", "string"}, {"minLength", 1}}},
        {"expected_source_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
        {"include_rgba", QJsonObject{{"type", "boolean"}, {"default", false}}}},
        {"source_path", "expected_source_sha256"});
    tool.handler = [](const QJsonObject& arguments) -> QJsonValue {
        const auto path = ParseAbsolutePath(arguments, "source_path");
        if (!path.Succeeded()) return Failure(path);
        const auto expected = arguments.value("expected_source_sha256").toString().toStdString();
        if (!IsLowerHexSha256(expected) || (!arguments.value("include_rgba").isUndefined()
            && !arguments.value("include_rgba").isBool()))
            return Failure("MINIMAP_PREVIEW_ARGUMENT", "须提供完整小写源SHA，include_rgba须为布尔值。");
        auto captured = Services::OpenMinimapPreview(*path.value, expected);
        if (!captured.Succeeded()) return Failure(captured);
        const auto& value = *captured.value;
        QJsonObject result{{"ok", true}, {"read_only", true}, {"saved_map_modified", false},
            {"content", "saved_minimap_base"}, {"source_sha256", QString::fromStdString(value.sourceSha256)},
            {"present", value.present}, {"member_sha256", QString::fromStdString(value.memberSha256)},
            {"rgba_sha256", QString::fromStdString(value.rgbaSha256)},
            {"width", static_cast<qint64>(value.image.width)}, {"height", static_cast<qint64>(value.image.height)}};
        if (value.present && arguments.value("include_rgba").toBool()) {
            result["pixel_format"] = "rgba8_top_down";
            result["rgba_base64"] = QString::fromLatin1(QByteArray(
                reinterpret_cast<const char*>(value.image.rgba.data()),
                static_cast<qsizetype>(value.image.rgba.size())).toBase64());
        }
        return result;
    };
    server.RegisterTool(std::move(tool));
}
} // namespace War3Map::Mcp
