#include "McpServer.h"
#include "McpTools.h"
#include "McpWorkspace.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "../MapDataFixtures.h"
#include "../PlacementCreationFixtures.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <fstream>
#include <map>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Mcp;
using namespace War3Map::MapData;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void Check(const T& value, const char* message) {
    if (value.Succeeded()) return;
    std::string text(message);
    for (const auto& diagnostic : value.diagnostics) text += " [" + diagnostic.code + "]";
    throw std::runtime_error(text);
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(stream), "write owned placement fixture");
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, std::as_bytes(std::span(text))); }
struct GameEnvironment {
    bool hadValue = qEnvironmentVariableIsSet("FORGE_MCP_WAR3_ROOT");
    QByteArray previous = qgetenv("FORGE_MCP_WAR3_ROOT");
    explicit GameEnvironment(const std::filesystem::path& root) {
        qputenv("FORGE_MCP_WAR3_ROOT", QString::fromStdWString(root.wstring()).toUtf8());
    }
    ~GameEnvironment() {
        if (hadValue) qputenv("FORGE_MCP_WAR3_ROOT", previous); else qunsetenv("FORGE_MCP_WAR3_ROOT");
    }
};
QJsonObject Call(McpServer& server, const char* name, const QJsonObject& arguments = {}) {
    const auto request = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}}).toJson(QJsonDocument::Compact);
    const auto reply = server.HandleMessage(request);
    Require(reply && !reply->error, "registered placement tool dispatch/schema");
    const auto content = reply->result.toObject().value("content").toArray();
    Require(content.size() == 1, "registered placement receipt");
    return QJsonDocument::fromJson(content[0].toObject().value("text").toString().toUtf8()).object();
}
void RequireOk(const QJsonObject& result) {
    if (!result.value("ok").toBool()) throw std::runtime_error(QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString());
}

