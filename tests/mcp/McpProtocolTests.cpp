#include "McpServer.h"
#include "McpTools.h"
#include "McpWorkspace.h"
#include "McpTriggerGraphJson.h"
#include "McpToolInternal.h"
#include "McpMapDataTools.h"
#include "McpLuaFileTools.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace War3Map::Mcp::ProjectSearchDetail {
// 只测试实际生产的内部元数据准备函数，不新增工作区注入/读盘/写入权限。
Result<std::vector<std::string>> PrepareImportedPaths(std::span<const WorkspaceFile> inventory, std::size_t maxExamined);
}

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void ExpectInvalidParams(
    War3Map::Mcp::McpServer& server,
    const QByteArray& request,
    const char* message) {
    const auto response = server.HandleMessage(request);
    Expect(response.has_value() && response->error.has_value() &&
           response->error->value("code").toInt() ==
               War3Map::Mcp::JsonRpcResponse::kInvalidParams,
        message);
}

void TestToolSurface() {
    War3Map::Mcp::McpServer server;
    War3Map::Mcp::RegisterAllTools(server);
    const auto response = server.HandleMessage(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    Expect(response.has_value() && !response->error.has_value(),
        "tools/list did not return a successful response");
    const QJsonArray tools = response->result.toObject().value("tools").toArray();
    std::set<std::string> names;
    for (const QJsonValue& value : tools) {
        names.insert(value.toObject().value("name").toString().toStdString());
    }

    for (const char* required : {
            "get_revision", "begin_edit", "create_trigger", "append_root_eca",
            "append_argument_call", "set_scalar_argument", "preview_edit",
            "validate_edit", "export_map_candidate", "rollback_edit",
            "install_generated_warvk_module", "save_terrain_blight_candidate",
            "get_terrain_edit_state", "query_terrain_points", "edit_terrain_stroke",
            "describe_map_data", "query_map_data_page", "save_map_edit_candidate", "query_map_trigger_text", "capture_model_animation", "project_search",
            "query_map_archive_member_identities", "query_trigger_graph", "edit_trigger_graph",
            "variables_list", "variables_get", "variables_create", "variables_update", "variables_rename", "variables_delete",
            "entities_query", "entities_set_fields", "entities_copy", "entities_rotate", "entities_create", "entities_delete",
            "feedback.draft", "feedback.list", "feedback.status", "update.status",
            "lua_test_hot_reload", "lua_test_reload_result",
            "lua_files_list", "lua_file_read", "lua_file_write", "map_test_start",
            "object_resource_state", "object_resource_query", "object_resource_inspect"}) {
        Expect(names.contains(required), "required safe MCP tool is missing");
    }
    for (const QJsonValue& value : tools) {
        const QJsonObject tool = value.toObject();
        if (tool.value("name") == "capture_model_animation") {
            const auto schema = tool.value("inputSchema").toObject();
            const auto properties = schema.value("properties").toObject();
            const auto policy = properties.value("world_preview").toObject();
            Expect(policy.value("type") == "boolean" && policy.value("default").isBool() && !policy.value("default").toBool()
                && !schema.value("required").toArray().contains("world_preview")
                && !properties.contains("game_root") && !properties.contains("source_map_path"),
                "world capture policy must be optional/default-strict without exposing new resource paths");
        }
        if (tool.value("name") == "save_map_edit_candidate") {
            const auto properties = tool.value("inputSchema").toObject().value("properties").toObject();
            const auto operation = properties.value("trigger_edits").toObject().value("properties").toObject()
                .value("operations").toObject().value("items").toObject().value("oneOf").toArray();
            Expect(operation.size() == 7, "generic paired trigger batch must publish all seven exact operation schemas");
        }
        if (tool.value("name") == "query_map_trigger_text") {
            const auto properties = tool.value("inputSchema").toObject().value("properties").toObject();
            Expect(properties.value("limit").toObject().value("maximum").toInt() == 65536
                && properties.contains("expected_source_sha256") && properties.contains("expected_catalog_sha256"),
                "public text query exposes bounded pages and the same source identity guards");
        }
        if (tool.value("name") == "imports_add" || tool.value("name") == "lua_file_write") {
            Expect(tool.value("description").toString().startsWith(QStringLiteral("仅编辑器 MCP 端口可用")),
                "standalone server must mark editor-only tools in their description");
        }
        if (tool.value("name") != "save_terrain_blight_candidate") continue;
        const QJsonObject schema = tool.value("inputSchema").toObject();
        const QJsonObject properties = schema.value("properties").toObject();
        Expect(schema.value("additionalProperties").isBool() &&
            !schema.value("additionalProperties").toBool() &&
            properties.contains("expected_source_sha256") &&
            properties.contains("expected_terrain_sha256") &&
            properties.contains("source_map_path") &&
            properties.contains("output_path") &&
            properties.contains("vertex_x") &&
            properties.contains("vertex_y") &&
            properties.contains("radius") &&
            properties.contains("set_blight") &&
            !properties.contains("tool_root") && !properties.contains("game_root") &&
            !properties.contains("replacements") && !properties.contains("member_bytes"),
            "blight save schema lacks identity/bounds or exposes raw member replacement");
    }
    for (const char* forbidden : {
            "set_object_field", "create_object", "delete_object", "export_candidate"}) {
        Expect(!names.contains(forbidden), "legacy unsafe MCP write tool is still registered");
    }

    const auto oldWrite = server.HandleMessage(
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"delete_object","arguments":{}}})");
    Expect(oldWrite.has_value() && oldWrite->error.has_value(),
        "legacy delete_object call did not fail as an unknown tool");

    constexpr auto revision =
        R"("expected_revision":{"generation":"1","source_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","ui_catalog_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"})";
    const QByteArray appendRoot = QByteArray(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"append_root_eca","arguments":{"transaction_id":"1",)") +
        revision +
        R"(,"trigger_id":"2","kind":"action","function_name":"DoNothing","insert_row":0}}})";
    ExpectInvalidParams(server, appendRoot,
        "append_root_eca accepted the undeclared insert_row reorder parameter");

    const QByteArray appendCall = QByteArray(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"append_argument_call","arguments":{"transaction_id":"1",)") +
        revision +
        R"(,"argument_id":"2","function_name":"GetUnit","expected_return_type":"unit"}}})";
    ExpectInvalidParams(server, appendCall,
        "append_argument_call accepted client-controlled expected_return_type");

    ExpectInvalidParams(
        server,
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"get_revision","arguments":{},"extra":true}})",
        "tools/call accepted an unknown top-level parameter");
    ExpectInvalidParams(
        server,
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"begin_edit","arguments":{}}})",
        "begin_edit accepted a missing required expected_revision");
    ExpectInvalidParams(
        server,
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"begin_edit","arguments":{"expected_revision":{"generation":"1","source_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","ui_catalog_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","extra":true}}}})",
        "nested revision object accepted an unknown property");
    ExpectInvalidParams(
        server,
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"begin_edit","arguments":{"expected_revision":{"generation":1,"source_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","ui_catalog_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}}}})",
        "nested revision generation accepted the wrong JSON type");
    const QByteArray wrongCategory = QByteArray(
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"create_trigger","arguments":{"transaction_id":"1",)") +
        revision + R"(,"category_id":"1","name":"x"}}})";
    ExpectInvalidParams(server, wrongCategory,
        "create_trigger category_id accepted the wrong JSON type");
    const QByteArray invalidKind = QByteArray(
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"append_root_eca\",\"arguments\":{"
        "\"transaction_id\":\"1\",") + revision +
        ",\"trigger_id\":\"2\",\"kind\":\"call\","
        "\"function_name\":\"GetUnit\"}}}";
    ExpectInvalidParams(server, invalidKind,
        "append_root_eca schema accepted call as a root ECA kind");
    const QByteArray invalidValueKind = QByteArray(
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"set_scalar_argument\",\"arguments\":{"
        "\"transaction_id\":\"1\",") + revision +
        ",\"argument_id\":\"2\",\"value_kind\":2,\"value\":\"x\"}}}";
    ExpectInvalidParams(server, invalidValueKind,
        "set_scalar_argument schema accepted a non-scalar value_kind");
    ExpectInvalidParams(
        server,
        R"({"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"begin_edit","arguments":{"expected_revision":{"generation":"1","source_sha256":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","ui_catalog_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}}}})",
        "revision schema accepted an uppercase SHA-256 identity");
    const QByteArray zeroTransaction = QByteArray(
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"preview_edit\",\"arguments\":{"
        "\"transaction_id\":\"0\",") + revision + "}}}";
    ExpectInvalidParams(server, zeroTransaction,
        "transaction schema accepted zero as an active transaction ID");

    const QByteArray arbitraryJass = QByteArray(
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"install_generated_warvk_module\",\"arguments\":{"
        "\"transaction_id\":\"1\",") + revision +
        ",\"category_id\":1,\"generated_repository_root\":\"C:\\\\repo\","
        "\"expected_manifest_sha256\":\"920872221B3836A5EFF69D3EC721915B21E0C4B5399C0F09F05B028CF46D27BF\","
        "\"custom_jass\":\"call BJDebugMsg(\\\"unsafe\\\")\"}}}";
    ExpectInvalidParams(server, arbitraryJass,
        "WarVK module installer accepted a client-provided custom_jass payload");
    const QByteArray lowercasePinnedSha = QByteArray(
        "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"install_generated_warvk_module\",\"arguments\":{"
        "\"transaction_id\":\"1\",") + revision +
        ",\"category_id\":1,\"generated_repository_root\":\"C:\\\\repo\","
        "\"expected_manifest_sha256\":\"920872221b3836a5eff69d3ec721915b21e0c4b5399c0f09f05b028cf46d27bf\"}}}";
    ExpectInvalidParams(server, lowercasePinnedSha,
        "WarVK module installer schema accepted a non-canonical manifest identity");

    War3Map::Mcp::McpServer bounded;
    bounded.RegisterTool({"wire-budget", "", QJsonObject{{"type", "object"}},
        [](const QJsonObject&) -> QJsonValue {
            return QJsonObject{{"ok", true}, {"text", QString(400000, QLatin1Char('"'))}};
        }, 1024 * 1024});
    const auto wire = bounded.HandleMessage(
        R"({"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"wire-budget","arguments":{}}})");
    Expect(wire && War3Map::Mcp::SerializeResponse(*wire).size() <= 1024 * 1024 &&
        wire->result.toObject().value("isError").toBool(),
        "outer JSON-RPC escaping exceeded bounded tool wire budget");

    int writes = 0;
    War3Map::Mcp::ToolDefinition writeTool;
    writeTool.name = "bounded-write";
    writeTool.inputSchema = QJsonObject{{"type", "object"}};
    writeTool.permission = War3Map::Mcp::ToolPermission::Edit;
    writeTool.maxResponseBytes = 1024 * 1024;
    writeTool.maxRequestIdBytes = 4096;
    writeTool.handler = [&writes](const QJsonObject&) -> QJsonValue {
        ++writes; return QJsonObject{{"ok", true}};
    };
    bounded.RegisterTool(std::move(writeTool));
    const auto callWrite = [&](const QJsonValue& id) {
        return bounded.HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
            {"method", "tools/call"}, {"params", QJsonObject{{"name", "bounded-write"}, {"arguments", QJsonObject{}}}}})
            .toJson(QJsonDocument::Compact));
    };
    const auto oversized = callWrite(QString(4095, QLatin1Char('x')));
    Expect(oversized && oversized->error && writes == 0,
        "oversized request id executed a write before refusing its response");
    const auto escapedId = callWrite(QString(2048, QLatin1Char('"')));
    Expect(escapedId && escapedId->error && writes == 0,
        "request id budget ignored JSON escaping before a write");
    const auto accepted = callWrite(QString(4094, QLatin1Char('x')));
    Expect(accepted && !accepted->error && writes == 1,
        "valid boundary request id did not execute exactly one write");
}

