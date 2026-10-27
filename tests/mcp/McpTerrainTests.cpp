#include "McpServer.h"
#include "../support/MapCustomSettingsFixture.h"
#include "McpTools.h"
#include "McpWorkspace.h"
#include "McpTerrain.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Workspace.h"
#include "../../src/Services/MapEditSaveValidation.h"
#include "../support/TerrainTextureSlotSaveFixture.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Mcp;

void Expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Bytes {
    std::vector<std::byte> data;
    void U8(std::uint8_t value) { data.push_back(static_cast<std::byte>(value)); }
    void U16(std::uint16_t value) { U8(static_cast<std::uint8_t>(value)); U8(static_cast<std::uint8_t>(value >> 8)); }
    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) U8(static_cast<std::uint8_t>(value >> shift));
    }
    void Raw(std::string_view text) { for (unsigned char c : text) U8(c); }
};

Bytes Terrain(std::uint32_t width = 6, std::uint32_t height = 5) {
    Bytes bytes;
    bytes.Raw("W3E!"); bytes.U32(11); bytes.U8('L'); bytes.U32(1);
    bytes.U32(2); bytes.Raw("TesA"); bytes.Raw(std::string_view("\0\xffT0", 4));
    bytes.U32(1); bytes.Raw("ClfZ"); bytes.U32(width); bytes.U32(height);
    bytes.U32(std::bit_cast<std::uint32_t>(-192.25f));
    bytes.U32(std::bit_cast<std::uint32_t>(64.5f));
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool marked = x == 2 && y == 2;
            bytes.U16(static_cast<std::uint16_t>(8192 + 4 * x + 8 * y));
            bytes.U16(static_cast<std::uint16_t>(marked ? 0xe008 : 8192));
            bytes.U8(static_cast<std::uint8_t>(marked ? 0xff : 0));
            bytes.U8(static_cast<std::uint8_t>(marked ? 0xbf : 0));
            bytes.U8(static_cast<std::uint8_t>(marked ? 0x32 : 2));
        }
    }
    return bytes;
}

void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) throw std::runtime_error("fixture write failed");
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    Write(path, std::as_bytes(std::span(text.data(), text.size())));
}

struct Fixture {
    QTemporaryDir temporary;
    std::filesystem::path root, source, ui;
    Fixture(bool withTerrain = true, bool rootTerrain = false, std::uint32_t size = 0) {
        Expect(temporary.isValid(), "unique test directory unavailable");
        root = temporary.path().toStdWString(); source = root / "source"; ui = root / "ui";
        Populate(root, withTerrain, rootTerrain, size);
    }
    static void Populate(const std::filesystem::path& target, bool withTerrain = true,
        bool rootTerrain = false, std::uint32_t size = 0) {
        const auto fixtureSource = target / "source", fixtureUi = target / "ui";
        WriteText(fixtureUi / "config", "base\n");
        WriteText(fixtureUi / "base" / "action.txt", "[DoNothing]\ntitle=Do nothing\n");
        Bytes wtg; wtg.Raw("WTG!"); wtg.U32(7); wtg.U32(0); wtg.U32(2); wtg.U32(0); wtg.U32(0);
        Bytes wct; wct.U32(1); wct.U8(0); wct.U32(0); wct.U32(0);
        Write(fixtureSource / "map" / "war3map.wtg", wtg.data);
        Write(fixtureSource / "map" / "war3map.wct", wct.data);
        if (withTerrain) Write(fixtureSource / (rootTerrain ? "war3map.w3e" : "map/war3map.w3e"),
            (size == 0 ? Terrain() : Terrain(size, size)).data);
        WriteText(fixtureSource / "sentinel.txt", "MCP reads must preserve this source\n");
    }
    void Open(McpServer& server) const {
        server.SetWorkspace(source, ui);
        if (server.Workspace() == nullptr) {
            throw std::runtime_error("fixture open failed: " + server.InitializationDiagnostics().front().code);
        }
    }
};

QJsonObject RevisionJson(const ProjectRevision& value) {
    return {{"generation", QString::number(value.generation)},
        {"source_sha256", QString::fromStdString(value.sourceSha256)},
        {"ui_catalog_sha256", QString::fromStdString(value.uiCatalogSha256)}};
}

QByteArray Request(const char* name, const QJsonObject& arguments) {
    return QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}}).toJson(QJsonDocument::Compact);
}

QJsonObject Call(McpServer& server, const char* name, const QJsonObject& arguments) {
    const auto response = server.HandleMessage(Request(name, arguments));
    Expect(response && !response->error, "unexpected protocol error");
    const auto envelope = response->result.toObject();
    const auto content = envelope.value("content").toArray();
    Expect(content.size() == 1 && content[0].toObject().value("type") == "text", "unexpected MCP content");
    const auto json = QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8());
    Expect(json.isObject(), "tool result is not typed JSON");
    const auto result = json.object();
    Expect(envelope.value("isError").toBool() == !result.value("ok").toBool(), "MCP error envelope disagrees");
    return result;
}

void Code(const QJsonObject& result, const char* code) {
    Expect(!result.value("ok").toBool() && !result.contains("vertices") && !result.contains("cells"),
        "failure returned a partial terrain block");
    const auto diagnostics = result.value("diagnostics").toArray();
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.toObject().value("code").toString() == QLatin1String(code)) return;
    }
    throw std::runtime_error("missing diagnostic " + std::string(code) + ": " +
        QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString());
}

QJsonObject Snapshot(McpServer& server) {
    auto result = Call(server, "get_terrain_snapshot", {{"expected_revision", RevisionJson(server.Workspace()->Revision())}});
    if (!result.value("ok").toBool()) throw std::runtime_error(QJsonDocument(result).toJson().toStdString());
    return result;
}

QJsonObject QueryArgs(const QJsonObject& snapshot) {
    return {{"expected_revision", snapshot.value("revision")},
        {"expected_terrain_binding", snapshot.value("terrain_binding")},
        {"tiles", QJsonObject{{"x", 1}, {"y", 1}, {"width", 2}, {"height", 2}}}};
}

void InvalidParams(McpServer& server, const char* name, const QJsonObject& arguments) {
    const auto response = server.HandleMessage(Request(name, arguments));
    Expect(response && response->error && response->error->value("code").toInt() == JsonRpcResponse::kInvalidParams,
        "invalid JSON/schema input reached the service");
}

