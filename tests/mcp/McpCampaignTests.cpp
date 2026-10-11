#include "McpCampaignTools.h"
#include "McpTools.h"
#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../../src/QtCommon/MapBackupPaths.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSettings>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Mcp;
namespace {
void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
QJsonObject Call(McpServer& server, const char* name, QJsonObject args = {}) {
    const auto response = server.HandleMessage(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", name}, {"arguments", args}}}}).toJson(QJsonDocument::Compact));
    if (!response || response->error) return {{"ok", false}, {"rpc_error", true}};
    const auto content = response->result.toObject()["content"].toArray();
    Require(!content.isEmpty(), "tool response content");
    return QJsonDocument::fromJson(content.first().toObject()["text"].toString().toUtf8()).object();
}
QJsonObject Identity(const QJsonObject& query) { return {{"document", query["document"]}, {"revision", query["revision"]}}; }
std::vector<std::byte> Bytes(std::string_view text) { std::vector<std::byte> bytes; for (char c : text) bytes.push_back(static_cast<std::byte>(c)); return bytes; }

void SharedSessionRevisionAndPolicy() {
    McpServer server; RegisterCampaignTools(server); auto session = Services::CampaignSession::Create(); server.SetCampaign(session);
    auto query = Call(server, "query_campaign"); Require(query["ok"].toBool(), "query active session");
    auto edit = Identity(query); edit["operation"] = "set_info"; edit["name"] = "Synthetic Campaign";
    Require(Call(server, "edit_campaign", edit)["ok"].toBool(), "edit through campaign command");
    Require(session->Info().name == "Synthetic Campaign", "host sees same session change");
    Require(!Call(server, "edit_campaign", edit)["ok"].toBool(), "stale revision rejected");
    Require(!Call(server, "create_campaign")["ok"].toBool(), "dirty campaign cannot silently close");
    query = Call(server, "query_campaign"); auto undo = Identity(query);
    Require(Call(server, "undo_campaign", undo)["ok"].toBool() && session->Info().name.empty(), "same history undo");
    query = Call(server, "query_campaign"); Require(Call(server, "redo_campaign", Identity(query))["ok"].toBool(), "same history redo");
    bool draft = true; int changed = 0; server.SetCampaignCallbacks([&] { return !draft; }, [&] { ++changed; });
    edit = Identity(Call(server, "query_campaign")); edit["operation"] = "set_info"; edit["author"] = "Synthetic Author";
    Require(!Call(server, "edit_campaign", edit)["ok"].toBool() && changed == 0, "GUI draft prevents MCP write");
    draft = false; Require(Call(server, "edit_campaign", edit)["ok"].toBool() && changed == 1, "GUI receives one shared publication");
    ToolPolicy policy; policy.allowEdit = false; policy.allowSave = false; server.SetToolPolicy(policy);
    Require(Call(server, "query_campaign")["ok"].toBool(), "readonly campaign query");
    edit = Identity(Call(server, "query_campaign")); edit["operation"] = "set_info"; edit["name"] = "Rejected";
    Require(!Call(server, "edit_campaign", edit)["ok"].toBool(), "edit denied by dispatcher");
    Require(!Call(server, "query_campaign", {{"limit", 257}})["ok"].toBool(), "oversize query refused");
    Require(!Call(server, "query_campaign", {{"offset", 0.5}})["ok"].toBool(), "fractional offset refused");
    Require(!Call(server, "query_campaign", {{"unknown", true}})["ok"].toBool(), "unknown field refused");
}

void SharedObjectEditingAndSave() {
    QTemporaryDir root; Require(root.isValid(), "temporary directory"); McpServer server; RegisterAllTools(server);
    Require(Call(server, "create_campaign")["ok"].toBool(), "campaign tools in public registry");
    auto session = server.Campaign();
    auto native = ObjectData::NativeObjectDocument::CreateEmpty(ObjectData::ObjectType::Unit);
    Require(native.value && native.value->AddCustomObject("H000", "hfoo").Succeeded(), "synthetic shared object");
    Require(native.value->AddModification({ObjectData::ObjectType::Unit, "H000"}, {"uhpm"}, ObjectData::NativeValueType::Int32, std::int32_t{100}).Succeeded(), "synthetic shared hp");
    const auto encoded = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Unit).Serialize(*native.value); Require(encoded.value.has_value(), "object encoded");
    Services::CampaignCommand shared; shared.operation = Services::CampaignOperation::ReplaceSharedData; shared.replacements = {{"war3campaign.w3u", *encoded.value}};
    Require(session->Apply(session->Revision(), shared).Succeeded(), "shared native staged");
    auto args = Identity(Call(server, "query_campaign")); args["object_type"] = "unit"; args["rawcode"] = "H000";
    const auto objects = Call(server, "query_campaign_objects", args); Require(objects["ok"].toBool() && objects["rows"].toArray().size() == 1, "bounded field discovery");
    args["operation"] = "set_object_field"; args["field_id"] = "uhpm"; args["value"] = 250;
    Require(Call(server, "edit_campaign", args)["ok"].toBool(), "exact native field edit");
    args = Identity(Call(server, "query_campaign")); args["operation"] = "set_object_field"; args["object_type"] = "unit"; args["rawcode"] = "H000"; args["field_id"] = "uhpm"; args["value"] = "250";
    Require(!Call(server, "edit_campaign", args)["ok"].toBool(), "wire type cannot be coerced");
    const auto target = root.filePath("candidate.w3n"); auto save = Identity(Call(server, "query_campaign")); save["in_place"] = false; save["output_path"] = target;
    const auto written = Call(server, "save_campaign", save); Require(written["ok"].toBool(), "shared save service");
    Require(!written["runtime_verified"].toBool() && !written["original_editor_verified"].toBool(), "no runtime claim");
    auto archive = MapArchiveSession::Open(std::filesystem::path(target.toStdWString())); Require(archive.value.has_value(), "saved candidate readback");
    Require(archive.value->HasMember("war3campaign.w3u") && !archive.value->HasMember("war3map.w3u"), "campaign object scope");
    const auto bytes = archive.value->ReadMember("war3campaign.w3u"); auto parsed = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Unit).Parse(*bytes.value);
    const auto* document = dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()); Require(document != nullptr, "saved native parsed");
    Require(std::get<std::int32_t>(document->Objects().front().modifications.front().value) == 250, "field persisted");
    save = Identity(Call(server, "query_campaign")); save["in_place"] = false; save["output_path"] = target;
    Require(!Call(server, "save_campaign", save)["ok"].toBool(), "existing target preserved");
}