void TestLuaListPagingProtocol() {
    using namespace War3Map::Mcp;
    int hostCalls = 0;
    const LuaFileToolHost host{[&](const QString&, const QJsonObject& args) {
        ++hostCalls;
        const int limit = args.value("limit").toInt(64);
        QJsonArray files;
        const auto path = QString(110, QLatin1Char('\\')) + QStringLiteral("file.lua");
        for (int i = 0; i < limit; ++i)
            files.append(QJsonObject{{"path", path}, {"state", "original"}, {"size", 1}});
        return QJsonObject{{"ok", true}, {"files", files}, {"offset", args.value("offset").toInt()},
            {"limit", limit}, {"total", 200}, {"next_offset", limit}, {"complete", false}};
    }};
    const QJsonObject base{{"project_identity", "owned-test"}, {"expected_revision", "1"}};
    for (const QJsonValue& value : {QJsonValue(-1), QJsonValue(1.5), QJsonValue(true), QJsonValue(QStringLiteral("2")),
            QJsonValue(QJsonValue::Null), QJsonValue(2147483648.0)}) {
        auto args = base; args.insert("offset", value);
        Expect(!InvokeLuaFileTool("lua_files_list", args, host).value("ok").toBool() && hostCalls == 0,
            "direct Lua list accepted an invalid offset before reaching host");
    }
    for (const QJsonValue& value : {QJsonValue(0), QJsonValue(257), QJsonValue(1.25), QJsonValue(false)}) {
        auto args = base; args.insert("limit", value);
        Expect(!InvokeLuaFileTool("lua_files_list", args, host).value("ok").toBool() && hostCalls == 0,
            "direct Lua list accepted an invalid limit before reaching host");
    }
    auto wrongTool = base; wrongTool.insert("offset", 0); wrongTool.insert("path", "a.lua");
    Expect(!InvokeLuaFileTool("lua_file_read", wrongTool, host).value("ok").toBool() && hostCalls == 0,
        "Lua read accidentally accepted list paging parameters");

    McpServer server;
    auto definitions = LuaFileToolDefinitions();
    auto list = std::find_if(definitions.begin(), definitions.end(), [](const auto& tool) { return tool.name == "lua_files_list"; });
    Expect(list != definitions.end(), "Lua list tool definition missing");
    list->handler = [&](const QJsonObject& args) -> QJsonValue { return InvokeLuaFileTool("lua_files_list", args, host); };
    server.RegisterTool(std::move(*list));
    const auto call = [&](const QJsonValue& id, QJsonObject args) {
        return server.HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
            {"method", "tools/call"}, {"params", QJsonObject{{"name", "lua_files_list"}, {"arguments", args}}}})
            .toJson(QJsonDocument::Compact));
    };
    auto invalid = base; invalid.insert("limit", 1.25);
    const auto invalidRpc = call(1, invalid);
    Expect(invalidRpc && invalidRpc->error && hostCalls == 0, "Lua list RPC schema accepted a fractional limit");
    auto large = base; large.insert("offset", 0); large.insert("limit", 200);
    const auto raw = QJsonDocument(host.invoke("lua_files_list", large)).toJson(QJsonDocument::Compact);
    const auto encoded = QJsonDocument(QJsonArray{QString::fromUtf8(raw)}).toJson(QJsonDocument::Compact);
    Expect(raw.size() < 65536 && encoded.size() - 2 > 65536,
        "synthetic Lua metadata did not isolate second-pass JSON escaping from raw body size");
    const auto tooLarge = call(QString(126, QLatin1Char('x')), large);
    Expect(tooLarge && !tooLarge->error && SerializeResponse(*tooLarge).size() <= 65536,
        "large Lua list response escaped the final 64 KiB envelope");
    const auto failureText = tooLarge->result.toObject().value("content").toArray()[0].toObject().value("text").toString();
    Expect(failureText.contains("LUA_FILES_RESPONSE_BUDGET") && !failureText.contains("MCP_TOOL_RESPONSE_BUDGET"),
        "large Lua page did not report a same-offset retryable budget failure");
    large.insert("limit", 40);
    const auto smaller = call(QString(126, QLatin1Char('x')), large);
    Expect(smaller && !smaller->error && SerializeResponse(*smaller).size() <= 65536,
        "smaller same-offset Lua page exceeded the actual JSON-RPC wire budget");
    const auto smallerText = smaller->result.toObject().value("content").toArray()[0].toObject().value("text").toString();
    Expect(QJsonDocument::fromJson(smallerText.toUtf8()).object().value("files").toArray().size() == 40,
        "Lua list budget failure could not be recovered by shrinking limit at the same offset");
    const int called = hostCalls;
    const auto oversizedId = call(QString(127, QLatin1Char('x')), large);
    Expect(oversizedId && oversizedId->error && hostCalls == called,
        "129-byte encoded Lua list request id reached the host");
    const LuaFileToolHost single{[](const QString&, const QJsonObject&) {
        return QJsonObject{{"ok", true}, {"files", QJsonArray{QJsonObject{{"path", QString(70000, QLatin1Char('\\'))}}}}};
    }};
    auto one = base; one.insert("limit", 1);
    const auto singleResult = InvokeLuaFileTool("lua_files_list", one, single);
    Expect(!singleResult.value("ok").toBool()
        && QJsonDocument(singleResult).toJson(QJsonDocument::Compact).contains("LUA_FILES_RESPONSE_BUDGET"),
        "one oversized synthetic metadata row was silently truncated or accepted");
}

void TestArchiveMemberIdentities() {
    QTemporaryDir temporary; Expect(temporary.isValid(), "archive identity temporary directory missing");
    const auto path = std::filesystem::path(temporary.path().toStdWString()) / L"查询 空格.w3x";
    const std::vector<War3Map::Mpq::MpqBuildFile> files{
        {"entry.dat", {std::byte{1}, std::byte{2}, std::byte{3}}, false, {}}, {"empty.dat", {}, false, {}}};
    Expect(War3Map::Mpq::WriteMpqArchive(path, files).Succeeded(), "archive identity fixture failed");
    const auto sourceSha = War3Map::Sha256File(path), payloadSha = War3Map::Sha256(files[0].data);
    Expect(sourceSha.Succeeded() && payloadSha.Succeeded(), "archive identity fixture hashes missing");
    War3Map::Mcp::McpServer server; War3Map::Mcp::RegisterAllTools(server);
    QJsonObject args{{"source_path", QString::fromStdWString(path.wstring())},
        {"expected_source_sha256", QString::fromStdString(*sourceSha.value)},
        {"member_names", QJsonArray{"entry.dat", "absent.dat", "empty.dat"}}};
    const auto request = [](QJsonObject arguments) { return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
        {"id", 32}, {"method", "tools/call"}, {"params", QJsonObject{{"name", "query_map_archive_member_identities"},
        {"arguments", arguments}}}}).toJson(QJsonDocument::Compact); };
    const auto call = [&] { const auto response = server.HandleMessage(request(args));
        Expect(response && !response->error, "archive identity protocol failed");
        const auto content = response->result.toObject().value("content").toArray();
        Expect(content.size() == 1, "archive identity tool response missing");
        return QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8()).object(); };
    const auto result = call(); const auto rows = result.value("members").toArray();
    Expect(result.value("ok").toBool() && result.value("schema_version").toInt() == 1 && rows.size() == 3
        && result.value("source_sha256").toString().toStdString() == *sourceSha.value,
        "independent archive query must work without directory workspace or UI roots");
    const auto present = rows[0].toObject(), missing = rows[1].toObject(), empty = rows[2].toObject();
    Expect(present.value("state") == "present" && present.value("bytes") == "3"
        && present.value("sha256").toString().toStdString() == *payloadSha.value && present.size() == 4,
        "archive identity exposed a body or reported wrong identity");
    Expect(missing.value("state") == "missing" && missing.value("bytes").isNull() && missing.value("sha256").isNull()
        && empty.value("state") == "present" && empty.value("bytes") == "0"
        && empty.value("sha256") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "archive query conflated empty and missing members");
    args["expected_source_sha256"] = QString(64, QLatin1Char('0'));
    const auto stale = call();
    Expect(!stale.value("ok").toBool() && !stale.contains("members"), "stale source exposed partial identities");
    args["expected_source_sha256"] = QString::fromStdString(*sourceSha.value); args["body"] = true;
    ExpectInvalidParams(server, request(args), "archive summary accepted a body export field");
    args.remove("body"); args["member_names"] = QJsonArray{"entry.dat", "../escape.dat"};
    const auto unsafe = call();
    Expect(!unsafe.value("ok").toBool() && !unsafe.contains("members"), "unsafe second name returned a partial batch");
    Expect(War3Map::Sha256File(path).value == sourceSha.value, "readonly archive query changed source bytes");
}