void TestTypedReadAndSourcePreservation() {
    Fixture fixture;
    const auto before = DirectoryWorkspace::Open(fixture.source);
    Expect(before.Succeeded(), "cannot record source manifest");
    McpServer server; RegisterAllTools(server); fixture.Open(server);
    const auto revision = server.Workspace()->Revision();
    const auto snapshot = Snapshot(server);
    const auto binding = snapshot.value("terrain_binding").toObject();
    Expect(!binding.value("document_id").toString().isEmpty() && binding.value("document_generation").isString(),
        "host open identity missing");
    const auto terrainSha = Sha256(Terrain().data);
    Expect(terrainSha.Succeeded() && binding.value("terrain_sha256").toString().toStdString() == *terrainSha.value,
        "terrain identity was not derived from actual W3E bytes");
    Expect(binding.value("source_sha256").toString().toStdString() == revision.sourceSha256,
        "terrain source identity detached from workspace");
    const auto header = snapshot.value("header").toObject();
    Expect(header.value("vertex_width") == 6 && header.value("tile_width") == 5 &&
        header.value("ground_rawcodes_hex").toArray()[1] == "00ff5430", "header/rawcode fidelity lost");
    const auto block = Call(server, "query_terrain_block", QueryArgs(snapshot));
    Expect(block.value("ok").toBool() && !block.value("is_render_capture").toBool(), "query failed or claimed screenshot");
    const auto vertices = block.value("vertices").toArray(), cells = block.value("cells").toArray();
    Expect(vertices.size() == 9 && cells.size() == 4, "tile rectangle confused with vertex rectangle");
    const auto first = vertices[0].toObject(), marked = vertices[4].toObject();
    Expect(first.value("source_index") == 7 && first.value("x") == 1 && first.value("y") == 1,
        "global vertex address lost");
    Expect(first.value("position_world").toArray() == QJsonArray{-64.25, 192.5, 3}, "wrong world coordinates");
    Expect(marked.value("ground_height_raw") == 8216 && marked.value("water_height_and_flags_raw") == 0xe008 &&
        marked.value("texture_and_flags_raw") == 255 && marked.value("variation_raw") == 191 &&
        marked.value("cliff_and_layer_raw") == 50 && marked.value("unknown_water_flags") == 32768 &&
        marked.value("is_ramp").toBool() && marked.value("has_water").toBool(), "raw bits/typed flags lost");
    Expect(block.value("unknown_water_flag_vertices") == 1 && block.value("unresolved_ground_texture_vertices") == 1 &&
        block.value("unresolved_cliff_texture_vertices") == 1, "unknown data was flattened to absent");
    Expect(cells[0].toObject().value("source_index") == 6 &&
        cells[0].toObject().value("source_vertices_sw_se_nw_ne").toArray() == QJsonArray{7, 8, 13, 14} &&
        cells[0].toObject().value("local_vertices_sw_se_nw_ne").toArray() == QJsonArray{0, 1, 3, 4}, "corner identities wrong");
    Expect(server.Workspace()->Revision() == revision && !server.Workspace()->HasActiveTransaction(),
        "read created a content revision or transaction");
    Expect(before.value->VerifyUnchanged().Succeeded(), "readonly MCP modified source tree");
}

void TestStaleBindingsAndLifetime() {
    Fixture fixture; McpServer server; RegisterAllTools(server); fixture.Open(server);
    const auto snapshot = Snapshot(server);
    const auto original = QueryArgs(snapshot);
    for (const char* field : {"document_id", "document_generation", "revision", "source_sha256", "terrain_sha256"}) {
        auto args = original;
        auto binding = args.value("expected_terrain_binding").toObject();
        binding[field] = (std::string_view(field).ends_with("sha256")) ? QJsonValue(QString(64, 'f')) :
            QJsonValue(QStringLiteral("9007199254740993"));
        args["expected_terrain_binding"] = binding;
        Code(Call(server, "query_terrain_block", args), "TERRAIN_INSPECT_STALE");
    }
    auto staleRevision = original;
    auto revision = staleRevision.value("expected_revision").toObject(); revision["generation"] = "9007199254740993";
    staleRevision["expected_revision"] = revision;
    Code(Call(server, "query_terrain_block", staleRevision), "MCP_REVISION_CONFLICT");
    for (const char* field : {"source_sha256", "ui_catalog_sha256"}) {
        auto wrong = original; auto expected = wrong.value("expected_revision").toObject();
        expected[field] = QString(64, 'f'); wrong["expected_revision"] = expected;
        Code(Call(server, "query_terrain_block", wrong), "MCP_REVISION_CONFLICT");
    }
    fixture.Open(server);
    Code(Call(server, "query_terrain_block", original), "TERRAIN_INSPECT_STALE");
    const auto reopened = Snapshot(server);
    Expect(reopened.value("terrain_binding").toObject().value("document_id") !=
        snapshot.value("terrain_binding").toObject().value("document_id"), "reopen reused stale document ID");
    Expect(reopened.value("terrain_binding").toObject().value("document_generation") == "2", "open generation not advanced");
    Expect(Call(server, "query_terrain_block", QueryArgs(reopened)).value("ok").toBool(), "new open binding failed");
    McpServer independent; RegisterAllTools(independent); fixture.Open(independent);
    Code(Call(independent, "query_terrain_block", QueryArgs(reopened)), "TERRAIN_INSPECT_STALE");

    WriteText(fixture.source / "sentinel.txt", "changed after terrain was cached");
    Code(Call(server, "query_terrain_block", QueryArgs(reopened)), "SOURCE_CHANGED");
    Code(Call(server, "get_terrain_snapshot", {{"expected_revision", reopened.value("revision")}}), "SOURCE_CHANGED");
}

