#include "McpHLuaTools.h"
#include "McpToolInternal.h"
#include "War3MapCore/HLua/Test.h"
#include "War3MapCore/IO/FileBytes.h"
#include <QJsonDocument>
#include <QUuid>
#include <map>

namespace War3Map::Mcp {
namespace {
struct Context { std::shared_ptr<HLua::TestSession> session; QString identity; };
QJsonObject Json(std::string text) { return QJsonDocument::fromJson(QByteArray::fromStdString(text)).object(); }
Result<HLua::BuildRequest> Parse(const QJsonObject& args) {
    using Output = Result<HLua::BuildRequest>;
    const QStringList allowed{"project_path","framework_root","runtime_root","build_root","base_map_path",
        "output_path","workspace_path","game_root","target","expected_project_sha256","offset","limit",
        "tool_root","ui_root","helper_root","ydwe_root","engine","cloaked"};
    for (auto it = args.begin(); it != args.end(); ++it) if (!allowed.contains(it.key()))
        return Output::Failure("HLua", "HLUA_ARGUMENT", "h-lua 工具不接受额外字段。");
    for (auto it=args.begin();it!=args.end();++it) {
        if(it.key()=="offset" || it.key()=="limit") continue;
        if(it.key()=="cloaked" ? !it.value().isBool() : !it.value().isString())
            return Output::Failure("HLua","HLUA_ARGUMENT","h-lua 参数类型与工具定义不符；没有选择默认构建或环境。");
    }
    if(args.contains("engine") && args.value("engine")!="owned" && args.value("engine")!="ydwe")
        return Output::Failure("HLua","HLUA_ARGUMENT","未知 Lua 环境。");
    HLua::BuildRequest request;
    for (const auto name : {"offset","limit"}) if (args.contains(name)) {
        const auto value=args.value(name);
        const auto number=value.toDouble(-1);
        if (!value.isDouble() || number!=value.toInt(-1) || number<(QString(name)=="limit" ? 1 : 0)
            || number>(QString(name)=="limit" ? 128 : 65536)) return Output::Failure("HLua","HLUA_ARGUMENT","查询分页必须是范围内的整数。");
    }
    const std::map<std::string, std::filesystem::path*> paths{
        {"project_path",&request.project},{"framework_root",&request.framework.repository},
        {"runtime_root",&request.framework.runtime},{"build_root",&request.framework.build},
        {"base_map_path",&request.baseMap},{"output_path",&request.target},{"workspace_path",&request.workspace},
        {"game_root",&request.gameRoot}};
    for (const auto& [name, target] : paths) {
        if (name != "project_path" && !args.contains(QString::fromStdString(name))) continue;
        auto path = ParseAbsolutePath(args, name.c_str());
        if (!path.Succeeded()) return {std::nullopt, std::move(path.diagnostics)};
        *target = std::move(*path.value);
    }
    const auto mode = args.value("target").toString("test");
    if (mode != "test" && mode != "build" && mode != "dist") return Output::Failure("HLua", "HLUA_ARGUMENT", "未知构建目标。");
    request.mode = mode == "test" ? HLua::BuildTarget::Test : mode == "build" ? HLua::BuildTarget::Build : HLua::BuildTarget::Dist;
    if (args.contains("expected_project_sha256")) {
        request.expectedProjectSha256 = args.value("expected_project_sha256").toString().toStdString();
        if (!IsLowerHexSha256(request.expectedProjectSha256)) return Output::Failure("HLua", "HLUA_ARGUMENT", "工程SHA-256无效。");
    }
    return Output::Success(std::move(request));
}
QJsonObject State(const Context& context) {
    if (!context.session) return Failure("HLUA_TEST_NOT_STARTED", "本工具尚未启动独立游戏测试。");
    const auto& launch = context.session->Launch();
    QJsonArray pids;
    if (launch.lease) for (const auto pid : launch.lease->ProcessIds()) pids.append(static_cast<qint64>(pid));
    QJsonObject result{{"ok",true},{"session_id",context.identity},{"active",launch.lease && launch.lease->IsActive()},
        {"launcher_pid",static_cast<qint64>(launch.launcherProcessId)},{"owned_process_ids",pids}};
    auto lua = context.session->QueryLua();
    if (lua.Succeeded()) result.insert("lua", QJsonObject{{"runtime_state",static_cast<int>(lua.value->runtimeState)},
        {"state",static_cast<int>(lua.value->state)},{"message",QString::fromStdString(lua.value->message)},
        {"runtime_message",QString::fromStdString(lua.value->runtimeMessage)},{"result",static_cast<int>(lua.value->resultCode)}});
    else result.insert("lua_mailbox_available",false);
    return result;
}
}
void RegisterHLuaTools(McpServer& server) {
    auto context = std::make_shared<Context>();
    const QJsonObject path{{"type","string"},{"minLength",1},{"maxLength",4096}};
    const QJsonObject properties{{"project_path",path},{"framework_root",path},{"runtime_root",path},{"build_root",path},
        {"base_map_path",path},{"output_path",path},{"workspace_path",path},{"game_root",path},{"tool_root",path},{"ui_root",path},
        {"helper_root",path},{"ydwe_root",path},{"target",QJsonObject{{"type","string"},{"enum",QJsonArray{"test","build","dist"}}}},
        {"expected_project_sha256",QJsonObject{{"type","string"},{"pattern","^[0-9a-f]{64}$"}}},
        {"offset",QJsonObject{{"type","integer"},{"minimum",0},{"maximum",65536}}},
        {"limit",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",128}}},
        {"engine",QJsonObject{{"type","string"},{"enum",QJsonArray{"owned","ydwe"}}}},
        {"cloaked",QJsonObject{{"type","boolean"}}}};
    for (const auto& entry : {std::pair{"hlua_inspect",ToolPermission::Query},std::pair{"hlua_ids",ToolPermission::Query},
            std::pair{"hlua_build",ToolPermission::Save},std::pair{"hlua_new",ToolPermission::Save},std::pair{"hlua_test",ToolPermission::External}}) {
        ToolDefinition tool; tool.name = entry.first; tool.permission = entry.second;
        tool.description = "h-lua 共用服务：只读检查/ID、独立构建、新建或专用进程测试；框架由用户提供（AGPL-3.0），编辑器不分发。";
        tool.inputSchema = BasicSchema(properties,{"project_path"}); tool.maxRequestIdBytes = 128; tool.maxResponseBytes = 256 * 1024;
        const auto command = tool.name;
        tool.handler = [context,command](const QJsonObject& args) -> QJsonValue {
            auto parsed = Parse(args);
            if (!parsed.Succeeded()) return Failure(parsed);
            auto& request = *parsed.value;
            if (command == "hlua_inspect") {
                auto result = HLua::InspectProject(request.framework,request.project,request.gameRoot);
                return result.Succeeded() ? Json(HLua::InspectReceiptJson(*result.value,args.value("offset").toInt(0),args.value("limit").toInt(128))) : Failure(result);
            }
            if (command == "hlua_new") {
                auto toolRoot = ParseAbsolutePath(args,"tool_root");
                if (!toolRoot.Succeeded()) return Failure(toolRoot);
                std::filesystem::path ui;
                if (args.contains("ui_root")) { auto root=ParseAbsolutePath(args,"ui_root"); if (!root.Succeeded()) return Failure(root); ui=*root.value; }
                auto result = HLua::CreateProject({request.framework,request.project,*toolRoot.value,request.gameRoot,ui});
                if (!result.Succeeded()) return Failure(result);
                return QJsonObject{{"ok",true},{"base_map",QString::fromStdString(IO::GenericPathUtf8(result.value->path))},
                    {"base_sha256",QString::fromStdString(result.value->sha256)}};
            }
            if (command == "hlua_test") {
                if (context->session && context->session->Launch().lease->IsActive()) return Failure("HLUA_TEST_ALREADY_ACTIVE","本工具已有活动测试，只能结束本工具持有的进程。");
                auto helper = ParseAbsolutePath(args,"helper_root");
                if (!helper.Succeeded()) return Failure(helper);
                HLua::TestRequest test; test.build=request; test.helpers=*helper.value;
                const auto engine=args.value("engine").toString("owned");
                if (engine!="owned" && engine!="ydwe") return Failure("HLUA_ARGUMENT","未知 Lua 环境。");
                test.engine=engine=="owned" ? Services::MapTestLuaEngine::Owned : Services::MapTestLuaEngine::Ydwe;
                if (args.contains("ydwe_root")) { auto root=ParseAbsolutePath(args,"ydwe_root"); if (!root.Succeeded()) return Failure(root); test.ydweRoot=*root.value; }
                if (args.contains("cloaked") && !args.value("cloaked").isBool()) return Failure("HLUA_ARGUMENT","cloaked 需要布尔值。");
                test.cloaked=args.value("cloaked").toBool(true);
                auto result=HLua::StartProjectTest(test);
                if (!result.Succeeded()) return Failure(result);
                context->session=std::move(*result.value); context->identity=QUuid::createUuid().toString(QUuid::WithoutBraces);
                auto response=State(*context); response.insert("build",Json(HLua::BuildReceiptJson(context->session->Build())));
                response.insert("diagnostics",DiagnosticsJson(result.diagnostics).value("diagnostics")); return response;
            }
            request.idsOnly = command == "hlua_ids";
            auto result = HLua::BuildProject(request);
            if (!result.Succeeded()) return Failure(result);
            const auto offset=args.value("offset").toInt(0), limit=args.value("limit").toInt(128);
            if (offset<0 || offset>65536 || limit<1 || limit>128) return Failure("HLUA_ARGUMENT","查询分页超出范围。");
            return Json(HLua::BuildReceiptJson(*result.value,static_cast<std::size_t>(offset),static_cast<std::size_t>(limit)));
        };
        server.RegisterTool(std::move(tool));
    }
    for (const auto name : {"hlua_test_result","hlua_test_stop"}) {
        ToolDefinition tool; tool.name=name; tool.permission=QString(name)=="hlua_test_stop" ? ToolPermission::External : ToolPermission::Query;
        tool.description="只查询或结束本工具启动且会话身份相符的测试；不枚举或结束其他游戏。";
        tool.inputSchema=BasicSchema({{"session_id",QJsonObject{{"type","string"},{"minLength",36},{"maxLength",36}}}},{"session_id"});
        tool.maxRequestIdBytes=128; tool.maxResponseBytes=64*1024;
        tool.handler=[context,stop=QString(name)=="hlua_test_stop"](const QJsonObject& args)->QJsonValue {
            if (args.size()!=1 || args.value("session_id").toString()!=context->identity || !context->session)
                return Failure("HLUA_TEST_IDENTITY","测试会话身份不符。");
            if (stop) { auto result=context->session->Stop(); if (!result.Succeeded()) return Failure(result); }
            auto response=State(*context);
            if (!context->session->Launch().lease->IsActive()) {
                const auto result=context->session->Cleanup(); response.insert("workspace_cleaned",result.Succeeded());
                if (!result.Succeeded()) response.insert("cleanup",Failure(result));
            }
            return response;
        };
        server.RegisterTool(std::move(tool));
    }
}
} // namespace War3Map::Mcp