// 独立 MCP 不展开目录也能只读查询地图归档：绑定与 CLI map-data-query 相同，结果可交给 field_edits。
void TestArchiveMapDataQuery() {
    QTemporaryDir temporary; Expect(temporary.isValid(), "archive map data temporary directory missing");
    const auto path = std::filesystem::path(temporary.path().toStdWString()) / L"归档 查询.w3x";
    const std::string wts = "STRING 1\r\n{\r\nHello\r\n}\r\n";
    std::vector<std::byte> wtsBytes(wts.size());
    std::transform(wts.begin(), wts.end(), wtsBytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    const std::vector<War3Map::Mpq::MpqBuildFile> files{{"war3map.wts", wtsBytes, false, {}}};
    Expect(War3Map::Mpq::WriteMpqArchive(path, files).Succeeded(), "archive map data fixture failed");
    const auto sourceSha = War3Map::Sha256File(path);
    Expect(sourceSha.Succeeded(), "archive map data fixture hash missing");
    const QString sha = QString::fromStdString(*sourceSha.value), pathText = QString::fromStdWString(path.wstring());
    War3Map::Mcp::McpServer server; War3Map::Mcp::RegisterAllTools(server);
    const auto request = [](const char* name, QJsonObject arguments) { return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
        {"id", 33}, {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}})
        .toJson(QJsonDocument::Compact); };
    const auto call = [&](const char* name, QJsonObject arguments) {
        const auto response = server.HandleMessage(request(name, std::move(arguments)));
        Expect(response && !response->error, "archive map data protocol failed");
        const auto content = response->result.toObject().value("content").toArray();
        Expect(content.size() == 1, "archive map data response missing");
        return QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8()).object(); };
    const auto failedWith = [](const QJsonObject& result, const char* code) {
        return !result.value("ok").toBool() && QJsonDocument(result).toJson().contains(code); };

    const auto described = call("describe_map_data", {{"source_map_path", pathText}});
    const auto binding = described.value("binding").toObject();
    Expect(described.value("ok").toBool() && described.value("workspace_kind") == "map-archive-readonly"
        && described.value("revision").isNull() && binding.value("source_sha256").toString() == sha
        && binding.value("document_id").toString() == "archive-readonly-" + sha,
        "archive describe must work without a directory workspace and report the source SHA binding");
    QJsonObject strings;
    for (const auto& value : described.value("members").toArray())
        if (value.toObject().value("kind") == "strings") strings = value.toObject();
    Expect(strings.value("state") == "ready" && strings.value("member_sha256").isString(), "archive strings member not described");

    QJsonObject page{{"source_map_path", pathText}, {"expected_source_sha256", sha},
        {"expected_map_data_binding", binding}, {"kind", "strings"},
        {"expected_member_snapshot", strings.value("snapshot_token")}, {"start_index", "0"}, {"limit", 8}};
    const auto rows = call("query_map_data_page", page);
    Expect(rows.value("ok").toBool() && rows.value("rows").toArray().size() == 1
        && rows.value("rows").toArray()[0].toObject().value("row_key").isString(),
        "archive page must return rows with row keys usable by field_edits");

    auto noSha = page; noSha.remove("expected_source_sha256");
    Expect(failedWith(call("query_map_data_page", noSha), "MCP_MAP_DATA_SOURCE_SHA_INVALID"), "archive page accepted a missing source SHA");
    auto stale = page; stale["expected_source_sha256"] = QString(64, QLatin1Char('0'));
    Expect(failedWith(call("query_map_data_page", stale), "MAP_DATA_QUERY_STALE"), "archive page accepted a stale source SHA");
    auto both = page; both["expected_revision"] = QJsonObject{{"generation", "1"}, {"source_sha256", sha}, {"ui_catalog_sha256", sha}};
    Expect(failedWith(call("query_map_data_page", both), "MCP_MAP_DATA_SOURCE_SCOPE"), "archive and workspace sources were mixed");
    Expect(failedWith(call("describe_map_data", {}), "MCP_MAP_DATA_SOURCE_SCOPE"), "describe without any source was accepted");
    Expect(War3Map::Sha256File(path).value == sourceSha.value, "readonly archive map data query changed source bytes");
}

// 独立 MCP 的导入成员增删与 CLI 共用服务：写新候选、源图不变，删除结果说明未检查引用。
void TestArchiveMemberTools() {
    QTemporaryDir temporary; Expect(temporary.isValid(), "archive member temporary directory missing");
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    const auto path = root / L"导入 增删.w3x";
    const auto imp = War3Map::Resources::ImportTable::CreateEmpty().Serialize();
    Expect(imp.Succeeded() && imp.value.has_value(), "empty import table fixture failed");
    // 脚本按 JASS 转义写 .mdl；Hero2 只是前缀相同，不算 Hero 的出现。
    const std::string script = "call AddSpecialEffect(\"war3mapImported\\\\Hero.mdl\", 0, 0)\n"
        "call AddSpecialEffect(\"war3mapImported\\\\Hero2.mdx\", 0, 0)\n";
    std::vector<std::byte> scriptBytes;
    for (const char c : script) scriptBytes.push_back(static_cast<std::byte>(c));
    // 触发器自定义代码里写的路径同样要查到（只按文本查找，不需要完整 WCT 结构）。
    std::vector<std::byte> wctBytes;
    for (const char c : std::string("call Preload(\"war3mapImported\\\\old.mdx\")\n")) wctBytes.push_back(static_cast<std::byte>(c));
    const std::vector<War3Map::Mpq::MpqBuildFile> files{{"war3map.imp", *imp.value, false, {}},
        {"war3mapImported\\old.mdx", {std::byte{'o'}, std::byte{'l'}, std::byte{'d'}}, false, {}},
        {"war3map.j", scriptBytes, false, {}}, {"war3map.wct", wctBytes, false, {}}};
    Expect(War3Map::Mpq::WriteMpqArchive(path, files).Succeeded(), "archive member fixture failed");
    const auto sourceSha = War3Map::Sha256File(path);
    Expect(sourceSha.Succeeded(), "archive member fixture hash missing");
    const auto disk = root / L"Hero.mdx";
    { std::ofstream out(disk, std::ios::binary); out << "MDLX"; }
    War3Map::Mcp::McpServer server; War3Map::Mcp::RegisterAllTools(server);
    const auto call = [&](const char* name, QJsonObject arguments) {
        const auto response = server.HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 34},
            {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}}).toJson(QJsonDocument::Compact));
        Expect(response && !response->error, "archive member protocol failed");
        const auto content = response->result.toObject().value("content").toArray();
        Expect(content.size() == 1, "archive member response missing");
        return QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8()).object(); };
    const auto failedWith = [](const QJsonObject& result, const char* code) {
        return !result.value("ok").toBool() && QJsonDocument(result).toJson().contains(code); };
    const auto text = [](const std::filesystem::path& value) { return QString::fromStdWString(value.wstring()); };
    const auto file = [&](const char* member) {
        return QJsonArray{QJsonObject{{"disk_path", text(disk)}, {"archive_path", member}}}; };
    const QJsonObject source{{"source_map_path", text(path)}, {"expected_source_sha256", QString::fromStdString(*sourceSha.value)}};

    auto add = source; add["output_path"] = text(root / L"added.w3x"); add["files"] = file("war3mapImported/Hero.mdx");
    const auto added = call("archive_members_add", add);
    Expect(added.value("ok").toBool() && added.value("added").toArray().size() == 1
        && added.value("added").toArray()[0].toString() == "war3mapImported\\Hero.mdx"
        && added.value("candidate_sha256").toString().size() == 64, "archive_members_add must write a new candidate");
    auto existing = source; existing["output_path"] = text(root / L"existing.w3x"); existing["files"] = file("WAR3MAPIMPORTED\\OLD.MDX");
    Expect(failedWith(call("archive_members_add", existing), "MEMBER_EXISTS")
        && !std::filesystem::exists(root / L"existing.w3x"), "archive_members_add overwrote an existing member");
    auto stale = add; stale["output_path"] = text(root / L"stale.w3x"); stale["expected_source_sha256"] = QString(64, QLatin1Char('0'));
    Expect(failedWith(call("archive_members_add", stale), "ARCHIVE_SOURCE_STALE"), "archive_members_add accepted a stale source");
    auto relative = add; relative["output_path"] = "relative.w3x";
    Expect(failedWith(call("archive_members_add", relative), "MCP_MAP_PATH_INVALID"), "archive_members_add accepted a relative output");

    QJsonObject remove{{"source_map_path", text(root / L"added.w3x")}, {"expected_source_sha256", added.value("candidate_sha256")},
        {"output_path", text(root / L"removed.w3x")}, {"archive_paths", QJsonArray{"WAR3MAPIMPORTED\\HERO.MDX"}}};
    Expect(failedWith(call("archive_members_remove", remove), "MEMBER_REFERENCED")
        && !std::filesystem::exists(root / L"removed.w3x"), "archive_members_remove deleted a file the script still names");
    remove["allow_referenced"] = true;
    const auto removed = call("archive_members_remove", remove);
    const auto row = removed.value("removed").toArray().first().toObject();
    const auto mentions = row.value("mentioned_in").toArray();
    Expect(removed.value("ok").toBool() && row.value("import_records_removed").toInt() == 1
        && removed.value("references_checked") == false && mentions.size() == 1
        && mentions[0].toObject().value("member").toString() == "war3map.j"
        && mentions[0].toObject().value("kind").toString() == "script"
        && mentions[0].toObject().value("count").toInt() == 1, "archive_members_remove must drop the member and its import record");
    auto oldRemove = remove; oldRemove["output_path"] = text(root / L"old-removed.w3x");
    oldRemove["archive_paths"] = QJsonArray{"war3mapImported\\old.mdx"};
    const auto oldRow = call("archive_members_remove", oldRemove).value("removed").toArray().first().toObject();
    const auto oldMentions = oldRow.value("mentioned_in").toArray();
    Expect(oldMentions.size() == 1 && oldMentions[0].toObject().value("member").toString() == "war3map.wct"
        && oldMentions[0].toObject().value("kind").toString() == "trigger", "trigger custom code mentions must be reported");
    auto managed = remove; managed["output_path"] = text(root / L"managed.w3x"); managed["archive_paths"] = QJsonArray{"war3map.imp"};
    Expect(failedWith(call("archive_members_remove", managed), "ARCHIVE_MEMBER_MANAGED"), "archive_members_remove deleted a map data file");
    Expect(War3Map::Sha256File(path).value == sourceSha.value, "archive member tools changed the source map");

    // 展开：具名成员按归档路径写到新目录；data_only 不写导入资源；已有目录拒绝。
    auto expand = source; expand["output_path"] = text(root / L"展开");
    const auto expanded = call("archive_expand", expand);
    Expect(expanded.value("ok").toBool() && expanded.value("written_members").toInt() == 4
        && std::filesystem::is_regular_file(root / L"展开" / L"war3map.imp")
        && std::filesystem::file_size(root / L"展开" / L"war3mapImported" / L"old.mdx") == 3
        && expanded.value("trigger_pair_present") == false, "archive_expand must write every named member");
    Expect(failedWith(call("archive_expand", expand), "ARCHIVE_EXPAND_TARGET_EXISTS"), "archive_expand overwrote a directory");
    auto dataOnly = source; dataOnly["output_path"] = text(root / L"data-only"); dataOnly["data_only"] = true;
    const auto data = call("archive_expand", dataOnly);
    Expect(data.value("ok").toBool() && data.value("written_members").toInt() == 3
        && data.value("skipped_counts").toObject().value("import").toInt() == 1
        && !std::filesystem::exists(root / L"data-only" / L"war3mapImported"), "archive_expand data_only wrote imports");
    for (const auto& entry : std::filesystem::directory_iterator(root))
        Expect(entry.path().filename().wstring().find(L".expanding-") == std::wstring::npos, "archive_expand left a staging directory");
}