void TestTransactionsAndFailures() {
    Fixture fixture; McpServer server; RegisterAllTools(server); fixture.Open(server);
    const auto snapshot = Snapshot(server);
    const auto revision = server.Workspace()->Revision();
    auto transaction = server.Workspace()->BeginEdit(revision);
    Expect(transaction.Succeeded(), "fixture transaction failed");
    Code(Call(server, "query_terrain_block", QueryArgs(snapshot)), "MCP_TERRAIN_TRANSACTION_ACTIVE");
    Code(Call(server, "get_terrain_snapshot", {{"expected_revision", snapshot.value("revision")}}), "MCP_TERRAIN_TRANSACTION_ACTIVE");
    Expect(server.Workspace()->Rollback(*transaction.value, revision).Succeeded(), "fixture rollback failed");
    Expect(Call(server, "query_terrain_block", QueryArgs(snapshot)).value("ok").toBool(), "rollback did not restore readonly access");
    Expect(server.Workspace()->Revision() == revision, "readonly refusals changed revision");

    // 惰性载入前的来源改变与缓存建立后的改变都必须拒绝。
    Fixture drift; drift.Open(server);
    const auto oldRevision = RevisionJson(server.Workspace()->Revision());
    Write(drift.source / "map" / "war3map.w3e", Terrain(7, 5).data);
    Code(Call(server, "get_terrain_snapshot", {{"expected_revision", oldRevision}}), "SOURCE_CHANGED");
    Fixture catalogDrift; catalogDrift.Open(server);
    const auto catalogSnapshot = Snapshot(server);
    WriteText(catalogDrift.ui / "base" / "action.txt", "[OtherFunction]\ntitle=Changed catalog\n");
    Code(Call(server, "query_terrain_block", QueryArgs(catalogSnapshot)), "MCP_UI_CATALOG_CHANGED");

    Fixture missing(false); missing.Open(server);
    Code(Call(server, "get_terrain_snapshot", {{"expected_revision", RevisionJson(server.Workspace()->Revision())}}), "MCP_TERRAIN_MISSING");
    Fixture ambiguous; Write(ambiguous.source / "war3map.w3e", Terrain().data); ambiguous.Open(server);
    Code(Call(server, "get_terrain_snapshot", {{"expected_revision", RevisionJson(server.Workspace()->Revision())}}), "MCP_TERRAIN_AMBIGUOUS");
    Fixture corrupt; WriteText(corrupt.source / "map" / "war3map.w3e", "W3E!"); corrupt.Open(server);
    const auto result = Call(server, "get_terrain_snapshot", {{"expected_revision", RevisionJson(server.Workspace()->Revision())}});
    Expect(!result.value("ok").toBool() && !result.value("diagnostics").toArray().isEmpty(), "malformed W3E silently accepted");
    Fixture atRoot(true, true); atRoot.Open(server);
    Expect(Snapshot(server).value("member_path") == "war3map.w3e", "root terrain path not supported");
    server.SetWorkspace(atRoot.source / "war3map.w3e", atRoot.ui);
    Code(Call(server, "get_terrain_snapshot", {{"expected_revision", snapshot.value("revision")}}), "MCP_WORKSPACE_NOT_DIRECTORY");
    McpServer unopened; RegisterAllTools(unopened);
    Code(Call(unopened, "get_terrain_snapshot", {{"expected_revision", snapshot.value("revision")}}), "MCP_TERRAIN_UNAVAILABLE");
}

void TestSchemaAndBudgets() {
    Fixture fixture(true, false, 20); McpServer server; RegisterAllTools(server); fixture.Open(server);
    const auto snapshot = Snapshot(server);
    auto args = QueryArgs(snapshot);
    args["tiles"] = QJsonObject{{"x", 0}, {"y", 0}, {"width", 16}, {"height", 16}};
    Expect(Call(server, "query_terrain_block", args).value("ok").toBool(), "full allowed 256 tile block failed");
    auto tooBig = args;
    tooBig["tiles"] = QJsonObject{{"x", 0}, {"y", 0}, {"width", 17}, {"height", 16}};
    Code(Call(server, "query_terrain_block", tooBig), "TERRAIN_INSPECT_BUDGET");
    for (const auto& limits : {QJsonObject{{"max_tiles", 255}}, QJsonObject{{"max_vertices", 288}}}) {
        auto tight = args; tight["limits"] = limits;
        Code(Call(server, "query_terrain_block", tight), "TERRAIN_INSPECT_BUDGET");
    }
    const auto valid = Call(server, "query_terrain_block", args);
    const auto jsonSize = QJsonDocument(valid).toJson(QJsonDocument::Compact).size();
    args["limits"] = QJsonObject{{"max_json_bytes", static_cast<qint64>(jsonSize)}};
    Expect(Call(server, "query_terrain_block", args) == valid, "exact JSON byte budget should pass unchanged");
    args["limits"] = QJsonObject{{"max_json_bytes", static_cast<qint64>(jsonSize - 1)}};
    Code(Call(server, "query_terrain_block", args), "MCP_TERRAIN_JSON_BUDGET");
    for (const auto& limits : {QJsonObject{{"max_tiles", 257}}, QJsonObject{{"max_vertices", 1025}},
            QJsonObject{{"max_json_bytes", 1048577}}, QJsonObject{{"max_tiles", 0}}, QJsonObject{{"max_tiles", -1}}}) {
        auto invalid = QueryArgs(snapshot); invalid["limits"] = limits;
        Code(Call(server, "query_terrain_block", invalid), "MCP_TERRAIN_LIMITS_INVALID");
    }
    for (const auto& tiles : {QJsonObject{{"x", -1}, {"y", 0}, {"width", 1}, {"height", 1}},
            QJsonObject{{"x", 0}, {"y", 0}, {"width", 0}, {"height", 1}}}) {
        auto invalid = QueryArgs(snapshot); invalid["tiles"] = tiles;
        Code(Call(server, "query_terrain_block", invalid), "MCP_TERRAIN_RECTANGLE_INVALID");
    }
    auto outside = QueryArgs(snapshot);
    outside["tiles"] = QJsonObject{{"x", 18}, {"y", 18}, {"width", 2}, {"height", 1}};
    Code(Call(server, "query_terrain_block", outside), "TERRAIN_INSPECT_BOUNDS");
    outside["tiles"] = QJsonObject{{"x", 2147483647}, {"y", 0}, {"width", 2147483647}, {"height", 1}};
    Code(Call(server, "query_terrain_block", outside), "TERRAIN_INSPECT_BOUNDS");
    outside["tiles"] = QJsonObject{{"x", 4294967295.0}, {"y", 0}, {"width", 1}, {"height", 1}};
    Code(Call(server, "query_terrain_block", outside), "MCP_INTEGER_INVALID");
    InvalidParams(server, "get_terrain_snapshot", {});
    auto invalid = QueryArgs(snapshot); invalid.remove("expected_terrain_binding");
    InvalidParams(server, "query_terrain_block", invalid);
    invalid = QueryArgs(snapshot); auto rectangle = invalid.value("tiles").toObject(); rectangle["x"] = 1.25;
    invalid["tiles"] = rectangle; InvalidParams(server, "query_terrain_block", invalid);
    invalid = QueryArgs(snapshot); rectangle["x"] = "1"; invalid["tiles"] = rectangle;
    InvalidParams(server, "query_terrain_block", invalid);
    for (const char* object : {"expected_revision", "expected_terrain_binding", "tiles", "limits"}) {
        invalid = QueryArgs(snapshot); auto nested = invalid.value(object).toObject(); nested["unexpected"] = true;
        invalid[object] = nested; InvalidParams(server, "query_terrain_block", invalid);
    }
    invalid = QueryArgs(snapshot); auto binding = invalid.value("expected_terrain_binding").toObject();
    binding["revision"] = 1; invalid["expected_terrain_binding"] = binding;
    InvalidParams(server, "query_terrain_block", invalid);
    binding["revision"] = "18446744073709551616"; invalid["expected_terrain_binding"] = binding;
    Code(Call(server, "query_terrain_block", invalid), "MCP_ID_INVALID");
    binding = snapshot.value("terrain_binding").toObject(); binding["document_id"] = QString(129, 'x');
    invalid["expected_terrain_binding"] = binding;
    Code(Call(server, "query_terrain_block", invalid), "MCP_TERRAIN_BINDING_INVALID");
}

