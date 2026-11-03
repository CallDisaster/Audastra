#include "McpToolInternal.h"
#include "War3MapCore/Services/MinimapPreview.h"
#include <QByteArray>

namespace War3Map::Mcp {
Result<Services::CurrentMinimapPreviewInput> ParseMinimapPreviewArguments(const QJsonObject& args) {
    using R=Result<Services::CurrentMinimapPreviewInput>; Services::CurrentMinimapPreviewInput input;
    if(args.contains("source")&&!args["source"].isString())return R::Failure("Minimap.Preview","MINIMAP_PREVIEW_ARGUMENT","source须为字符串。");
    const auto source=args.value("source").toString("saved");
    if(source!="saved"&&source!="current")return R::Failure("Minimap.Preview","MINIMAP_PREVIEW_ARGUMENT","source须为current或saved。");
    for(const auto* key:{"include_rgba","units","starts","icons"})
        if(args.contains(key)&&!args[key].isBool())return R::Failure("Minimap.Preview","MINIMAP_PREVIEW_ARGUMENT","显示开关须为布尔值。");
    if(args.contains("layers")){
        const auto value=args["layers"].toDouble(-1);
        if(value<0||value>0x4af||value!=int(value)||(int(value)&~0x4af))
            return R::Failure("Minimap.Preview","MINIMAP_PREVIEW_ARGUMENT","图层位无效。");
        input.layers=std::uint32_t(value);
    }
    if(source=="saved"&&(args.contains("layers")||args.contains("units")||args.contains("starts")||args.contains("icons")||args.contains("game_root")))
        return R::Failure("Minimap.Preview","MINIMAP_PREVIEW_ARGUMENT","已保存视图不接受生成图层或游戏目录。");
    if(args.contains("game_root")){auto path=ParseAbsolutePath(args,"game_root");if(!path.Succeeded())return {{},path.diagnostics};input.gameRoot=*path.value;}
    input.units=args["units"].toBool();input.starts=args["starts"].toBool();input.icons=args["icons"].toBool();
    return R::Success(std::move(input));
}
QJsonObject MinimapPreviewJson(const Services::MinimapPreview& value,bool includeRgba){
    QJsonObject result{{"ok",true},{"read_only",true},{"saved_map_modified",false},
        {"content",value.current?"current_minimap":"saved_minimap_base"},
        {"document_revision",QString::number(value.documentRevision)}, {"layers",qint64(value.layers)},
        {"generation_ms",value.generationMilliseconds},{"source_sha256",QString::fromStdString(value.sourceSha256)},
        {"present",value.present},{"member_sha256",QString::fromStdString(value.memberSha256)},
        {"rgba_sha256",QString::fromStdString(value.rgbaSha256)},
        {"width",qint64(value.image.width)},{"height",qint64(value.image.height)}};
    if(value.present&&includeRgba){result["pixel_format"]="rgba8_top_down";result["rgba_base64"]=QString::fromLatin1(QByteArray(
        reinterpret_cast<const char*>(value.image.rgba.data()),qsizetype(value.image.rgba.size())).toBase64());}
    return result;
}
ToolDefinition MinimapPreviewToolDefinition() {
    ToolDefinition tool;
    tool.name = "query_minimap_preview";
    tool.description = "只读查看已保存底图或按当前文档生成小地图。活动工程的current在后台生成，pending时稍后查询；不改地图。独立文件生成需提供game_root。";
    tool.maxResponseBytes = 2U * 1024U * 1024U;
    tool.inputSchema = BasicSchema({
        {"source_path", QJsonObject{{"type", "string"}, {"minLength", 1}}},
        {"expected_source_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
        {"source",QJsonObject{{"type","string"},{"enum",QJsonArray{"current","saved"}},{"default","saved"}}},
        {"game_root",QJsonObject{{"type","string"}}},
        {"layers",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",0x4af}}},
        {"units",QJsonObject{{"type","boolean"}}},{"starts",QJsonObject{{"type","boolean"}}},{"icons",QJsonObject{{"type","boolean"}}},
        {"project_identity",QJsonObject{{"type","string"}}}, {"expected_revision",QJsonObject{{"type","string"}}},
        {"include_rgba", QJsonObject{{"type", "boolean"}, {"default", false}}}},
        {"source_path", "expected_source_sha256"});
    tool.handler = [](const QJsonObject& arguments) -> QJsonValue {
        const auto path = ParseAbsolutePath(arguments, "source_path");
        if (!path.Succeeded()) return Failure(path);
        const auto expected = arguments.value("expected_source_sha256").toString().toStdString();
        if (!IsLowerHexSha256(expected) || (!arguments.value("include_rgba").isUndefined()
            && !arguments.value("include_rgba").isBool()))
            return Failure("MINIMAP_PREVIEW_ARGUMENT", "须提供完整小写源SHA，include_rgba须为布尔值。");
        auto input=ParseMinimapPreviewArguments(arguments);if(!input.Succeeded())return Failure(input);
        auto captured = arguments.value("source").toString("saved")=="current"
            ? Services::OpenCurrentMinimapPreview(*path.value,expected,input.value->gameRoot,input.value->layers,input.value->units,input.value->starts,input.value->icons)
            : Services::OpenMinimapPreview(*path.value, expected);
        if (!captured.Succeeded()) return Failure(captured);
        return MinimapPreviewJson(*captured.value,arguments.value("include_rgba").toBool());
    };
    return tool;
}
void RegisterMinimapPreviewTool(McpServer& server) {
    server.RegisterTool(MinimapPreviewToolDefinition());
}
} // namespace War3Map::Mcp