void TestMapDataPageBinding() {
    using War3Map::Mcp::McpServer;
    using War3Map::Mcp::RegisterAllTools;
    QTemporaryDir temporary;
    Expect(temporary.isValid(), "map data fixture directory unavailable");
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    const auto source = root / "source", ui = root / "ui";
    const auto write = [](const std::filesystem::path& path, std::string_view bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("map data fixture write failed");
    };
    const auto u32 = [](std::string& bytes, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>(value >> shift));
    };
    write(ui / "config", "base\n");
    write(ui / "base/action.txt", "[DoNothing]\ntitle=Do nothing\n[StoreInteger]\n[[.args]]\ntype=integer\n");
    write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
    write(ui / "base/event.txt", "[ReadEvent]\ntitle=Event\n");
    write(ui / "base/condition.txt", "[ReadCondition]\ntitle=Condition\n");
    write(ui / "base/call.txt", "[ReadCall]\ntitle=Call\nreturns=integer\n");
    std::string wtg = "WTG!"; u32(wtg, 7); u32(wtg, 1);
    u32(wtg, 0); wtg += "Category"; wtg.push_back(0); u32(wtg, 0);
    u32(wtg, 2); u32(wtg, 0); u32(wtg, 1);
    wtg += "Kinds"; wtg.push_back(0); wtg.push_back(0);
    for (const auto flag:{0u,1u,0u,0u,0u,0u,4u}) u32(wtg,flag);
    const std::array<const char*,4> names{"ReadEvent","ReadCondition","DoNothing","ReadCall"};
    for (std::uint32_t kind=0;kind<names.size();++kind) { u32(wtg,kind); wtg+=names[kind]; wtg.push_back(0); u32(wtg,1); u32(wtg,0); }
    std::string wct; u32(wct, 1); wct.push_back(0); u32(wct, 0); u32(wct, 1); u32(wct,0);
    write(source / "map/war3map.wtg", wtg); write(source / "map/war3map.wct", wct);
    std::string regions; u32(regions, 5); u32(regions, 2);
    for (int index = 0; index < 2; ++index) {
        for (float value : {0.f, 0.f, 512.f, 512.f}) u32(regions, std::bit_cast<std::uint32_t>(value));
        regions += index == 0 ? "TRIGSTR_12" : "TRIGSTR_13"; regions.push_back(0);
        u32(regions, static_cast<std::uint32_t>(42 + index)); u32(regions, 0); regions.push_back(0); u32(regions, 0);
    }
    write(source / "map/war3map.w3r", regions);
    write(source / "map/war3map.wts", "STRING 12\n{\nNorth\n}\nSTRING 13\n{\nSouth\n}\n");
    McpServer server; RegisterAllTools(server); server.SetWorkspace(source, ui);
    if (!server.Workspace()) {
        for (const auto& diagnostic : server.InitializationDiagnostics()) {
            std::cerr << diagnostic.subsystem << '/' << diagnostic.code << ": "
                      << diagnostic.message << '\n';
        }
    }
    Expect(server.Workspace() != nullptr, "map data fixture workspace failed to open");
    const auto revision = server.Workspace()->Revision();
    const QJsonObject expectedRevision{{"generation", QString::number(revision.generation)},
        {"source_sha256", QString::fromStdString(revision.sourceSha256)},
        {"ui_catalog_sha256", QString::fromStdString(revision.uiCatalogSha256)}};
    const auto call = [&](const char* name, QJsonObject arguments) {
        const auto request = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1},
            {"method", "tools/call"}, {"params", QJsonObject{{"name", name},
                {"arguments", arguments}}}}).toJson(QJsonDocument::Compact);
        const auto response = server.HandleMessage(request);
        Expect(response && !response->error, "map data protocol request failed");
        const auto content = response->result.toObject().value("content").toArray();
        Expect(content.size() == 1, "map data tool response missing content");
        return QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8()).object();
    };
    const auto code = [](const QJsonObject& result, const char* expected) {
        Expect(!result.value("ok").toBool(), "stale map data query succeeded");
        for (const auto& entry : result.value("diagnostics").toArray())
            if (entry.toObject().value("code") == QLatin1String(expected)) return;
        throw std::runtime_error("map data query missing failure code");
    };
    const auto searchCoverage = [](const QJsonObject& result, const char* category) {
        for (const auto& entry : result.value("coverage").toArray())
            if (entry.toObject().value("category") == QLatin1String(category)) return entry.toObject();
        return QJsonObject{};
    };
    const auto coldSearch = call("project_search", {});
    Expect(coldSearch.value("ok").toBool() && coldSearch.value("coverage").toArray().size() == 11
        && coldSearch.value("scope") == "standalone-opened-cache"
        && coldSearch.contains("source_checked") && !coldSearch.value("source_checked").toBool(),
        "cached search registration/scope is not explicit");
    Expect(searchCoverage(coldSearch, "region").value("state") == "unavailable"
        && searchCoverage(coldSearch, "map_object").value("state") == "unavailable"
        && searchCoverage(coldSearch, "code_symbol").value("state") == "unavailable",
        "cold standalone search invented unavailable domains or prepared them implicitly");
    const auto trigger=call("get_trigger",{{"trigger_id",QString::number(server.Workspace()->VisibleDocument().triggers.front().id)}});
    const auto roots=trigger.value("root_ecas").toArray(); const std::array<const char*,4> kinds{"event","condition","action","call"};
    Expect(trigger.value("ok").toBool() && roots.size()==4,"get_trigger kind fixture unavailable");
    for (int index=0;index<roots.size();++index)
        Expect(roots[index].toObject().value("kind")==QLatin1String(kinds[static_cast<std::size_t>(index)]),"WTG raw kind cast into wrong UI enum");
    const auto description = call("describe_map_data", {{"expected_revision", expectedRevision}});
    Expect(description.value("ok").toBool() && description.value("content_scope") == "committed-source" &&
        !description.value("includes_unsaved_ui_revision").toBool(), "map data description scope is false");
    const auto binding = description.value("binding").toObject();
    const auto stillCold = call("project_search", {});
    Expect(searchCoverage(stillCold, "region").value("state") == "unavailable",
        "separate committed-source reader was misrepresented as the current workspace-owned cache");
    const auto prepared = call("entities_query", {{"kind", "regions"}, {"page", QJsonObject{{"limit", 1}}}});
    Expect(prepared.value("ok").toBool(), "explicit typed MapData query did not prepare its existing captured snapshot");
    const auto cacheContext = server.Workspace()->MapDataContext(server.DocumentId(), server.DocumentGeneration());
    Expect(cacheContext.Succeeded() && cacheContext.value->preparedSnapshot
        && cacheContext.value->preparedSnapshot->Binding() == cacheContext.value->binding,
        "accepted current owner/binding cache missing");
    const auto savedSnapshot = cacheContext.value->preparedSnapshot;
    int capturesAccepted = 0;
    War3Map::Mcp::MapDataToolContext reusable;
    reusable.document = cacheContext.value->document; reusable.binding = cacheContext.value->binding;
    reusable.preparedSnapshot = savedSnapshot;
    reusable.acceptSnapshot = [&](auto) { ++capturesAccepted; return War3Map::Status::Ok(); };
    for (int index = 0; index < 3; ++index) {
        const auto reused = War3Map::Mcp::CaptureMapDataToolSnapshot(reusable, reusable.document);
        Expect(reused.Succeeded() && reused.value->OwnedDocument() == reusable.document,
            "explicit map facade failed to reuse current sealed snapshot");
    }
    Expect(capturesAccepted == 0, "prepared facade repeated Capture instead of using accepted snapshot");
    auto wrongBinding = cacheContext.value->binding; ++wrongBinding.revision;
    Expect(!server.Workspace()->PrepareProjectSearchMapCache(wrongBinding, savedSnapshot).Succeeded(),
        "forged snapshot binding accepted");
    const auto differentOwner = std::make_shared<const War3Map::MapDocument>(*reusable.document);
    const auto differentSnapshot = War3Map::Services::CaptureMapDataSnapshot(differentOwner, reusable.binding, false);
    Expect(differentSnapshot.Succeeded() && !server.Workspace()->PrepareProjectSearchMapCache(reusable.binding,
        std::make_shared<const War3Map::Services::MapDataQuerySnapshot>(*differentSnapshot.value)).Succeeded(),
        "equal-body different owner was accepted as the current map");
    const auto falseOwner = std::shared_ptr<const War3Map::MapDocument>(reusable.document.get(), [](const auto*) {});
    const auto aliasSnapshot = War3Map::Services::CaptureMapDataSnapshot(falseOwner, reusable.binding, false);
    Expect(aliasSnapshot.Succeeded() && !server.Workspace()->PrepareProjectSearchMapCache(reusable.binding,
        std::make_shared<const War3Map::Services::MapDataQuerySnapshot>(*aliasSnapshot.value)).Succeeded(),
        "no-op alias control block accepted as actual source ownership");
    const QJsonObject searchRequest{{"page", QJsonObject{{"limit", 1}}}};
    const auto warmSearch = call("project_search", searchRequest);
    Expect(warmSearch.value("ok").toBool() && searchCoverage(warmSearch, "region").value("state") == "ready"
        && searchCoverage(warmSearch, "imported_file").value("state") == "partial"
        && searchCoverage(warmSearch, "code_symbol").value("state") == "unavailable"
        && !warmSearch.value("complete").toBool() && warmSearch.value("next_cursor").isObject(),
        "warm standalone search coverage or bounded pagination is false");
    Expect(warmSearch.value("context_revision") != coldSearch.value("context_revision"), "cache availability did not stale the old revision");
    const auto sourceRevision = server.Workspace()->Revision();
    // 只移动本原创 QTemporaryDir 下的两个子目录。查询期间磁盘来源不可读，缓存页仍成立，
    // 证明该工具不隐式 Open / Rescan / reload catalog；不授予后续保存的环境门禁。
    std::filesystem::rename(source, root / "source-search-hidden"); std::filesystem::rename(ui, root / "ui-search-hidden");
    const auto detachedSearch = call("project_search", searchRequest);
    std::filesystem::rename(root / "source-search-hidden", source); std::filesystem::rename(root / "ui-search-hidden", ui);
    Expect(detachedSearch.value("ok").toBool() && detachedSearch.value("window_sha256") == warmSearch.value("window_sha256")
        && !detachedSearch.value("source_checked").toBool() && server.Workspace()->Revision() == sourceRevision,
        "cached search touched physical sources, changed the revision, or misreported source checking");
    const auto continuation = call("project_search", {{"page", QJsonObject{{"limit", 1}, {"cursor", warmSearch.value("next_cursor")}}}});
    Expect(continuation.value("ok").toBool() && continuation.value("window_sha256") == warmSearch.value("window_sha256"),
        "bounded standalone cursor did not continue the exact cached window");
    const auto afterSearch = server.Workspace()->MapDataContext(server.DocumentId(), server.DocumentGeneration());
    Expect(afterSearch.Succeeded() && afterSearch.value->preparedSnapshot == savedSnapshot,
        "search page replaced or prepared the cached typed snapshot");
    const auto member = description.value("members").toArray()[3].toObject();
    const auto regionHash = War3Map::Sha256(std::as_bytes(std::span(regions.data(), regions.size())));
    Expect(member.value("state") == "ready" && member.value("record_count") == "2" &&
        regionHash.Succeeded() && member.value("member_sha256").toString().toStdString() == *regionHash.value &&
        member.value("snapshot_token") == member.value("member_sha256"),
        "region member snapshot missing or not derived from source bytes");
    QJsonObject args{{"expected_revision", expectedRevision}, {"expected_map_data_binding", binding},
        {"kind", "regions"}, {"expected_member_snapshot", member.value("snapshot_token")},
        {"start_index", "0"}, {"limit", 1}};
    const auto first = call("query_map_data_page", args);
    Expect(first.value("ok").toBool() && first.value("rows").toArray().size() == 1,
        "map data first page failed");
    const auto firstRow = first.value("rows").toArray()[0].toObject();
    Expect(firstRow.value("name").toObject().value("raw_hex") == "545249475354525f3132" &&
        firstRow.value("name").toObject().value("display_utf8") == "North" &&
        firstRow.value("fields").toArray()[4].toObject().value("id") == "right" &&
        firstRow.value("fields").toArray()[4].toObject().value("raw") == "f32:44000000" &&
        firstRow.value("row_key").toString().startsWith("region:42:") &&
        first.value("next_cursor").toObject().value("next_index") == "1", "map data first page lost raw/display/bounds");
    args["start_index"] = first.value("next_cursor").toObject().value("next_index");
    const auto second = call("query_map_data_page", args);
    Expect(second.value("ok").toBool() && second.value("rows").toArray().size() == 1,
        "map data second page failed");
    Expect(second.value("rows").toArray()[0].toObject().value("source_index") == "1" &&
        second.value("next_cursor").isNull(), "map data continuation failed");
    auto missing = args; missing["kind"] = "settings";
    missing["expected_member_snapshot"] = description.value("members").toArray()[0].toObject().value("snapshot_token");
    missing["start_index"] = "0";
    const auto empty = call("query_map_data_page", missing);
    Expect(empty.value("ok").toBool() && empty.value("member").toObject().value("state") == "missing" &&
        empty.value("rows").toArray().isEmpty(), "missing member was not explicit empty state");
    args["expected_member_snapshot"] = QString(64, 'f');
    code(call("query_map_data_page", args), "MCP_MAP_DATA_MEMBER_STALE");
    args["expected_member_snapshot"] = member.value("snapshot_token");
    auto edit = server.Workspace()->BeginEdit(revision);
    Expect(edit.Succeeded(), "map data active transaction fixture failed");
    code(call("project_search", {{"page", QJsonObject{{"limit", 1}, {"cursor", warmSearch.value("next_cursor")}}}}), "PROJECT_SEARCH_CURSOR_STALE");
    const auto invalidatedSearch = call("project_search", {});
    Expect(searchCoverage(invalidatedSearch, "region").value("state") == "unavailable",
        "transaction binding change silently reused stale typed snapshot");
    code(call("query_map_data_page", args), "MCP_MAP_DATA_TRANSACTION_ACTIVE");
    const auto graph = call("query_trigger_graph", {{"table", "triggers"}, {"start", 0}, {"limit", 1}});
    const auto graphRows = graph.value("rows").toArray();
    Expect(graph.value("ok").toBool() && graphRows.size() == 1 && graph.value("eof").toBool() &&
        graph.value("counts").toObject().value("ecas") == "4", "GUI graph page/counts unavailable");
    const auto graphRow = graphRows[0].toObject();
    Expect(graphRow.value("name").toObject().value("raw_bytes_hex") == "4b696e6473" &&
        graphRow.value("ecas").toArray().size() == 4 && graphRow.value("source_span").isObject(),
        "GUI graph page lost original name bytes or stable nested references");
    code(call("query_trigger_graph", {{"table", "arguments"}, {"limit", 129}}), "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    // ECA 按函数名/函数库层筛选后分页，行内带所属触发器名，不需要逐页扫描全图。
    const auto ecaPage = call("query_trigger_graph", {{"table", "ecas"}, {"limit", 128}});
    const auto ecaRows = ecaPage.value("rows").toArray();
    Expect(ecaPage.value("ok").toBool() && !ecaRows.isEmpty(), "ECA page unavailable");
    const auto firstEca = ecaRows[0].toObject();
    const auto functionName = firstEca.value("name").toObject().value("text").toString();
    const auto layerName = firstEca.value("ui_layer").toString();
    qsizetype sameFunction = 0, sameLayer = 0;
    for (const auto& row : ecaRows) {
        sameFunction += row.toObject().value("name").toObject().value("text").toString() == functionName;
        sameLayer += row.toObject().value("ui_layer").toString() == layerName;
    }
    const auto byFunction = call("query_trigger_graph", {{"table", "ecas"}, {"functions", QJsonArray{functionName}}});
    Expect(byFunction.value("ok").toBool() && byFunction.value("query_mode") == "filter"
        && byFunction.value("total") == QString::number(sameFunction) && byFunction.value("rows").toArray().size() == sameFunction
        && byFunction.value("rows").toArray()[0].toObject().value("source_trigger_name").isObject(),
        "ECA function filter did not return exactly the matching calls");
    const auto byLayer = call("query_trigger_graph", {{"table", "ecas"}, {"ui_layer", layerName}, {"limit", 1}});
    Expect(byLayer.value("ok").toBool() && byLayer.value("total") == QString::number(sameLayer)
        && byLayer.value("rows").toArray().size() == 1 && byLayer.value("eof").toBool() == (sameLayer == 1),
        "ECA layer filter did not page over the filtered rows");
    const auto none = call("query_trigger_graph", {{"table", "ecas"}, {"functions", QJsonArray{"NoSuchFunctionForFilter"}}});
    Expect(none.value("ok").toBool() && none.value("total") == "0" && none.value("rows").toArray().isEmpty() && none.value("eof").toBool(),
        "ECA filter with no match must be an empty successful page");
    code(call("query_trigger_graph", {{"table", "triggers"}, {"functions", QJsonArray{functionName}}}), "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    const auto graphBinding = graph.value("binding").toObject();
    const auto target = graphRow.value("ref").toObject();
    QJsonObject rename{{"op", "rename"}, {"target", target}, {"expected_name", graphRow.value("name")}, {"name", "GUI renamed"}};
    const auto graphRequest = [&](QJsonArray operations) {
        return QJsonObject{{"transaction_id", QString::number(*edit.value)}, {"expected_revision", expectedRevision},
            {"expected", graphBinding}, {"operations", operations}};
    };
    auto badRename = rename;
    auto staleName = graphRow.value("name").toObject(); staleName["text"] = "stale"; badRename["expected_name"] = staleName;
    const QJsonObject firstSet{{"op", "set"}, {"target", target}, {"field", "description"}, {"expected", ""}, {"value", "must rollback"}};
    const auto budgetBatch = War3Map::Mcp::DecodeGraphBatch(QJsonObject{{"expected", graphBinding}, {"operations", QJsonArray{firstSet}}});
    Expect(budgetBatch.Succeeded(), "prepublish fixture did not decode its real Graph DTO");
    const auto priorToken = server.Workspace()->ObjectRevision();
    int candidateChecks = 0;
    const auto refusedPublish = server.Workspace()->ApplyGraphEdit(*edit.value, revision, *budgetBatch.value,
        [&](const War3Map::Trigger::TriggerDocument& before, const War3Map::Trigger::TriggerDocument& after,
            const War3Map::Trigger::TriggerGraphEditReceipt& receipt) {
            ++candidateChecks;
            Expect(before.triggers[0].description.text.empty() && after.triggers[0].description.text == "must rollback"
                && receipt.before.currentWtgSha256 != receipt.after.currentWtgSha256,
                "prepublish check did not receive the actual isolated candidate");
            return War3Map::Status::Failure("MCP.Test", "TEST_RECEIPT_BUDGET", "候选回执超预算，不发布。");
        });
    Expect(!refusedPublish.Succeeded() && candidateChecks == 1 && server.Workspace()->ObjectRevision() == priorToken
        && call("query_trigger_graph", {{"table", "triggers"}}).value("binding") == graph.value("binding"),
        "failed prepublish validator changed active generation or visible Graph");
    code(call("edit_trigger_graph", graphRequest({firstSet, badRename})), "TRIGGER_GRAPH_NAME_STALE");
    Expect(call("query_trigger_graph", {{"table", "triggers"}}).value("binding") == graph.value("binding"),
        "failed second GUI operation left the first operation applied");
    auto invalidRename = rename; staleName = graphRow.value("name").toObject(); staleName["extra"] = true; invalidRename["expected_name"] = staleName;
    code(call("edit_trigger_graph", graphRequest({invalidRename})), "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    auto wrongType = firstSet; wrongType["field"] = "enabled"; wrongType["expected"] = "true"; wrongType["value"] = false;
    code(call("edit_trigger_graph", graphRequest({wrongType})), "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    const auto renamed = call("edit_trigger_graph", graphRequest({rename}));
    const auto searchOverlay = call("project_search", {{"query", "GUI renamed"}});
    Expect(searchOverlay.value("ok").toBool() && !searchOverlay.value("rows").toArray().isEmpty()
        && searchOverlay.value("context_revision") != warmSearch.value("context_revision"),
        "standalone search missed the actual active trigger overlay");
    const auto afterBinding = renamed.value("binding").toObject();
    Expect(renamed.value("ok").toBool() && renamed.value("transaction_active").toBool() &&
        !renamed.value("saved").toBool() && afterBinding.value("current_wtg_sha256") != graphBinding.value("current_wtg_sha256") &&
        afterBinding.value("current_wct_sha256") == graphBinding.value("current_wct_sha256") &&
        afterBinding.value("source_wct_sha256") == graphBinding.value("source_wct_sha256"),
        "pure GUI rename changed WCT bytes or pretended to save/rebind the source");
    Expect(server.Workspace()->PreviewEdit(*edit.value, revision).Succeeded() &&
        server.Workspace()->ValidateEdit(*edit.value, revision).satisfied, "renamed GUI generation did not validate");
    War3Map::Services::MapEditSaveRequest archiveRequest;
    archiveRequest.sourceMap = root / "missing-map.w3x"; archiveRequest.targetCandidate = root / "candidate.w3x";
    archiveRequest.toolRoot = ui; archiveRequest.gameRoot = root; archiveRequest.expectedSourceSha256 = std::string(64, 'a');
    const auto exportResult = server.Workspace()->ExportArchiveCandidate(*edit.value, revision, archiveRequest);
    Expect(!exportResult.Succeeded() && std::any_of(exportResult.diagnostics.begin(), exportResult.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "SOURCE_LEASE_FAILED"; }),
        "WCT-preserving rename was rejected before the common archive source guard");
    Expect(server.Workspace()->Rollback(*edit.value, revision).Succeeded(), "map data transaction rollback failed");
    const auto argumentEdit = server.Workspace()->BeginEdit(revision);
    Expect(argumentEdit.Succeeded(), "argument replacement transaction failed to start");
    auto activeGraph = call("query_trigger_graph", {{"table", "triggers"}});
    const auto argumentRequest = [&](const QJsonObject& binding, const QJsonArray& operations) {
        return QJsonObject{{"transaction_id", QString::number(*argumentEdit.value)}, {"expected_revision", expectedRevision},
            {"expected", binding}, {"operations", operations}};
    };
    const QJsonObject getter{{"function", "ReadCall"}, {"kind", "call"}, {"enabled", true}, {"arguments", QJsonArray{}}, {"children", QJsonArray{}}};
    const QJsonObject store{{"function", "StoreInteger"}, {"kind", "action"}, {"enabled", true},
        {"arguments", QJsonArray{QJsonObject{{"client_key", "retained_argument"}, {"call", getter}}}}, {"children", QJsonArray{}}};
    auto emptyArgument = store; emptyArgument["arguments"] = QJsonArray{QJsonObject{{"client_key", "empty_argument"}}};
    code(call("edit_trigger_graph", argumentRequest(activeGraph.value("binding").toObject(),
        QJsonArray{QJsonObject{{"op", "insert_subtree"}, {"root", emptyArgument},
            {"parent", QJsonObject{{"kind", "trigger_ecas"}, {"owner", target}, {"ordered_children", graphRow.value("ecas")}}}}})),
        "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    const QJsonObject inserted = call("edit_trigger_graph", argumentRequest(activeGraph.value("binding").toObject(),
        QJsonArray{QJsonObject{{"op", "insert_subtree"}, {"root", store},
            {"parent", QJsonObject{{"kind", "trigger_ecas"}, {"owner", target}, {"ordered_children", graphRow.value("ecas")}}}}}));
    Expect(inserted.value("ok").toBool(), "argument replacement nested fixture failed to insert");
    const auto argumentRef = inserted.value("created").toObject().value("retained_argument").toObject();
    const auto argumentId = argumentRef.value("id");
    const QJsonObject pointRequest{{"table", "arguments"}, {"argument_ids", QJsonArray{argumentId}}, {"include_replacement_guard", true}};
    const auto point = call("query_trigger_graph", pointRequest);
    const auto pointRows = point.value("rows").toArray();
    Expect(point.value("ok").toBool() && pointRows.size() == 1 && point.value("query_mode") == "argument_ids"
        && pointRows[0].toObject().value("replacement_guard").isObject(), "bounded guard query missing real Core guard");
    const auto originalArgument = pointRows[0].toObject();
    auto guard = originalArgument.value("replacement_guard").toObject();
    Expect(guard.value("expected_type") == "integer" && guard.value("subtree_sha256").toString().size() == 64,
        "argument guard lost actual type or digest");
    QJsonObject replacement{{"op", "replace_argument"}, {"target", argumentRef}, {"expected", guard},
        {"value", QJsonObject{{"scalar", QJsonObject{{"kind", 3}, {"value", "42"}}}}}};
    auto badReplacement = replacement;
    auto badGuard = guard; badGuard["unexpected"] = true; badReplacement["expected"] = badGuard;
    code(call("edit_trigger_graph", argumentRequest(point.value("binding").toObject(), {badReplacement})), "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    code(call("query_trigger_graph", {{"table", "arguments"}, {"argument_ids", QJsonArray{argumentId, argumentId}}}), "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    code(call("query_trigger_graph", {{"table", "arguments"}, {"argument_ids", QJsonArray{"1"}}}), "MCP_TRIGGER_GRAPH_ARGUMENT_MISSING");
    const auto replaced = call("edit_trigger_graph", argumentRequest(point.value("binding").toObject(), {replacement}));
    Expect(replaced.value("ok").toBool() && replaced.value("argument_replacements").toArray().size() == 1
        && !replaced.value("saved").toBool(), "argument replacement receipt missing or claimed save");
    const auto afterPoint = call("query_trigger_graph", pointRequest);
    const auto afterArgument = afterPoint.value("rows").toArray()[0].toObject();
    Expect(afterArgument.value("ref") == argumentRef && afterArgument.value("call").isNull()
        && afterArgument.value("value").toObject().value("text") == "42"
        && afterPoint.value("binding").toObject().value("current_wct_sha256") == point.value("binding").toObject().value("current_wct_sha256"),
        "replace_argument changed stable root, WCT, or left the old call");
    const auto staleResult = call("edit_trigger_graph", argumentRequest(afterPoint.value("binding").toObject(), {replacement}));
    Expect(!staleResult.value("ok").toBool()
        && call("query_trigger_graph", pointRequest).value("binding") == afterPoint.value("binding"), "stale argument guard mutated document");
    // 紧凑 T 写法：夹具触发器含 WE 不会产生的“调用”根节点，导出须如实拒绝；整体替换后导出与写入一致；错误名字不写入。
    const auto exportText = [&] {
        return call("query_trigger_graph", {{"table", "trigger_text"}, {"trigger_id", target.value("id")}});
    };
    code(exportText(), "TRIGGER_TEXT_RENDER");
    const QString canonical = "events:\n    ReadEvent()\nactions:\n    StoreInteger(42)\n    ~StoreInteger(ReadCall())\n";
    const QString written = "events: ReadEvent()\nactions: StoreInteger(42); ~StoreInteger(ReadCall())";
    const auto textEdit = call("edit_trigger_graph", argumentRequest(call("query_trigger_graph", {{"table", "triggers"}}).value("binding").toObject(),
        {QJsonObject{{"op", "replace_trigger_text"}, {"trigger", target}, {"text", written}}}));
    Expect(textEdit.value("ok").toBool(), "replace_trigger_text failed through MCP JSON");
    const auto exported = exportText();
    Expect(exported.value("ok").toBool() && exported.value("text").toString() == canonical, "trigger_text did not export the replaced body");
    code(call("edit_trigger_graph", argumentRequest(exported.value("binding").toObject(),
        {QJsonObject{{"op", "replace_trigger_text"}, {"trigger", target}, {"text", "actions:\n    Missing()\n"}}})),
        "TRIGGER_TEXT_UNKNOWN_FUNCTION");
    Expect(exportText().value("text").toString() == canonical, "rejected text changed the trigger");
    // 局部替换：按导出行号与原文改一行。
    const auto patched = call("edit_trigger_graph", argumentRequest(exportText().value("binding").toObject(),
        {QJsonObject{{"op", "patch_trigger_text"}, {"trigger", target}, {"from_line", 4}, {"to_line", 4},
            {"expected", "    StoreInteger(42)"}, {"text", "    StoreInteger(43)"}}}));
    Expect(patched.value("ok").toBool() && exportText().value("text").toString() == QString(canonical).replace("42", "43"),
        "patch_trigger_text did not replace the guarded line");
    // 功能写法：一次写入类别与触发器，再按类别导出。
    const auto feature = call("edit_trigger_graph", argumentRequest(exportText().value("binding").toObject(),
        {QJsonObject{{"op", "apply_feature_text"}, {"category", "MCP Feature"}, {"text", "触发器 Feature Trigger:\n    StoreInteger(7)\n"}}}));
    Expect(feature.value("ok").toBool(), "apply_feature_text failed through MCP JSON");
    const auto featureCategory = feature.value("created").toObject().value("feature/category").toObject();
    const auto featureText = call("query_trigger_graph", {{"table", "feature_text"}, {"category_id", featureCategory.value("id")}});
    Expect(featureText.value("ok").toBool()
        && featureText.value("text").toString() == "触发器 Feature Trigger:\n    actions:\n        StoreInteger(7)\n",
        "feature_text did not export the written feature");
    // after 锚点：新触发器插在指定触发器之后；轻量 move 只凭 before 锚点。
    const auto firstTrigger = feature.value("created").toObject().value("feature/trigger/Feature Trigger").toObject();
    const auto anchored = call("edit_trigger_graph", argumentRequest(feature.value("binding").toObject(),
        {QJsonObject{{"op", "apply_feature_text"}, {"category", "MCP Feature"}, {"client_key", "second"}, {"after", firstTrigger},
            {"text", "触发器 Second:\n    StoreInteger(8)\n"}}}));
    Expect(anchored.value("ok").toBool(), "apply_feature_text with after failed through MCP JSON");
    const auto secondTrigger = anchored.value("created").toObject().value("second/trigger/Second").toObject();
    const auto placed = call("edit_trigger_graph", argumentRequest(anchored.value("binding").toObject(),
        {QJsonObject{{"op", "move"}, {"target", secondTrigger}, {"before", firstTrigger}}}));
    Expect(placed.value("ok").toBool(), "light move failed through MCP JSON");
    const auto movedText = call("query_trigger_graph", {{"table", "feature_text"}, {"category_id", featureCategory.value("id")}})
        .value("text").toString();
    Expect(movedText.indexOf("触发器 Second:") >= 0 && movedText.indexOf("触发器 Second:") < movedText.indexOf("触发器 Feature Trigger:"),
        "light move did not put Second before the anchor");
    code(call("edit_trigger_graph", argumentRequest(placed.value("binding").toObject(),
        {QJsonObject{{"op", "move"}, {"target", secondTrigger}, {"before", firstTrigger}, {"after", firstTrigger}}})), "MCP_TRIGGER_GRAPH_REQUEST_INVALID");
    // 真实用法原句：刚写入的 StoreInteger(7) 能按函数名找回。
    const auto examples = call("query_trigger_graph", {{"table", "function_examples"}, {"functions", QJsonArray{"StoreInteger"}}});
    const auto storeExamples = examples.value("examples").toObject().value("StoreInteger").toArray();
    Expect(examples.value("ok").toBool() && std::any_of(storeExamples.begin(), storeExamples.end(),
        [](const QJsonValue& row) { return row.toObject().value("text").toString() == "StoreInteger(7)"; }),
        "function_examples did not return the real usage line");
    const auto presets = call("query_trigger_graph", {{"table", "presets"}, {"type", "no_such_type"}});
    Expect(presets.value("ok").toBool() && presets.value("total").toString() == "0", "presets table did not filter by type");
    Expect(server.Workspace()->Rollback(*argumentEdit.value, revision).Succeeded(), "argument replacement rollback failed");
    server.SetWorkspace(source, ui);
    code(call("query_map_data_page", args), "MAP_DATA_QUERY_STALE");

    // 使用真实成功创建的变量，避免 get 以不存在 ID 拒绝而掩盖环境门禁缺失。
    Expect(server.Workspace() != nullptr, "variable environment fixture failed to reopen");
    const auto variableRevision = server.Workspace()->Revision();
    const auto variableEdit = server.Workspace()->BeginEdit(variableRevision);
    Expect(variableEdit.Succeeded(), "variable environment transaction failed to start");
    const auto emptyVariables = call("variables_list", {});
    Expect(emptyVariables.value("ok").toBool() && emptyVariables.value("rows").toArray().isEmpty(),
        "variable environment fixture did not query the actual empty source");
    const auto createdVariable = call("variables_create", {
        {"project_identity", emptyVariables.value("project_identity")},
        {"expected_revision", emptyVariables.value("revision")},
        {"name", "SourceGateCounter"}, {"type_name", "integer"},
        {"initialized", true}, {"initial_value", "42"}});
    const auto variableId = createdVariable.value("id");
    Expect(createdVariable.value("ok").toBool() && variableId.isString() && !variableId.toString().isEmpty(),
        "variable environment fixture did not create a real guarded variable");
    const QJsonObject variableGetArguments{{"id", variableId}};
    const auto readyVariable = call("variables_get", variableGetArguments);
    const auto readyVariables = call("variables_list", {});
    Expect(readyVariable.value("ok").toBool() && readyVariables.value("ok").toBool()
        && readyVariable.value("variable").toObject().value("value").toObject().value("text") == "42"
        && readyVariables.value("rows").toArray() == QJsonArray{readyVariable.value("variable")},
        "variable environment list/get must succeed with the actual created StableId before changing sources");
    const auto variableToken = server.Workspace()->ObjectRevision();
    const auto expectEnvironmentRefusal = [&](const char* expectedCode) {
        const auto environment = server.Workspace()->EnvironmentStatus();
        Expect(!environment.Succeeded(), "changed variable environment incorrectly remained current");
        const auto expectedDiagnostics = War3Map::Mcp::DiagnosticsJson(environment.diagnostics).value("diagnostics");
        const auto listed = call("variables_list", {});
        const auto fetched = call("variables_get", variableGetArguments);
        for (const auto& refused : {listed, fetched}) {
            code(refused, expectedCode);
            Expect(refused.value("diagnostics") == expectedDiagnostics
                && !refused.contains("rows") && !refused.contains("variable") && !refused.contains("binding"),
                "variable query did not propagate the exact EnvironmentStatus failure or exposed stale partial records");
        }
        Expect(server.Workspace()->ObjectRevision() == variableToken,
            "failed variable environment queries changed active generation or opaque revision");
    };
    const auto expectVariableRecovery = [&] {
        Expect(server.Workspace()->EnvironmentStatus().Succeeded()
            && call("variables_get", variableGetArguments) == readyVariable
            && call("variables_list", {}) == readyVariables
            && server.Workspace()->ObjectRevision() == variableToken,
            "restoring exact environment bytes did not recover the original variable and query revision");
    };
    auto changedRegions = regions;
    // 保持 W3R 长度与布局，只改变首个有限 float 的一个有效位，测试真实来源身份变化。
    changedRegions[8] = static_cast<char>(static_cast<unsigned char>(changedRegions[8]) ^ 1U);
    write(source / "map/war3map.w3r", changedRegions);
    expectEnvironmentRefusal("SOURCE_CHANGED");
    write(source / "map/war3map.w3r", regions);
    expectVariableRecovery();
    const std::string originalActions = "[DoNothing]\ntitle=Do nothing\n[StoreInteger]\n[[.args]]\ntype=integer\n";
    write(ui / "base/action.txt", "[DoNothing]\ntitle=Changed catalog title\n[StoreInteger]\n[[.args]]\ntype=integer\n");
    expectEnvironmentRefusal("MCP_UI_CATALOG_CHANGED");
    write(ui / "base/action.txt", originalActions);
    expectVariableRecovery();
    Expect(server.Workspace()->Rollback(*variableEdit.value, variableRevision).Succeeded(),
        "variable environment transaction rollback failed");
    const auto restoredVariables = call("variables_list", {});
    Expect(restoredVariables.value("ok").toBool() && restoredVariables.value("rows").toArray().isEmpty(),
        "variable environment rollback did not restore the original empty source list");
}

void TestProjectSearchTypePreparation() {
    using namespace War3Map;
    using namespace War3Map::Mcp;
    using Services::ProjectSearchCategory;
    using Services::ProjectSearchState;
    QTemporaryDir temporary;
    Expect(temporary.isValid(), "type preparation fixture directory unavailable");
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    const auto source = root / "source", ui = root / "ui";
    const auto write = [](const std::filesystem::path& path, std::string_view bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        Expect(out.good(), "type preparation fixture write failed");
    };
    const auto u32 = [](std::string& bytes, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>(value >> shift));
    };
    const auto text = [](std::string& bytes, std::string_view value) { bytes += value; bytes.push_back(0); };
    write(ui / "config", "base\n");
    write(ui / "base/define.txt", "[TriggerTypes]\nstring=0,1,1,String,string\ntextalias=0,1,1,Text alias,string\n"
        "[TriggerParams]\nAliasValue=0,textalias,Alias,gg_unit_n000_0001\n");
    for (const char* name : {"action", "event", "condition", "call"}) write(ui / (std::string("base/") + name + ".txt"), "");
    std::string wtg = "WTG!"; u32(wtg, 7); u32(wtg, 1); u32(wtg, 0); text(wtg, "Category"); u32(wtg, 0);
    u32(wtg, 2); u32(wtg, 3);
    for (const char* name : {"CounterA", "CounterB", "CounterC"}) {
        text(wtg, name); text(wtg, "textalias");
        for (const auto value : {1U, 0U, 1U, 1U}) u32(wtg, value);
        text(wtg, "gg_unit_n000_0001"); // 热缓存按 string 别名解释，不误当预放置单位引用。
    }
    u32(wtg, 1); text(wtg, "CounterTrigger"); text(wtg, "");
    for (const auto value : {0U, 1U, 0U, 0U, 0U, 0U, 0U}) u32(wtg, value);
    std::string wct; u32(wct, 1); text(wct, ""); u32(wct, 0); u32(wct, 1); u32(wct, 0);
    write(source / "war3map.wtg", wtg); write(source / "war3map.wct", wct);
    McpServer server; RegisterAllTools(server); server.SetWorkspace(source, ui);
    auto* workspace = server.Workspace();
    Expect(workspace != nullptr && workspace->PreparedProjectSearchTypes() != nullptr,
        "accepted Open did not explicitly prepare types");
    const auto* acceptedTypes = workspace->PreparedProjectSearchTypes();
    Expect(&acceptedTypes->Catalog() == &workspace->Catalog() && acceptedTypes->Find("textalias")
        && acceptedTypes->LiteralKind("textalias") == Trigger::TriggerTypeSystem::Literal::String,
        "prepared types are not bound to the actual accepted catalog or alias");
    const auto revision = workspace->Revision();
    const auto contextRevision = workspace->CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration());
    Services::ProjectSearchOptions options{128, 4, 8192, 32, 4096};
    const auto search = [&](McpWorkspace& current, std::string_view expected, const Services::ProjectSearchCursor* cursor = nullptr) {
        return current.SearchCachedProject(server.DocumentId(), server.DocumentGeneration(), expected, "counter", options, 1, cursor);
    };
    auto first = search(*workspace, contextRevision);
    Expect(first.Succeeded() && first.value && first.value->next && first.value->totalWindowRows == 4
        && first.value->coverage[static_cast<std::size_t>(ProjectSearchCategory::Variable)].state == ProjectSearchState::Ready
        && first.value->nameNodesCharged > 0 && first.value->nameNodesCharged <= options.maxTriggerNodes
        && first.value->nameTextBytesCharged <= options.maxTriggerTextBytes,
        "real bounded search did not use the prepared alias or preserve service work budgets");
    const auto cursor = *first.value->next;
    // 已接受所有来源后使物理目录不可用；重复页和续页仍只借用同一类型缓存，不读盘补建。
    std::filesystem::rename(source, root / "source-hidden"); std::filesystem::rename(ui, root / "ui-hidden");
    for (int index = 0; index < 4; ++index) {
        const auto page = search(*workspace, contextRevision, &cursor);
        Expect(page.Succeeded() && page.value && page.value->windowSha256 == first.value->windowSha256
            && workspace->PreparedProjectSearchTypes() == acceptedTypes
            && workspace->CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration()) == contextRevision
            && workspace->Revision() == revision,
            "pagination rebuilt types, touched unavailable physical sources, or changed the accepted cache/revision");
    }
    std::size_t definitions = 0; std::uintmax_t sourceBytes = 0;
    for (const auto& item : workspace->Catalog().Manifest()) { definitions += item.definitionCount; sourceBytes += item.size; }
    Expect(definitions > 1 && sourceBytes > 1, "fixture must charge real definition and source-byte metadata");
    const auto hasCode = [](const auto& result, std::string_view expected) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == expected; });
    };
    const auto rejected = workspace->PrepareProjectSearchTypesCache(definitions - 1, sourceBytes);
    Expect(!rejected.Succeeded() && hasCode(rejected, "PROJECT_SEARCH_TYPES_BUDGET")
        && !workspace->PreparedProjectSearchTypes(), "definition budget refusal kept or built an uncharged type cache");
    Expect(!search(*workspace, contextRevision, &cursor).Succeeded(), "old cursor survived explicit cache invalidation");
    const auto coldRevision = workspace->CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration());
    const auto cold = search(*workspace, coldRevision);
    Expect(cold.Succeeded() && cold.value && hasCode(cold, "PROJECT_SEARCH_TYPES_UNPREPARED")
        && cold.value->coverage[static_cast<std::size_t>(ProjectSearchCategory::Variable)].state == ProjectSearchState::Partial
        && !workspace->PreparedProjectSearchTypes(), "cold query silently prepared types or claimed complete type coverage");
    Expect(!workspace->PrepareProjectSearchTypesCache(definitions, sourceBytes - 1).Succeeded()
        && !workspace->PreparedProjectSearchTypes(), "source-byte budget boundary was ignored");
    Expect(workspace->PrepareProjectSearchTypesCache(definitions, sourceBytes).Succeeded()
        && workspace->PreparedProjectSearchTypes(), "exact preparation budgets failed on already accepted metadata");
    Expect(!workspace->PrepareProjectSearchTypesCache(32769, sourceBytes).Succeeded()
        && !workspace->PreparedProjectSearchTypes(), "caller widened the hard preparation cap");
    Expect(workspace->PrepareProjectSearchTypesCache().Succeeded(), "explicit metadata-only preparation could not recover hot types");
    std::filesystem::rename(root / "source-hidden", source); std::filesystem::rename(root / "ui-hidden", ui);
    const auto reopened = McpWorkspace::Open(source, ui);
    Expect(reopened.Succeeded() && reopened.value && (*reopened.value)->PreparedProjectSearchTypes(), "move fixture failed to open");
    auto moved = McpWorkspace(std::move(**reopened.value));
    Expect(!moved.PreparedProjectSearchTypes(), "moved workspace borrowed types bound to the old catalog address");
    const auto movedRevision = moved.CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration());
    const auto movedCold = search(moved, movedRevision);
    Expect(movedCold.Succeeded() && movedCold.value && hasCode(movedCold, "PROJECT_SEARCH_TYPES_UNPREPARED")
        && !moved.PreparedProjectSearchTypes(), "moved cold query dereferenced or rebuilt the old-owner cache");
    Expect(moved.PrepareProjectSearchTypesCache().Succeeded() && moved.PreparedProjectSearchTypes()
        && &moved.PreparedProjectSearchTypes()->Catalog() == &moved.Catalog(), "explicit move recovery did not bind the new catalog owner");
    const auto movedHot = search(moved, moved.CurrentProjectSearchRevision(server.DocumentId(), server.DocumentGeneration()));
    Expect(movedHot.Succeeded() && movedHot.value
        && movedHot.value->coverage[static_cast<std::size_t>(ProjectSearchCategory::Variable)].state == ProjectSearchState::Ready,
        "new-owner prepared types did not restore the actual alias behavior");
    std::cout << "PROJECT_SEARCH_TYPES_CACHE definitions=" << definitions << " source_bytes=" << sourceBytes
              << " repeated_pages=4 cold_partial=true exact_budget=true moved_owner_gate=true\n";
}

