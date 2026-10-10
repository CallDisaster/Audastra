#include "McpToolInternal.h"
#include "War3MapCore/Render/ModelAnimationCapture.h"
#include <QDir>
#include <QJsonDocument>
#include <QRegularExpression>

namespace War3Map::Mcp {
void RegisterModelAnimationTool(McpServer& server) {
    ToolDefinition tool;
    tool.name="capture_model_animation";
    tool.description="游戏资源只读、多实例显式时间动画预览；独立无窗口渲染，不是活动Qt工程截图。TSV默认仅模型；开头 particles\\ttrue 显式启用同帧粒子，time的局部/全局毫秒保持原值，纯粒子须显式camera。默认严格；world_preview=true 显式使用世界页的静态保留/坏部件隔离近似，原诊断和未显示网格记录保留。相同TSV内连续帧复用静态资产。返回报告路径，质量仅PreviewApproximate。";
    tool.inputSchema=BasicSchema({{"plan_tsv",QJsonObject{{"type","string"},{"maxLength",1048576},
        {"description","UTF-8 TSV；particles\\ttrue 显式启用粒子，默认false。time实例局部/全局毫秒不自动修正；纯粒子须给camera。"}}},
        {"output_name",QJsonObject{{"type","string"},{"pattern","^[A-Za-z0-9_-]{1,64}$"}}},
        {"world_preview",QJsonObject{{"type","boolean"},{"default",false},
            {"description","默认false保持严格；true显式复用世界页局部恢复，报告仍为PreviewApproximate并保留未显示geoset/原诊断。"}}},
        {"width",QJsonObject{{"type","integer"},{"minimum",64},{"maximum",2048}}},
        {"height",QJsonObject{{"type","integer"},{"minimum",64},{"maximum",2048}}}}, {"plan_tsv","output_name"});
    tool.handler=[](const QJsonObject& parameters)->QJsonValue {
        const auto root=qEnvironmentVariable("FORGE_MCP_WAR3_ROOT");
        const auto outputRoot=qEnvironmentVariable("FORGE_MCP_ANIMATION_OUTPUT_ROOT");
        if(root.isEmpty() || outputRoot.isEmpty())return Failure("MCP_ANIMATION_HOST_UNCONFIGURED",
            "宿主须设置 FORGE_MCP_WAR3_ROOT 与 FORGE_MCP_ANIMATION_OUTPUT_ROOT；请求不能指定游戏根或DLL。");
        const auto name=parameters.value("output_name").toString();
        static const QRegularExpression validName(QStringLiteral("\\A[A-Za-z0-9_-]{1,64}\\z"));
        if(!validName.match(name).hasMatch())return Failure("MCP_ANIMATION_OUTPUT_INVALID","输出名称必须为1..64个字母/数字/下划线/连字符。");
        if(!parameters.value("plan_tsv").isString())return Failure("MCP_ANIMATION_PLAN_INVALID","plan_tsv 必须为UTF-8清单。");
        if(parameters.contains("world_preview") && !parameters.value("world_preview").isBool())
            return Failure("MCP_ANIMATION_POLICY_INVALID","world_preview 必须为boolean，不能用字符串代替。");
        const auto text=parameters.value("plan_tsv").toString().toUtf8();
        auto plan=Render::ParseAnimationCapturePlan(std::string_view(text.constData(),static_cast<std::size_t>(text.size())));
        if(!plan.Succeeded())return Failure(plan);
        Render::AnimationCaptureRequest request;
        request.gameRoot=root.toStdWString();
        request.outputDirectory=QDir(outputRoot).absoluteFilePath(name).toStdWString();
        request.plan=std::move(*plan.value);
        request.worldPreview=parameters.value("world_preview").toBool(false);
        // MCP回执只回摘要路径，详细实例/帧元数据在报告文件；不把RGBA/Base64塞进协议。
        for(const auto* key:{"width","height"}) {
            if(!parameters.contains(QLatin1String(key)))continue;
            auto parsed=ParseInt32(parameters,key);if(!parsed.Succeeded())return Failure(parsed);
            if(*parsed.value<64 || *parsed.value>2048)return Failure("MCP_ANIMATION_SIZE_INVALID","尺寸须为64..2048。");
            if(std::string_view(key)=="width")request.width=static_cast<std::uint32_t>(*parsed.value);
            else request.height=static_cast<std::uint32_t>(*parsed.value);
        }
        const auto result=Render::RunModelAnimationCapture(request);
        if(!result.Succeeded())return Failure(result);
        return QJsonObject{{"quality","PreviewApproximate"},{"report",QString::fromStdWString(result.value->report.wstring())},
            {"captures",static_cast<int>(request.plan.captures.size())},{"active_qt_document",false},
            {"world_preview",request.worldPreview},{"capture_policy",request.worldPreview?"WorldPreview":"Strict"}};
    };
    server.RegisterTool(std::move(tool));
}
} // namespace War3Map::Mcp
