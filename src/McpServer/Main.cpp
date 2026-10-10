#include "McpServer.h"
#include "McpTools.h"
#include "McpResources.h"
#include "McpProductTools.h"
#include "McpHttpTransport.h"
#include "AudastraVersion.generated.h"

#include <QCoreApplication>
#include <QCommandLineParser>

#include <filesystem>
#include <iostream>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("war3-map-mcp");
    QCoreApplication::setApplicationVersion(QString::fromUtf8(War3Map::BuildInfo::Version));

    QCommandLineParser parser;
    parser.setApplicationDescription("War3MapEditor64 MCP Server (stdio / local Streamable HTTP)");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption workspaceOpt(
        {"w", "workspace"},
        "地图项目根目录（包含 lni/ 和 map/ 子目录）",
        "path");
    QCommandLineOption uiDataOpt(
        {"u", "ui-data"},
        "Warcraft/YDWE UI 数据根目录",
        "path");
    parser.addOption(workspaceOpt);
    parser.addOption(uiDataOpt);
    QCommandLineOption luaTargetOpt("lua-map-target", "可信宿主选择的Lua完整保存档案", "path");
    parser.addOption(luaTargetOpt);
    QCommandLineOption skillsOpt("skills", "当前版本可发现的Skill安装目录", "path");
    QCommandLineOption httpPortOpt("http-port", "开启回环Streamable HTTP（不设置则使用stdio）", "port");
    QCommandLineOption httpTokenEnvOpt("http-token-env", "读取本机访问令牌的环境变量名称", "name", "FORGE_MCP_ACCESS_TOKEN");
    parser.addOption(skillsOpt);
    parser.addOption(httpPortOpt);
    parser.addOption(httpTokenEnvOpt);
    QCommandLineOption feedbackDataOpt("feedback-data-dir", "使用显式本地反馈队列目录；不恢复用户同意、不发送网络请求", "path");
    parser.addOption(feedbackDataOpt);
    parser.process(app);

    War3Map::Mcp::McpServer server;
    if (parser.isSet(luaTargetOpt)) {
        auto target = War3Map::Services::LoadLuaMapTargetConfiguration(
            std::filesystem::path(parser.value(luaTargetOpt).toStdWString()));
        if (!target.Succeeded() || !target.value) {
            for (const auto& diagnostic : target.diagnostics)
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            return 2;
        }
        server.SetLuaMapTarget(std::move(*target.value));
    }

    const std::filesystem::path workspace = parser.isSet(workspaceOpt)
        ? std::filesystem::path(parser.value(workspaceOpt).toStdWString())
        : std::filesystem::current_path();
    const std::filesystem::path uiData = parser.isSet(uiDataOpt)
        ? std::filesystem::path(parser.value(uiDataOpt).toStdWString())
        : std::filesystem::path{};

    server.SetWorkspace(workspace, uiData);

    // 注册工具和资源（内部会加载文档）
    War3Map::Mcp::RegisterAllTools(server);
    if (parser.isSet(feedbackDataOpt)) {
        // 启动配置限定的数据根供独立安装/自动化使用，协议请求无权切换目录或开启发送。
        auto isolated = War3Map::Feedback::FeedbackQueue::Open(
            std::filesystem::path(parser.value(feedbackDataOpt).toStdWString()));
        if (!isolated.Succeeded()) {
            for (const auto& diagnostic : isolated.diagnostics)
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            return 2;
        }
        War3Map::Mcp::ProductToolHost isolatedHost;
        isolatedHost.feedback = [queue = *isolated.value] {
            return War3Map::Result<std::shared_ptr<War3Map::Feedback::FeedbackQueue>>::Success(queue);
        };
        isolatedHost.nowUnixSeconds = War3Map::Mcp::DefaultProductToolHost().nowUnixSeconds;
        War3Map::Mcp::RegisterProductTools(server, std::move(isolatedHost));
    }
    War3Map::Mcp::RegisterAllResources(server, parser.isSet(skillsOpt)
        ? std::filesystem::path(parser.value(skillsOpt).toStdWString()) : std::filesystem::path{});

    if (parser.isSet(httpPortOpt)) {
        bool valid = false;
        const uint port = parser.value(httpPortOpt).toUInt(&valid);
        if (!valid || port > 65535) {
            std::cerr << "MCP_HTTP_PORT_INVALID: port must be 0..65535\n";
            return 2;
        }
        War3Map::Mcp::McpHttpTransport http(server);
        War3Map::Mcp::McpHttpOptions options;
        options.port = static_cast<quint16>(port);
        const QByteArray tokenVariable = parser.value(httpTokenEnvOpt).toUtf8();
        options.bearerToken = qgetenv(tokenVariable.constData());
        QString error;
        if (!http.Start(std::move(options), &error)) {
            std::cerr << "MCP_HTTP_START_FAILED: " << error.toStdString() << '\n';
            return 2;
        }
        // stdout 仍不输出地址或凭据；不会把访问令牌复制到日志。
        std::cerr << "MCP_HTTP_LISTENING: " << http.EndpointUrl().toString().toStdString() << '\n';
        return app.exec();
    }

    // 运行 stdio 事件循环（阻塞直到 stdin 关闭）
    server.RunStdio();
    return 0;
}