void TestProjectSearchImportInputBudgets() {
    using namespace War3Map;
    using Mcp::ProjectSearchDetail::PrepareImportedPaths;
    const auto code = [](const auto& result, std::string_view expected) {
        return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.code == expected && diagnostic.severity == DiagnosticSeverity::Warning; });
    };
    const auto item = [](std::filesystem::path path) { WorkspaceFile result; result.relativePath = std::move(path); return result; };
    std::vector<WorkspaceFile> inventory{item("owned.blp"), item("other.mdx")};
    const auto ordinary = PrepareImportedPaths(inventory, 4096);
    Expect(ordinary.Succeeded() && ordinary.value && *ordinary.value == std::vector<std::string>{"owned.blp", "other.mdx"}
        && ordinary.diagnostics.empty(), "small accepted metadata changed path identity or emitted a false refusal");
    // 原创内存metadata，无文件/目录创建。巨本机路径含坏编码，用不同诊断证明native门先于转换。
    std::filesystem::path::string_type huge(8192, std::filesystem::path::value_type{'x'});
#ifdef _WIN32
    huge[0] = static_cast<wchar_t>(0xd800); // 单独高代理；若先转换会失败，而不是native长度拒绝。
#else
    huge[0] = static_cast<char>(0xff);
#endif
    inventory = {item("owned.blp"), item(std::filesystem::path(huge)), item("suffix.mdx")};
    const auto native = PrepareImportedPaths(inventory, 4096);
    Expect(native.Succeeded() && native.value && *native.value == std::vector<std::string>{"owned.blp"}
        && code(native, "PROJECT_SEARCH_IMPORT_NATIVE_PATH_LIMIT")
        && !code(native, "PROJECT_SEARCH_IMPORT_PATH_INVALID") && inventory[1].relativePath.native() == huge,
        "huge native path was converted/copied, rewrote metadata, or leaked an unchecked suffix identity");
    inventory.clear();
    constexpr std::size_t singleBytes = 2048, totalBytes = 1024 * 1024;
    for (std::size_t index = 0; index < totalBytes / singleBytes; ++index) {
        const auto prefix = std::to_string(index) + '/';
        inventory.push_back(item(prefix + std::string(singleBytes - prefix.size(), 'a')));
    }
    const auto exact = PrepareImportedPaths(inventory, 4096);
    Expect(exact.Succeeded() && exact.value, "exact metadata budget did not produce an owned result");
    std::size_t charged = 0; for (const auto& path : *exact.value) charged += path.size();
    Expect(exact.Succeeded() && exact.value && exact.value->size() == 512 && charged == totalBytes
        && exact.diagnostics.empty(), "exact one-MiB/2048-byte metadata boundaries failed");
    inventory.push_back(item("z"));
    const auto over = PrepareImportedPaths(inventory, 4096);
    Expect(over.Succeeded() && over.value && *over.value == *exact.value && code(over, "PROJECT_SEARCH_IMPORT_PATH_TOTAL_LIMIT"),
        "one-byte overflow converted/appended an unchecked identity or refused the known bounded prefix");
    inventory.clear();
    for (std::size_t index = 0; index < 4098; ++index) inventory.push_back(item(std::to_string(index) + ".blp"));
    const auto rows = PrepareImportedPaths(inventory, 32768);
    Expect(rows.Succeeded() && rows.value && rows.value->size() == 4097 && rows.value->back() == "4096.blp"
        && code(rows, "PROJECT_SEARCH_IMPORT_ROW_LIMIT"), "4097-row hard window was widened or scanned its suffix");
    const auto witness = PrepareImportedPaths(inventory, 1);
    Expect(witness.Succeeded() && witness.value && witness.value->size() == 2
        && code(witness, "PROJECT_SEARCH_IMPORT_ROW_LIMIT"), "one-row service budget lost its one bounded truncation witness");
    inventory = {item("duplicate.blp"), item("duplicate.blp"), item("suffix.mdx")};
    const auto duplicate = PrepareImportedPaths(inventory, 4096);
    Expect(duplicate.Succeeded() && duplicate.value && duplicate.value->empty()
        && code(duplicate, "PROJECT_SEARCH_IMPORT_PATH_IDENTITY_INVALID"), "duplicate identity selected the first metadata row");
    std::filesystem::path::string_type invalid(4, std::filesystem::path::value_type{'x'}); invalid[1] = 0;
    inventory = {item("owned.blp"), item(std::filesystem::path(invalid)), item("suffix.mdx")};
    const auto nul = PrepareImportedPaths(inventory, 4096);
    Expect(nul.Succeeded() && nul.value && nul.value->empty() && code(nul, "PROJECT_SEARCH_IMPORT_PATH_INVALID"),
        "NUL path was treated as a shortened first identity");
#ifdef _WIN32
    inventory = {item(std::filesystem::path(std::wstring(683, static_cast<wchar_t>(0x4e2d))))};
    const auto encoded = PrepareImportedPaths(inventory, 4096);
    Expect(encoded.Succeeded() && encoded.value && encoded.value->empty()
        && code(encoded, "PROJECT_SEARCH_IMPORT_PATH_BYTES_LIMIT"), "native length admitted a UTF8 path above the byte limit");
#endif
    std::cout << "PROJECT_SEARCH_IMPORT_INPUT native_limit=2048 utf8_total=1048576 row_limit=4097 no_files_created=true\n";
}

} // namespace

int main() {
    try {
        TestToolSurface();
        TestLuaListPagingProtocol();
        TestArchiveMemberIdentities();
        TestArchiveMapDataQuery();
        TestArchiveMemberTools();
        TestMapDataPageBinding();
        TestProjectSearchTypePreparation();
        TestProjectSearchImportInputBudgets();
        std::cout << "War3Map MCP safe protocol tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "War3Map MCP safe protocol tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
