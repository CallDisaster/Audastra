#include "McpWorldViewportTools.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <limits>
#include <stdexcept>
#include <iostream>

using namespace War3Map::Mcp;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QString Code(const QJsonObject& result) { return result.value("diagnostics").toArray().first().toObject().value("code").toString(); }
void Contracts() {
    const auto tools = WorldViewportToolDefinitions(); Require(tools.size() == 4, "four shared viewport tools");
    int calls = 0;
    WorldViewportToolHost host{[&](const QString& name, const QJsonObject&) {
        ++calls; return QJsonObject{{"ok", true}, {"name", name}};
    }};
    QJsonObject args{{"project_identity", "fixture"}, {"expected_revision", "0"}};
    Require(Code(InvokeWorldViewportTool("world_viewport_camera_read", args, {})) == "WORLD_VIEWPORT_UNSUPPORTED",
        "stdio must report no viewport, not an empty image");
    Require(InvokeWorldViewportTool("world_viewport_camera_read", args, host).value("ok").toBool() && calls == 1, "read forwards");
    auto invalid = args; invalid["path"] = "injected";
    Require(!InvokeWorldViewportTool("world_viewport_camera_read", invalid, host).value("ok").toBool() && calls == 1,
        "no external file paths");
    invalid = args; invalid["expected_revision"] = 0;
    Require(!InvokeWorldViewportTool("world_viewport_camera_read", invalid, host).value("ok").toBool() && calls == 1,
        "identity cannot lose uint64 precision");
    args["expected_viewport_revision"] = "0";
    args["camera"] = QJsonObject{{"target_x", 0}, {"target_y", 0}, {"target_z", 0},
        {"distance", 2048}, {"yaw_radians", 0}, {"pitch_radians", 0.8}};
    Require(!InvokeWorldViewportTool("world_viewport_camera_set", args, host).value("ok").toBool() && calls == 1,
        "dry-run choice is explicit");
    args["dry_run"] = true;
    Require(InvokeWorldViewportTool("world_viewport_camera_set", args, host).value("ok").toBool() && calls == 2, "preview forwards");
    invalid = args; auto camera = invalid.value("camera").toObject(); camera["target_x"] = "0"; invalid["camera"] = camera;
    Require(!InvokeWorldViewportTool("world_viewport_camera_set", invalid, host).value("ok").toBool() && calls == 2,
        "numeric strings are not accepted");
    invalid = args; camera = invalid.value("camera").toObject(); camera["extra"] = 1; invalid["camera"] = camera;
    Require(!InvokeWorldViewportTool("world_viewport_camera_set", invalid, host).value("ok").toBool() && calls == 2,
        "camera accepts only six actual fields");
    args.remove("camera"); args.remove("dry_run"); args["max_rgba_bytes"] = 4;
    Require(InvokeWorldViewportTool("world_viewport_capture", args, host).value("ok").toBool() && calls == 3, "capture bounded budget");
    invalid = args; invalid["max_rgba_bytes"] = 4.5;
    Require(!InvokeWorldViewportTool("world_viewport_capture", invalid, host).value("ok").toBool() && calls == 3, "integral byte budget");
    invalid = args; invalid["max_rgba_bytes"] = 64 * 1024 * 1024 + 1;
    Require(!InvokeWorldViewportTool("world_viewport_capture", invalid, host).value("ok").toBool() && calls == 3, "bounded memory");
    McpServer server; RegisterWorldViewportTools(server);
    ToolPolicy policy; policy.allowEdit = false; server.SetToolPolicy(policy);
    const auto response = server.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    Require(response && response->result.toObject().value("tools").toArray().size() == 3, "edit denied but three read tools remain");
    Require(tools[1].permission == ToolPermission::Edit && tools[0].permission == ToolPermission::Query,
        "camera writes honor edit policy");
    const QString png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a0BsAAAAASUVORK5CYII=";
    Require(server.SetToolHandler("world_viewport_capture_result", [png](const QJsonObject&) {
        return QJsonObject{{"ok", true}, {"status", "ready"}, {"image", QJsonObject{{"mime_type", "image/png"},
            {"encoding", "base64"}, {"data", png}}}};
    }), "bind trusted image host");
    const QByteArray request = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"world_viewport_capture_result","arguments":{"project_identity":"fixture","expected_revision":"0","capture_id":"fixture","include_png":true}}})";
    const auto imageResponse = server.HandleMessage(request); Require(imageResponse && !imageResponse->error, "image response");
    const auto content = imageResponse->result.toObject().value("content").toArray();
    Require(content.size() == 2 && content[0].toObject().value("type") == QJsonValue("text")
        && !content[0].toObject().value("text").toString().contains(png)
        && content[1].toObject().value("type") == QJsonValue("image") && content[1].toObject().value("data") == QJsonValue(png),
        "PNG uses native MCP image content, no repeated base64 text");
    Require(server.SetToolHandler("world_viewport_capture_result", [](const QJsonObject&) {
        return QJsonObject{{"ok", true}, {"image", QJsonObject{{"mime_type", "image/png"}, {"encoding", "base64"}, {"data", "invalid"}}}};
    }), "replace invalid host");
    const auto invalidImage = server.HandleMessage(request);
    Require(invalidImage && invalidImage->result.toObject().value("isError").toBool()
        && invalidImage->result.toObject().value("content").toArray().size() == 1, "malformed host image is not delivered");
    Require(server.SetToolHandler("world_viewport_capture_result", [](const QJsonObject&) {
        return QJsonObject{{"ok", true}, {"image", QJsonObject{{"mime_type", "image/png"}, {"encoding", "base64"},
            {"data", "iVBORw0KGgo invalid"}}}};
    }), "replace invalid prefixed host image");
    const auto invalidPrefixed = server.HandleMessage(request);
    Require(invalidPrefixed && invalidPrefixed->result.toObject().value("isError").toBool()
        && invalidPrefixed->result.toObject().value("content").toArray().size() == 1,
        "PNG prefix alone must not bypass strict base64 validation");
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try { Contracts(); std::cout << "PASS viewport schemas, bounds and permissions\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