void BoundedMenuPagination() {
    QTemporaryDir root; Require(root.isValid(), "temporary directory");
    const auto chapter = std::filesystem::path(root.filePath("chapter.w3x").toStdWString());
    const std::vector<MapArchiveMember> members{{"war3map.w3i", Bytes("synthetic container")}};
    Require(WriteVerifiedArchive(chapter, members).Succeeded(), "synthetic chapter");
    McpServer server; RegisterCampaignTools(server); auto session = Services::CampaignSession::Create(); server.SetCampaign(session);
    Services::CampaignCommand map; map.operation = Services::CampaignOperation::AddMap; map.path = "Chapter.w3x"; map.mapSource = chapter;
    Require(session->Apply(session->Revision(), map).Succeeded(), "map added");
    Services::CampaignCommand button; button.operation = Services::CampaignOperation::AddButton;
    button.button.mapPath = map.path; button.button.chapterTitle.assign(2048, '\\'); button.button.mapTitle.assign(2048, 'x');
    for (int i = 0; i < 200; ++i) Require(session->Apply(session->Revision(), button).Succeeded(), "button added");
    int offset = 0, count = 0;
    while (offset < 200) {
        const auto page = Call(server, "query_campaign", {{"offset", offset}, {"limit", 256}});
        Require(page["ok"].toBool(), "bounded query remains successful");
        const auto rows = page["buttons"].toArray(); Require(!rows.isEmpty(), "pagination advances");
        Require(rows.first().toObject()["text_truncated"].toBool(), "truncation explicit");
        count += static_cast<int>(rows.size()); const int next = page["next_offset"].toInt(); Require(next > offset, "cursor advances"); offset = next;
    }
    Require(count == 200, "all menu identities discoverable");
}
void InPlaceUsesHostBackupPreferences() {
    QTemporaryDir root; Require(root.isValid(), "temporary backup root");
    QSettings preferences(root.filePath("preferences.ini"), QSettings::IniFormat);
    const auto backupRoot = root.filePath("backups");
    preferences.setValue("mapSave/backupDirectory", backupRoot);
    preferences.setValue("mapSave/backupLimit", 1);
    preferences.sync(); Require(preferences.status() == QSettings::NoError, "temporary preferences saved");
    McpServer server; RegisterCampaignTools(server);
    Require(Call(server, "create_campaign")["ok"].toBool(), "new campaign for in-place save");
    auto save = Identity(Call(server, "query_campaign")); save["in_place"] = false; save["output_path"] = root.filePath("in-place.w3n");
    Require(Call(server, "save_campaign", save)["ok"].toBool(), "baseline candidate saved");
    const auto path = std::filesystem::path(root.filePath("in-place.w3n").toStdWString());
    const auto original = IO::ReadFileBytes(path, 1024u * 1024u); Require(original.value.has_value(), "original bytes");
    auto edit = Identity(Call(server, "query_campaign")); edit["operation"] = "set_info"; edit["description"] = "in-place edit";
    Require(Call(server, "edit_campaign", edit)["ok"].toBool(), "staged in-place edit");
    save = Identity(Call(server, "query_campaign")); save["in_place"] = true;
    Require(!Call(server, "save_campaign", save)["ok"].toBool(), "missing host backup preferences refuses in-place write");
    const auto unchanged = IO::ReadFileBytes(path, 1024u * 1024u);
    Require(unchanged.value && *unchanged.value == *original.value, "failed save leaves original unchanged");
    int preferenceReads = 0;
    server.SetCampaignBackupOptionsProvider([&] {
        ++preferenceReads;
        auto options = QtCommon::ReadMapInPlaceSaveOptions(preferences, root.path());
        Require(options.value && options.value->keepBackups == 1, "configured retention is shared");
        return options;
    });
    const auto written = Call(server, "save_campaign", save);
    Require(written["ok"].toBool() && preferenceReads == 1, "in-place save reads the injected preferences");
    const auto backup = std::filesystem::path(written["backup"].toString().toStdWString());
    Require(!backup.empty() && backup.parent_path() == std::filesystem::path(backupRoot.toStdWString()), "configured backup directory used");
    const auto backedUp = IO::ReadFileBytes(backup, 1024u * 1024u);
    Require(backedUp.value && *backedUp.value == *original.value, "backup contains pre-save bytes");
    const auto current = IO::ReadFileBytes(path, 1024u * 1024u);
    Require(current.value && *current.value != *original.value, "in-place edit persisted");
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try { SharedSessionRevisionAndPolicy(); SharedObjectEditingAndSave(); BoundedMenuPagination(); InPlaceUsesHostBackupPreferences();
        std::cout << "PASS campaign shared MCP session, policy, revision, objects, save and bounded paging\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