// 原创七类目录放进自有 MPQ，真实宿主按默认游戏成员名读取，无目录注入与本机资源依赖。
void PrepareSources(const std::filesystem::path& root) {
    const auto ui = root / "ui", source = root / "source";
    Write(ui / "config", "base\n");
    Write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n[TriggerParams]\n");
    for (const auto* name : {"action.txt", "event.txt", "condition.txt", "call.txt"}) Write(ui / "base" / name, "");
    const auto catalog = Trigger::UiCatalog::Load(ui); Check(catalog, "registered fixture UI");
    const auto triggers = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value); Check(triggers, "registered empty triggers");
    const auto pair = Trigger::WtgWctCodec{}.Serialize(*triggers.value, *catalog.value); Check(pair, "registered paired triggers");
    Write(source / "map/war3map.wtg", pair.value->wtg); Write(source / "map/war3map.wct", pair.value->wct);
    Write(source / "map/war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes);
    Write(source / "map/war3map.w3e", Tests::PlacementTerrainBytes());
    const auto units = UnitPlacements{}.Serialize(), doodads = DoodadPlacements{}.Serialize();
    Check(units, "registered empty units"); Check(doodads, "registered empty doodads");
    Write(source / "map/war3mapUnits.doo", *units.value); Write(source / "map/war3map.doo", *doodads.value);
    const auto catalogSources = ObjectData::StandardObjectCatalog::DefaultSources();
    const auto schemaSources = ObjectData::ObjectFieldSchema::DefaultSources();
    std::map<std::string, std::vector<Tests::ObjectRow>> metadata;
    std::vector<Mpq::MpqBuildFile> game;
    for (const auto& kind : Tests::ObjectCommandsFixture::kinds) {
        for (const auto& spec : catalogSources) if (spec.type == kind.type) {
            const auto table = Tests::ObjectTable({{{"ID", kind.base}, {"Name", "原创放置类型"}, {"code", kind.base}}});
            game.push_back({spec.slkFiles.front(), Tests::ObjectBytes(table), false});
        }
        for (const auto& spec : schemaSources) if (spec.type == kind.type) metadata[spec.metadataFile].push_back({
            {"ID", kind.nameField}, {"field", "Name"}, {"slk", "Profile"}, {"type", "string"}, {"index", "0"},
            {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"}, {"useItem", "1"}, {"useCreep", "1"}});
        Write(source / "table" / std::string(ObjectData::LniFileName(kind.type)),
            "[" + std::string(kind.custom) + "]\n_parent=\"" + kind.base + "\"\n");
    }
    for (const auto& [path, rows] : metadata) game.push_back({path, Tests::ObjectBytes(Tests::ObjectTable(rows)), false});
    std::filesystem::create_directories(root / "game");
    Check(Mpq::WriteMpqArchive(root / "game/war3.mpq", game), "original game directory MPQ");
}
QJsonObject Base(McpServer& server, const char* kind) {
    const auto page = Call(server, "entities_query", {{"kind", kind}}); RequireOk(page);
    return {{"project_identity", QString::fromStdString(server.DocumentId())},
        {"expected_revision", QString::fromStdString(server.Workspace()->ObjectRevision())}, {"kind", kind},
        {"expected_member_sha256", page.value("member").toObject().value("member_sha256")}};
}
QJsonObject Record(const char* kind, const char* id) {
    return {{"object_kind", kind}, {"object_id", id}, {"x", 64}, {"y", -128}};
}
void RegisteredCreateSaveReopenDelete(const std::filesystem::path& root, const char* kind, const QJsonArray& records) {
    McpServer server; RegisterAllTools(server); server.SetWorkspace(root / "source", root / "ui");
    Require(server.Workspace() != nullptr, "actual standalone placement workspace");
    auto* workspace = server.Workspace();
    auto args = Base(server, kind); args.insert("placements", records); args.insert("dry_run", true);
    const auto initial = workspace->ObjectRevision();
    const auto preview = Call(server, "entities_create", args); RequireOk(preview);
    Require(preview.value("candidate_ids").toArray().size() == 4 && !workspace->HasActiveTransaction()
        && workspace->ObjectRevision() == initial, "registered preview leaves transaction and revision unchanged");
    args["dry_run"] = false;
    Require(!Call(server, "entities_create", args).value("ok").toBool(), "registered actual create requires explicit transaction");
    const auto revision = workspace->Revision(); const auto transaction = workspace->BeginEdit(revision); Check(transaction, "placement transaction");
    Require(!Call(server, "entities_create", args).value("ok").toBool(), "begin invalidates earlier creation identity");
    args = Base(server, kind); args.insert("placements", records);
    const auto created = Call(server, "entities_create", args); RequireOk(created);
    Require(created.value("new_ids").toArray().size() == 4 && created.value("transaction_active").toBool(), "actual host publishes four native rows");
    auto remove = Base(server, kind); QJsonArray sources;
    for (const auto& value : created.value("new_ids").toArray()) {
        const auto row = value.toObject(); sources.append(QJsonObject{{"source_index", row.value("source_index")}, {"row_key", row.value("row_key")}});
    }
    remove.insert("sources", sources);
    Require(!Call(server, "entities_delete", remove).value("ok").toBool(), "registered host retains single-batch recipe gate");
    Check(workspace->PreviewEdit(*transaction.value, revision), "preview sealed create");
    Require(workspace->ValidateEdit(*transaction.value, revision).satisfied, "validate sealed creation");
    const auto output = root / (std::string(kind) + "-created");
    Check(workspace->ExportCandidate(*transaction.value, revision, output), "export real created candidate directory");
    server.SetWorkspace(output, root / "ui"); Require(server.Workspace() != nullptr, "reopen created candidate");
    workspace = server.Workspace();
    const auto rows = Call(server, "entities_query", {{"kind", kind}}); RequireOk(rows);
    Require(rows.value("rows").toArray().size() == 4, "native records survive actual candidate reopen");
    const auto deleteRevision = workspace->Revision(); const auto deleteTransaction = workspace->BeginEdit(deleteRevision); Check(deleteTransaction, "delete transaction");
    remove = Base(server, kind); sources = {};
    for (const auto& value : rows.value("rows").toArray()) {
        const auto row = value.toObject(); sources.append(QJsonObject{{"source_index", row.value("source_index")}, {"row_key", row.value("row_key")}});
    }
    remove.insert("sources", sources);
    RequireOk(Call(server, "entities_delete", remove));
    Check(workspace->PreviewEdit(*deleteTransaction.value, deleteRevision), "preview sealed deletion");
    Require(workspace->ValidateEdit(*deleteTransaction.value, deleteRevision).satisfied, "validate sealed deletion");
    const auto deletedOutput = root / (std::string(kind) + "-deleted");
    Check(workspace->ExportCandidate(*deleteTransaction.value, deleteRevision, deletedOutput), "export real deleted candidate directory");
    const auto member = std::string(kind) == "units" ? "map/war3mapUnits.doo" : "map/war3map.doo";
    const auto empty = IO::ReadFileBytes(root / "source" / member), restored = IO::ReadFileBytes(deletedOutput / member);
    Check(empty, "original empty table"); Check(restored, "exported empty table");
    Require(*empty.value == *restored.value, "deleting created final rows restores original native bytes");
}
}

void TestRegisteredPlacementHost() {
    QTemporaryDir temporary; Require(temporary.isValid(), "placement host temporary directory");
    const auto root = std::filesystem::path(temporary.path().toStdWString());
    PrepareSources(root); GameEnvironment environment(root / "game");
    const auto source = DirectoryWorkspace::Open(root / "source"); Check(source, "original source snapshot");
    RegisteredCreateSaveReopenDelete(root, "units", {Record("unit", "u001"), Record("unit", "U001"), Record("item", "i001"), Record("item", "I001")});
    RegisteredCreateSaveReopenDelete(root, "doodads", {Record("doodad", "D001"), Record("doodad", "D101"), Record("destructable", "b001"), Record("destructable", "b101")});
    const auto unchanged = DirectoryWorkspace::Open(root / "source"); Check(unchanged, "final source snapshot");
    Require(unchanged.value->Snapshot().aggregateSha256 == source.value->Snapshot().aggregateSha256, "registered operations never write source directory");
}