QJsonObject BlightSaveArgs(const Fixture& fixture) {
    return {{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')},
        {"expected_terrain_sha256", QString(64, 'b')},
        {"vertex_x", 2}, {"vertex_y", 2}, {"radius", 1}, {"set_blight", true}};
}

struct ScopedSaveRoots {
    const bool hadYdwe = qEnvironmentVariableIsSet("FORGE_MCP_YDWE_ROOT");
    const bool hadWar3 = qEnvironmentVariableIsSet("FORGE_MCP_WAR3_ROOT");
    const QByteArray previousYdwe = qgetenv("FORGE_MCP_YDWE_ROOT");
    const QByteArray previousWar3 = qgetenv("FORGE_MCP_WAR3_ROOT");
    ScopedSaveRoots() {
        qunsetenv("FORGE_MCP_YDWE_ROOT");
        qunsetenv("FORGE_MCP_WAR3_ROOT");
    }
    ~ScopedSaveRoots() {
        if (hadYdwe) qputenv("FORGE_MCP_YDWE_ROOT", previousYdwe);
        else qunsetenv("FORGE_MCP_YDWE_ROOT");
        if (hadWar3) qputenv("FORGE_MCP_WAR3_ROOT", previousWar3);
        else qunsetenv("FORGE_MCP_WAR3_ROOT");
    }
    void Set(const std::filesystem::path& root) {
        const auto encoded = QString::fromStdWString(root.wstring()).toUtf8();
        qputenv("FORGE_MCP_YDWE_ROOT", encoded);
        qputenv("FORGE_MCP_WAR3_ROOT", encoded);
    }
};

void TestBlightSaveTransportRejectsInvalidAndOccupiedCandidates() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    auto args = BlightSaveArgs(fixture);
    auto invalid = args; invalid.remove("expected_terrain_sha256");
    InvalidParams(server, "save_terrain_blight_candidate", invalid);
    invalid = args; invalid["set_blight"] = "true";
    InvalidParams(server, "save_terrain_blight_candidate", invalid);
    invalid = args; invalid["member_bytes"] = "raw replacement";
    InvalidParams(server, "save_terrain_blight_candidate", invalid);
    invalid = args; invalid["tool_root"] = QString::fromStdWString(fixture.ui.wstring());
    InvalidParams(server, "save_terrain_blight_candidate", invalid);
    invalid = args; invalid["game_root"] = QString::fromStdWString(fixture.ui.wstring());
    InvalidParams(server, "save_terrain_blight_candidate", invalid);
    invalid = args; invalid["radius"] = 17;
    Code(Call(server, "save_terrain_blight_candidate", invalid), "MCP_TERRAIN_BRUSH_BOUNDS");
    invalid = args; invalid["expected_source_sha256"] = QString(64, 'A');
    InvalidParams(server, "save_terrain_blight_candidate", invalid);
    invalid = args; invalid["output_path"] = "candidate.w3x";
    Code(Call(server, "save_terrain_blight_candidate", invalid), "MCP_MAP_PATH_INVALID");

    WriteText(fixture.root / "candidate.w3x", "existing user output");
    Code(Call(server, "save_terrain_blight_candidate", args), "MCP_OUTPUT_EXISTS");
    Expect(std::filesystem::file_size(fixture.root / "candidate.w3x") == 20,
        "occupied candidate was changed");
}

void TestBlightSaveForwardsServiceDiagnosticsAndRejectsActiveOverlay() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    const auto args = BlightSaveArgs(fixture);
    ScopedSaveRoots roots;
    Code(Call(server, "save_terrain_blight_candidate", args), "MCP_SAVE_HOST_CONFIG_MISSING");
    roots.Set(fixture.ui);
    Code(Call(server, "save_terrain_blight_candidate", args), "SOURCE_LEASE_FAILED");
    Expect(!std::filesystem::exists(fixture.root / "candidate.w3x"),
        "service failure produced a partial candidate");

    fixture.Open(server);
    const auto revision = server.Workspace()->Revision();
    auto transaction = server.Workspace()->BeginEdit(revision);
    Expect(transaction.Succeeded() && transaction.value, "fixture transaction failed");
    Code(Call(server, "save_terrain_blight_candidate", args), "MCP_TERRAIN_TRANSACTION_ACTIVE");
    Expect(server.Workspace()->Rollback(*transaction.value, revision).Succeeded(),
        "fixture rollback failed");
    Expect(!std::filesystem::exists(fixture.root / "candidate.w3x"),
        "active overlay refusal produced a candidate");
}

void TestShadowSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    auto args = BlightSaveArgs(fixture);
    args.remove("vertex_x"); args.remove("vertex_y"); args.remove("radius"); args.remove("set_blight");
    auto invalid = args; invalid.remove("expected_terrain_sha256");
    InvalidParams(server, "save_terrain_shadow_candidate", invalid);
    invalid = args; invalid["game_root"] = QString::fromStdWString(fixture.ui.wstring());
    InvalidParams(server, "save_terrain_shadow_candidate", invalid);
    invalid = args; invalid["shadow_bytes"] = "untrusted replacement";
    InvalidParams(server, "save_terrain_shadow_candidate", invalid);
    ScopedSaveRoots roots;
    Code(Call(server, "save_terrain_shadow_candidate", args), "MCP_SAVE_HOST_CONFIG_MISSING");
    roots.Set(fixture.ui);
    Code(Call(server, "save_terrain_shadow_candidate", args), "SOURCE_LEASE_FAILED");
    Expect(!std::filesystem::exists(fixture.root / "candidate.w3x"), "shadow preparation failure wrote a candidate");
    fixture.Open(server);
    const auto revision = server.Workspace()->Revision();
    auto transaction = server.Workspace()->BeginEdit(revision);
    Expect(transaction.Succeeded() && transaction.value, "shadow fixture transaction failed");
    Code(Call(server, "save_terrain_shadow_candidate", args), "MCP_TERRAIN_TRANSACTION_ACTIVE");
    Expect(server.Workspace()->Rollback(*transaction.value, revision).Succeeded(), "shadow fixture rollback failed");
    WriteText(fixture.root / "candidate.w3x", "existing user output");
    Code(Call(server, "save_terrain_shadow_candidate", args), "MCP_OUTPUT_EXISTS");
    Expect(std::filesystem::file_size(fixture.root / "candidate.w3x") == 20, "shadow overwrote occupied candidate");
}

void TestNormalSaveTextureAndWaterSchema() {
    McpServer server; RegisterAllTools(server);
    const auto response = server.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    Expect(response && !response->error, "terrain save tool list must be available");
    bool texture = false, water = false, waterBrush = false, ramp = false, corner = false, straight = false, bounds = false;
    bool cliffLevel = false, cliffTexture = false, boundary = false;
    for (const auto& tool : response->result.toObject().value("tools").toArray()) {
        if (tool.toObject().value("name") != "save_map_edit_candidate") continue;
        const auto properties = tool.toObject().value("inputSchema").toObject().value("properties").toObject();
        for (const auto& variant : properties.value("terrain_strokes").toObject().value("items").toObject().value("oneOf").toArray()) {
            const auto fields = variant.toObject().value("properties").toObject();
            const auto kind = fields.value("kind").toObject().value("const").toString();
            if (kind == "texture") texture = fields.contains("ground_texture") && !fields.contains("strength");
            if (kind == "water_height") water = fields.value("water_height").toObject().value("minimum").toInt() == -2048
                && !fields.contains("strength");
            if (kind == "ramp") {
                const auto required = variant.toObject().value("required").toArray();
                ramp = fields.value("set_ramp").toObject().value("type") == "boolean"
                    && required.contains("set_ramp") && fields.value("shape").toObject().value("const") == "circle"
                    && fields.value("radius").toObject().contains("const")
                    && fields.value("radius").toObject().value("const").toInt(-1) == 0
                    && !fields.contains("strength") && !fields.contains("water_height") && !fields.contains("set_blight");
            }
            if (kind == "boundary") {
                const auto required = variant.toObject().value("required").toArray();
                boundary = fields.size() == 7 && required.size() == 7 && required.contains("value")
                    && fields.value("value").toObject().value("type") == "boolean"
                    && fields.value("radius").toObject().value("minimum").toInt(-1) == 0
                    && fields.value("radius").toObject().value("maximum").toInt() == 16
                    && fields.value("shape").toObject().value("enum").toArray() == QJsonArray{"circle", "square"}
                    && fields.value("follow_doodads").toObject().value("const") == false
                    && !variant.toObject().value("additionalProperties").toBool(true);
            }
            if (kind == "corner_ramp" || kind == "straight_ramp") {
                const auto required = variant.toObject().value("required").toArray();
                const bool nativeRamp = fields.size() == 5 && required.size() == 5
                    && required.contains("set_ramp") && fields.value("set_ramp").toObject().value("type") == "boolean"
                    && fields.value("point_space").toObject().value("const") == "world"
                    && fields.value("points").toObject().value("maxItems").toInt() == 1
                    && fields.value("follow_doodads").toObject().value("const") == false
                    && !variant.toObject().value("additionalProperties").toBool(true);
                if (kind == "corner_ramp") corner = nativeRamp;
                else straight = nativeRamp;
            }
            if (kind == "playable_bounds") {
                const auto required = variant.toObject().value("required").toArray();
                bounds = fields.size() == 5 && required.size() == 5 && required.contains("west") && required.contains("north")
                    && fields.value("east").toObject().value("maximum").toInt() == 2048
                    && !variant.toObject().value("additionalProperties").toBool(true);
            }
            if (kind == "cliff_level" || kind == "cliff_texture") {
                const auto required = variant.toObject().value("required").toArray();
                const bool common = fields.value("size").toObject().value("minimum").toInt() == 1
                    && fields.value("size").toObject().value("maximum").toInt() == 8
                    && fields.value("cliff_texture").toObject().value("maximum").toInt() == 14
                    && required.contains("cliff_texture") && required.contains("size")
                    && fields.value("follow_doodads").toObject().value("const") == false
                    && !fields.contains("radius") && !fields.contains("anchor") && !fields.contains("enforce_water_limits")
                    && !fields.contains("strength") && !variant.toObject().value("additionalProperties").toBool(true);
                if (kind == "cliff_level") cliffLevel = common && fields.size() == 9 && required.size() == 9
                    && required.contains("level_delta") && required.contains("seed")
                    && fields.value("level_delta").toObject().value("minimum").toInt() == -1
                    && fields.value("level_delta").toObject().value("maximum").toInt() == 1
                    && fields.value("seed").toObject().value("maximum").toDouble() == 4294967295.0;
                else cliffTexture = common && fields.size() == 7 && required.size() == 7
                    && !fields.contains("level_delta") && !fields.contains("seed");
            }
            const auto kinds = fields.value("kind").toObject().value("enum").toArray();
            if (kinds.contains("shallow_water") && kinds.contains("deep_water")) {
                const auto required = variant.toObject().value("required").toArray();
                waterBrush = fields.value("size").toObject().value("maximum").toInt() == 8
                    && fields.value("seed").toObject().value("maximum").toDouble() == 4294967295.0
                    && required.contains("cliff_texture") && required.contains("seed")
                    && required.contains("enforce_water_limits") && !fields.contains("radius")
                    && !fields.contains("anchor") && !fields.contains("water_height") && !fields.contains("strength");
            }
        }
    }
    Expect(texture && water, "normal save must expose typed texture and water parameters without overloading strength");
    Expect(waterBrush, "new shallow/deep water must expose explicit replay options and no caller-supplied anchor or rules");
    Expect(ramp, "ramp save schema requires a real set_ramp boolean, circle and radius0 without height or blight aliases");
    Expect(corner, "corner schema only exposes one world point, explicit set_ramp and no caller-supplied brush or rules");
    Expect(straight, "straight schema requires its own kind with a single world point and no caller-supplied rules");
    Expect(bounds, "bounds schema only exposes integer half-open cell edges without fake points, rules or object movement");
    Expect(cliffLevel && cliffTexture, "cliff level and pure texture expose distinct strict classic-size contracts");
    Expect(boundary, "boundary schema only exposes its explicit boolean and bounded brush fields");
}

void TestPlayableBoundsSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject stroke{{"kind", "playable_bounds"}, {"west", 1}, {"south", 2}, {"east", 17}, {"north", 14}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "bounds-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](QJsonObject value) {
        args["terrain_strokes"] = QJsonArray{value}; return Call(server, "save_map_edit_candidate", args);
    };
    Code(call(stroke), "SOURCE_LEASE_FAILED");
    for (const auto field : {"west", "south", "east", "north"}) {
        auto invalid = stroke; invalid.remove(field); Code(call(invalid), "MCP_INTEGER_REQUIRED");
        invalid = stroke; invalid[field] = 1.5; Code(call(invalid), "MCP_INTEGER_INVALID");
        invalid = stroke; invalid[field] = "1"; Code(call(invalid), "MCP_INTEGER_REQUIRED");
    }
    for (const auto [field, value] : {std::pair{"west", -1}, {"east", 1}, {"north", 2049}, {"south", 14}}) {
        auto invalid = stroke; invalid[field] = value; Code(call(invalid), "MCP_MAP_EDIT_BOUNDS_INVALID");
    }
    for (const auto field : {"points", "follow_doodads", "shape", "point_space", "water_height", "actual_margins", "rules_sha256"}) {
        auto invalid = stroke; invalid[field] = 1; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    Expect(!std::filesystem::exists(fixture.root / "bounds-candidate.w3x"), "invalid bounds cannot publish partial map candidates");
}

void TestRampSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject stroke{{"kind", "ramp"}, {"shape", "circle"}, {"point_space", "world"}, {"radius", 0},
        {"set_ramp", true}, {"points", QJsonArray{QJsonObject{{"x", 448}, {"y", 512}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "ramp-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](QJsonObject value) {
        args["terrain_strokes"] = QJsonArray{value};
        return Call(server, "save_map_edit_candidate", args);
    };
    for (const bool enabled : {true, false}) for (const auto space : {"world", "terrain_vertex"}) {
        auto valid = stroke; valid["set_ramp"] = enabled; valid["point_space"] = space;
        Code(call(valid), "SOURCE_LEASE_FAILED");
    }
    // oneOf在通用schema中不展开；typed gate必须在实际源租约前拒绝以下输入。
    for (const auto invalidValue : {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
        auto invalid = stroke; invalid["set_ramp"] = invalidValue;
        Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RAMP_INVALID");
    }
    auto invalid = stroke; invalid.remove("set_ramp");
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RAMP_INVALID");
    invalid = stroke; invalid["radius"] = 1;
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RADIUS_INVALID");
    invalid = stroke; invalid["radius"] = "0";
    Code(call(invalid), "MCP_INTEGER_REQUIRED");
    invalid = stroke; invalid["radius"] = 0.5;
    Code(call(invalid), "MCP_INTEGER_INVALID");
    invalid = stroke; invalid["kind"] = "unsupported";
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_KIND_UNSUPPORTED");
    invalid = stroke; invalid["shape"] = "square";
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_SHAPE_INVALID");
    for (const auto field : {"strength", "set_blight", "water_height", "size", "anchor", "loaded_rules"}) {
        invalid = stroke; invalid[field] = 1;
        Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    invalid = stroke; invalid["follow_doodads"] = true;
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED");
    Expect(!std::filesystem::exists(fixture.root / "ramp-candidate.w3x"),
        "ramp transport failure must not publish a partial candidate");
}

void TestBoundarySaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject stroke{{"kind", "boundary"}, {"shape", "circle"}, {"point_space", "terrain_vertex"}, {"radius", 0},
        {"value", true}, {"points", QJsonArray{QJsonObject{{"x", 4}, {"y", 4}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "boundary-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](QJsonObject value) {
        args["terrain_strokes"] = QJsonArray{value}; return Call(server, "save_map_edit_candidate", args);
    };
    for (const bool enabled : {true, false}) for (const auto shape : {"circle", "square"})
        for (const auto space : {"world", "terrain_vertex"}) for (const int radius : {0, 16}) {
            auto valid = stroke; valid["value"] = enabled; valid["shape"] = shape;
            valid["point_space"] = space; valid["radius"] = radius; Code(call(valid), "SOURCE_LEASE_FAILED");
        }
    for (const auto value : {QJsonValue("true"), QJsonValue(1), QJsonValue(0), QJsonValue(QJsonValue::Null)}) {
        auto invalid = stroke; invalid["value"] = value; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_BOUNDARY_INVALID");
    }
    auto invalid = stroke; invalid.remove("value"); Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_BOUNDARY_INVALID");
    for (const int radius : {-1, 17}) {
        invalid = stroke; invalid["radius"] = radius; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RADIUS_INVALID");
    }
    invalid = stroke; invalid["radius"] = "0"; Code(call(invalid), "MCP_INTEGER_REQUIRED");
    invalid = stroke; invalid["radius"] = 0.5; Code(call(invalid), "MCP_INTEGER_INVALID");
    invalid = stroke; invalid["shape"] = "triangle"; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_SHAPE_INVALID");
    invalid = stroke; invalid["point_space"] = "cell"; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_SPACE_INVALID");
    for (const auto field : {"set_ramp", "set_blight", "set_boundary", "strength", "water_height", "west", "east",
            "map_edge", "size", "anchor", "loaded_rules"}) {
        invalid = stroke; invalid[field] = true; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    invalid = stroke; invalid["follow_doodads"] = true; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED");
    invalid = stroke; invalid["follow_doodads"] = "false"; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED");
    for (const auto x : {QJsonValue(-1), QJsonValue(1.5), QJsonValue("4")}) {
        invalid = stroke; invalid["points"] = QJsonArray{QJsonObject{{"x", x}, {"y", 4}}};
        Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_POINT_INVALID");
    }
    invalid = stroke; invalid["points"] = QJsonArray{}; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_POINTS_INVALID");
    invalid = stroke; invalid["points"] = QJsonArray{QJsonObject{{"x", 4}, {"y", 4}, {"map_edge", true}}};
    Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    QJsonArray tooMany;
    for (unsigned i = 0; i < 129; ++i) tooMany.append(stroke);
    args["terrain_strokes"] = tooMany; Code(Call(server, "save_map_edit_candidate", args), "MCP_MAP_EDIT_TERRAIN_STROKES_INVALID");
    Expect(!std::filesystem::exists(fixture.root / "boundary-candidate.w3x"), "refused boundary never publishes a partial candidate");
}

void TestCornerRampSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject stroke{{"kind", "corner_ramp"}, {"point_space", "world"}, {"set_ramp", true},
        {"points", QJsonArray{QJsonObject{{"x", 1056}, {"y", 1056}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "corner-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](QJsonObject value) {
        args["terrain_strokes"] = QJsonArray{value}; return Call(server, "save_map_edit_candidate", args);
    };
    for (const bool enabled : {true, false}) {
        auto valid = stroke; valid["set_ramp"] = enabled; Code(call(valid), "SOURCE_LEASE_FAILED");
    }
    auto invalid = stroke; invalid["point_space"] = "terrain_vertex";
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_SPACE_INVALID");
    invalid = stroke; invalid["set_ramp"] = "true";
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RAMP_INVALID");
    invalid = stroke; invalid.remove("set_ramp");
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RAMP_INVALID");
    invalid = stroke; invalid["points"] = QJsonArray{QJsonObject{{"x", 1056}, {"y", 1056}},
        QJsonObject{{"x", 992}, {"y", 1056}}};
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_POINTS_INVALID");
    for (const auto field : {"shape", "radius", "size", "strength", "anchor", "loaded_rules", "enforce_water_limits"}) {
        invalid = stroke; invalid[field] = 1; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    Expect(!std::filesystem::exists(fixture.root / "corner-candidate.w3x"),
        "corner transport failures must never create an output map");
}

void TestCliffSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject common{{"shape", "square"}, {"point_space", "terrain_vertex"}, {"size", 2},
        {"cliff_texture", 1}, {"points", QJsonArray{QJsonObject{{"x", 2}, {"y", 2}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "cliff-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](const QJsonObject& stroke) {
        args["terrain_strokes"] = QJsonArray{stroke}; return Call(server, "save_map_edit_candidate", args);
    };
    for (const bool level : {false, true}) {
        auto stroke = common; stroke["kind"] = level ? "cliff_level" : "cliff_texture";
        if (level) { stroke["level_delta"] = -1; stroke["seed"] = 4294967295.0; }
        for (const auto space : {"world", "terrain_vertex"}) for (const auto shape : {"circle", "square"}) {
            auto valid = stroke; valid["point_space"] = space; valid["shape"] = shape;
            Code(call(valid), "SOURCE_LEASE_FAILED");
        }
        for (const auto field : {"size", "cliff_texture"}) {
            auto invalid = stroke; invalid.remove(field); Code(call(invalid), "MCP_INTEGER_REQUIRED");
            invalid = stroke; invalid[field] = 1.5; Code(call(invalid), "MCP_INTEGER_INVALID");
            invalid = stroke; invalid[field] = "1"; Code(call(invalid), "MCP_INTEGER_REQUIRED");
        }
        for (const auto size : {0, 9}) {
            auto invalid = stroke; invalid["size"] = size; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID");
        }
        for (const auto slot : {-1, 15}) {
            auto invalid = stroke; invalid["cliff_texture"] = slot; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID");
        }
        for (const auto field : {"radius", "anchor", "loaded_rules", "strength", "water_height", "enforce_water_limits", "set_ramp"}) {
            auto invalid = stroke; invalid[field] = 1; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
        }
        auto invalid = stroke; invalid["follow_doodads"] = true;
        Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED");
        if (!level) for (const auto field : {"level_delta", "seed"}) {
            invalid = stroke; invalid[field] = 0; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
        }
        if (level) {
            invalid = stroke; invalid.remove("level_delta"); Code(call(invalid), "MCP_INTEGER_REQUIRED");
            invalid = stroke; invalid["level_delta"] = 0.5; Code(call(invalid), "MCP_INTEGER_INVALID");
            for (const auto delta : {-2, 2}) {
                invalid = stroke; invalid["level_delta"] = delta; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID");
            }
            for (const auto seed : {-1.0, 0.5, 4294967296.0}) {
                invalid = stroke; invalid["seed"] = seed; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID");
            }
            invalid = stroke; invalid.remove("seed"); Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID");
            invalid = stroke; invalid["seed"] = "1"; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_CLIFF_BRUSH_INVALID");
            for (const auto delta : {-1, 0, 1}) for (const auto seed : {0.0, 2147483648.0, 4294967295.0}) {
                auto valid = stroke; valid["level_delta"] = delta; valid["seed"] = seed;
                Code(call(valid), "SOURCE_LEASE_FAILED");
            }
        }
    }
    Expect(!std::filesystem::exists(fixture.root / "cliff-candidate.w3x"), "cliff transport failures never create a partial map");
}

void TestRemoveWaterSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots; roots.Set(fixture.ui);
    const QJsonObject stroke{{"kind", "remove_water"}, {"shape", "square"}, {"point_space", "terrain_vertex"},
        {"radius", 16}, {"points", QJsonArray{QJsonObject{{"x", 0}, {"y", 4}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "remove-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](const QJsonObject& value) {
        args["terrain_strokes"] = QJsonArray{value}; return Call(server, "save_map_edit_candidate", args);
    };
    Code(call(stroke), "SOURCE_LEASE_FAILED");
    for (const auto* field : {"strength", "water_height", "cliff_texture", "seed", "enforce_water_limits", "value", "size", "anchor"}) {
        auto invalid = stroke; invalid[field] = 1; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    auto invalid = stroke; invalid["radius"] = 17; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_RADIUS_INVALID");
    invalid = stroke; invalid["follow_doodads"] = true; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED");
    invalid = stroke; invalid["shape"] = "triangle"; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_SHAPE_INVALID");
}

void TestNativeWaterSaveTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server);
    ScopedSaveRoots roots; roots.Set(fixture.ui);
    QJsonObject stroke{{"kind", "shallow_water"}, {"shape", "circle"}, {"point_space", "terrain_vertex"},
        {"size", 1}, {"cliff_texture", 0}, {"seed", 4294967295.0}, {"enforce_water_limits", true},
        {"points", QJsonArray{QJsonObject{{"x", 2}, {"y", 2}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "water-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')},
        {"terrain_strokes", QJsonArray{stroke}}};
    for (const auto kind : {"shallow_water", "deep_water"}) {
        stroke["kind"] = kind;
        for (const auto seed : {0.0, 2147483647.0, 2147483648.0, 4294967295.0}) {
            stroke["seed"] = seed;
            args["terrain_strokes"] = QJsonArray{stroke};
            Code(Call(server, "save_map_edit_candidate", args), "SOURCE_LEASE_FAILED");
        }
    }
    for (const auto seed : {-1.0, 1.5, 4294967296.0}) {
        auto invalid = stroke;
        invalid["seed"] = seed;
        args["terrain_strokes"] = QJsonArray{invalid};
        const auto response = server.HandleMessage(Request("save_map_edit_candidate", args));
        Expect(response.has_value(), "invalid seed must have an explicit response");
        if (response->error) {
            Expect(response->error->value("code").toInt() == JsonRpcResponse::kInvalidParams,
                "invalid seed must fail at the protocol or typed argument gate");
        } else {
            const auto content = response->result.toObject().value("content").toArray();
            Expect(content.size() == 1, "invalid seed must produce one diagnostic envelope");
            Code(QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8()).object(),
                "MCP_MAP_EDIT_TERRAIN_WATER_BRUSH_INVALID");
        }
    }
    for (const auto field : {"anchor", "loaded_rules", "strength", "water_height", "radius", "set_ramp"}) {
        auto mixed = stroke;
        mixed[field] = 1;
        args["terrain_strokes"] = QJsonArray{mixed};
        // 通用 schema 验证器不展开 oneOf；准确验证 typed gate 在尝试来源租约前拒绝，
        // 不能用 missing.w3x 的 SOURCE_LEASE_FAILED 代替不支持字段的拒绝。
        Code(Call(server, "save_map_edit_candidate", args), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    Expect(!std::filesystem::exists(fixture.root / "water-candidate.w3x"),
        "water transport rejection must not produce a partial map");
}

#include "McpStraightRampSaveCases.h"
#include "McpTerrainBlockSaveCases.h"
#include "McpTerrainPathingSaveCases.h"
#include "TerrainResizeSaveCases.h"
#include "TerrainTextureSlotSaveCases.h"

} // namespace

#include "MapCustomSettingsSaveCases.h"
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        const auto arguments = application.arguments();
        if (arguments.size() == 3 && arguments[1] == "--write-fixture") {
            std::error_code error;
            const auto target = std::filesystem::absolute(
                std::filesystem::path(arguments[2].toStdWString()), error).lexically_normal();
            Expect(!error && !arguments[2].isEmpty(), "fixture output path invalid");
            const bool exists = std::filesystem::exists(target, error);
            Expect(!error, "fixture output existence unknown");
            if (exists) {
                const bool directory = std::filesystem::is_directory(target, error);
                Expect(!error && directory, "fixture output must be an empty directory");
                const bool empty = std::filesystem::is_empty(target, error);
                Expect(!error && empty, "refusing nonempty fixture output directory");
            } else {
                std::filesystem::create_directories(target, error);
                Expect(!error, "cannot create fixture output directory");
            }
            Fixture::Populate(target);
            const QJsonObject receipt{{"workspace", QString::fromStdWString((target / "source").wstring())},
                {"ui_data", QString::fromStdWString((target / "ui").wstring())}, {"synthetic_fixture", true}};
            std::cout << QJsonDocument(receipt).toJson(QJsonDocument::Compact).toStdString() << '\n';
            return 0;
        }
        Expect(arguments.size() == 1, "usage: test executable [--write-fixture <new-or-empty-directory>]");
        TestCustomSettingsSaveAndQuery();
        TestNormalSaveTextureAndWaterSchema();
        TestNativeWaterSaveTransport();
        TestRemoveWaterSaveTransport();
        TestCliffSaveTransport();
        TestRampSaveTransport();
        TestBoundarySaveTransport();
        TestPathingSaveSchemaAndTransport();
        TestCornerRampSaveTransport();
        TestStraightRampSaveTransport();
        TestTerrainBlockSaveTransport();
        TestPlayableBoundsSaveTransport();
        TestTerrainResizeSaveTransport();
        TestTextureSlotSaveTransport();
        TestTypedReadAndSourcePreservation();
        TestStaleBindingsAndLifetime();
        TestTransactionsAndFailures();
        TestSchemaAndBudgets();
        TestBlightSaveTransportRejectsInvalidAndOccupiedCandidates();
        TestBlightSaveForwardsServiceDiagnosticsAndRejectsActiveOverlay();
        TestShadowSaveTransport();
        std::cout << "MCP terrain directory identity, typed JSON and bounded queries passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MCP terrain tests failed: " << error.what() << '\n';
        return 1;
    }
}
