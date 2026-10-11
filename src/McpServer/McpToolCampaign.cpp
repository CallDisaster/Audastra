#include "McpToolInternal.h"
#include "McpCampaignTools.h"
#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include <QJsonDocument>
#include <algorithm>
#include <bit>
#include <limits>

#include <charconv>
#include <cmath>

namespace War3Map::Mcp {
Result<InPlaceSaveOptions> McpServer::CampaignBackupOptions() const {
    if (campaignBackupOptions_) return campaignBackupOptions_();
    return Result<InPlaceSaveOptions>::Failure("Campaign.Mcp", "CAMPAIGN_BACKUP_PREFERENCES_UNAVAILABLE",
        "宿主尚未提供战役保存的备份偏好，本次未写回原文件。");
}
namespace {
QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::string Utf8(const QJsonValue& value) { return value.toString().toUtf8().toStdString(); }
QJsonObject Identity(const Services::CampaignSession& session) {
    const auto revision = session.Revision();
    return {{"document", QString::number(revision.document)}, {"revision", QString::number(revision.revision)}};
}
std::optional<std::uint64_t> Number(const QJsonValue& value) {
    if (!value.isString()) return {};
    const auto text = value.toString().toLatin1(); std::uint64_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.isEmpty() || result.ec != std::errc{} || result.ptr != text.data() + text.size()) return {};
    return number;
}
std::optional<Services::CampaignRevision> Revision(const QJsonObject& params) {
    const auto document = Number(params["document"]), revision = Number(params["revision"]);
    if (!document || !revision) return {};
    return Services::CampaignRevision{*document, *revision};
}
QJsonValue Error() { return Failure("CAMPAIGN_CONTEXT_REQUIRED", "需要活动战役，以及查询回执中的 document/revision 字符串。"); }
QJsonObject Success(const Services::CampaignSession& session) {
    auto result = Identity(session); result["ok"] = true; result["dirty"] = session.Dirty(); return result;
}
std::optional<ObjectData::ObjectType> ObjectType(const QJsonValue& value) {
    if (!value.isString()) return {};
    for (int i = 0; i < 7; ++i) {
        const auto type = static_cast<ObjectData::ObjectType>(i);
        if (Text(ObjectData::ToString(type)) == value.toString()) return type;
    }
    return {};
}
Result<ObjectData::NativeObjectDocument> Objects(const Services::CampaignSession& session, ObjectData::ObjectType type) {
    const auto bytes = session.ReadMember(session.Revision(), Services::CampaignSession::ObjectMember(type));
    if (!bytes.Succeeded() || !bytes.value) return {std::nullopt, bytes.diagnostics};
    auto parsed = ObjectData::NativeObjectCodec(type).Parse(*bytes.value);
    const auto* document = dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get());
    if (!parsed.Succeeded() || !document) return {std::nullopt, parsed.diagnostics};
    return Result<ObjectData::NativeObjectDocument>::Success(*document);
}
QJsonObject SetObjectField(const Services::CampaignSession& session, const QJsonObject& p, Services::CampaignCommand& command) {
    const auto type = ObjectType(p["object_type"]);
    if (!type || !p["rawcode"].isString() || !p["field_id"].isString()) return Failure("CAMPAIGN_OBJECT_ID", "需要物编类别、rawcode 与 field_id。");
    auto parsed = Objects(session, *type);
    if (!parsed.Succeeded() || !parsed.value) return Failure(parsed);
    ObjectData::ObjectKey object{*type, Utf8(p["rawcode"])};
    ObjectData::ModificationKey field{Utf8(p["field_id"])};
    for (const auto pair : {std::pair{"level", &field.level}, {"data_index", &field.dataIndex}, {"variation", &field.variation}}) {
        if (!p.contains(pair.first)) continue;
        const auto value = p[pair.first];
        if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 2147483647 || value.toDouble() != std::floor(value.toDouble()))
            return Failure("CAMPAIGN_OBJECT_FIELD_ID", "字段等级、数据、变体必须是非负整数。");
        *pair.second = static_cast<std::uint32_t>(value.toDouble());
    }
    const auto wire = parsed.value->ModificationValueType(object, field);
    if (!wire) return Failure("CAMPAIGN_OBJECT_FIELD_MISSING", "原生物编中没有此精确字段，不能猜测类型或默认值。");
    ObjectData::Value value;
    if (*wire == ObjectData::NativeValueType::String) {
        if (!p["value"].isString() || p["value"].toString().size() > 65536) return Failure("CAMPAIGN_OBJECT_VALUE", "字符串字段最多 65536 字符。");
        value = Utf8(p["value"]);
    } else {
        if (!p["value"].isDouble() || !std::isfinite(p["value"].toDouble())) return Failure("CAMPAIGN_OBJECT_VALUE", "字段需要有限数值。");
        const double number = p["value"].toDouble();
        if (*wire == ObjectData::NativeValueType::Int32) {
            if (number < -2147483648.0 || number > 2147483647.0 || number != std::floor(number)) return Failure("CAMPAIGN_OBJECT_VALUE", "字段需要 32 位整数。");
            value = static_cast<std::int32_t>(number);
        } else {
            if (std::abs(number) > (std::numeric_limits<float>::max)()) return Failure("CAMPAIGN_OBJECT_VALUE", "字段超出原生实数范围。");
            value = static_cast<float>(number);
        }
    }
    const auto changed = parsed.value->SetModification(object, field, std::move(value));
    if (!changed.Succeeded()) return Failure(changed);
    auto bytes = ObjectData::NativeObjectCodec(*type).Serialize(*parsed.value);
    if (!bytes.Succeeded() || !bytes.value) return Failure(bytes);
    command.operation = Services::CampaignOperation::ReplaceSharedData;
    command.replacements.push_back({Services::CampaignSession::ObjectMember(*type), std::move(*bytes.value)});
    return {{"ok", true}};
}
} // namespace

std::vector<ToolDefinition> CampaignToolDefinitions(McpServer& server) {
    std::vector<ToolDefinition> definitions;
    const QJsonObject text{{"type", "string"}}, integer{{"type", "integer"}, {"minimum", 0}};
    const auto add = [&](const char* name, const char* description, ToolPermission permission,
        QJsonObject schema, ToolHandler handler) {
        ToolDefinition tool; tool.name = name; tool.description = QString::fromUtf8(description);
        tool.permission = permission; tool.inputSchema = std::move(schema);
        tool.handler = [&server, handler = std::move(handler), permission](const QJsonObject& params) -> QJsonValue {
            if (permission != ToolPermission::Query && !server.PrepareCampaignWrite())
                return Failure("CAMPAIGN_GUI_DRAFT", "界面仍有共享数据、设置或章节草稿，请先在界面提交；当前战役未修改。");
            auto result = handler(params);
            if (permission != ToolPermission::Query && result.toObject()["ok"].toBool()) server.NotifyCampaignChanged();
            return result;
        };
        tool.maxResponseBytes = 256 * 1024; tool.maxRequestIdBytes = 256;
        definitions.push_back(std::move(tool));
    };
    add("open_campaign", "只读打开经典 .w3n 战役。已有未保存战役时拒绝替换；不修改地图或游戏安装。", ToolPermission::Edit,
        BasicSchema({{"path", text}}, {"path"}), [&server](const QJsonObject& p) -> QJsonValue {
            if (server.Campaign() && server.Campaign()->Dirty()) return Failure("CAMPAIGN_DIRTY", "请先保存或明确关闭当前战役。");
            const auto path = ParseAbsolutePath(p, "path"); if (!path.Succeeded()) return Failure(path);
            auto opened = Services::CampaignSession::Open(*path.value);
            if (!opened.Succeeded() || !opened.value) return Failure(opened);
            server.SetCampaign(*opened.value); return Success(*server.Campaign());
        });
    add("create_campaign", "建立空的经典战役会话；保存须另选不存在的 .w3n 路径。", ToolPermission::Edit,
        BasicSchema({}, {}), [&server](const QJsonObject&) -> QJsonValue {
            if (server.Campaign() && server.Campaign()->Dirty()) return Failure("CAMPAIGN_DIRTY", "当前战役有未保存修改。");
            server.SetCampaign(Services::CampaignSession::Create()); return Success(*server.Campaign());
        });
    add("query_campaign", "有界查询战役信息、独立地图身份、菜单按钮身份和成员。offset/limit 同时约束三个列表，最多 256。", ToolPermission::Query,
        BasicSchema({{"offset", integer}, {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 256}}}}, {}),
        [&server](const QJsonObject& p) -> QJsonValue {
            const auto& session = server.Campaign(); if (!session) return Error();
            for (const auto* key : {"offset", "limit"}) if (p.contains(key)
                && (!p[key].isDouble() || p[key].toDouble() != std::floor(p[key].toDouble()) || p[key].toDouble() > 2147483000))
                return Failure("CAMPAIGN_QUERY_LIMIT", "offset/limit 必须是范围内的整数。");
            const auto offset = p.value("offset").toInt(0), limit = p.value("limit").toInt(64);
            if (offset < 0 || limit < 1 || limit > 256) return Failure("CAMPAIGN_QUERY_LIMIT", "offset/limit 无效。");
            const auto page = session->Members(session->Revision(), static_cast<std::size_t>(offset), static_cast<std::size_t>(limit));
            if (!page.Succeeded() || !page.value) return Failure(page);
            auto result = Success(*session); const auto& info = session->Info();
            // 设置字段单项限制回应正文；原文仍完整保存在服务中，截断有明确标记。
            const auto label = [](const std::string& value) { return Text(value).left(512); };
            result["info"] = QJsonObject{{"name", label(info.name)}, {"difficulty", label(info.difficulty)},
                {"author", label(info.author)}, {"description", label(info.description)},
                {"text_truncated", Text(info.name).size() > 512 || Text(info.difficulty).size() > 512 || Text(info.author).size() > 512 || Text(info.description).size() > 512},
                {"format_version", info.formatVersion}, {"editor_version", info.editorVersion}, {"flags", static_cast<double>(info.flags)},
                {"background_id", info.background}, {"background_path", label(info.backgroundPath)},
                {"minimap_path", label(info.minimapPath)}, {"ambient_id", info.ambientSound}, {"ambient_path", label(info.ambientSoundPath)},
                {"fog_style", info.fogStyle}, {"cursor_id", info.cursor},
                {"fog_bits", QJsonArray{static_cast<double>(info.fogBits[0]), static_cast<double>(info.fogBits[1]), static_cast<double>(info.fogBits[2])}},
                {"fog_color", QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(info.fogColor.data()), 4).toHex())}};
            QJsonArray maps, buttons, members;
            std::size_t rowCount = 0, payloadBytes = 0;
            const auto total = (std::max)({info.maps.size(), info.buttons.size(), page.value->total});
            for (std::size_t i = static_cast<std::size_t>(offset); i < total && rowCount < static_cast<std::size_t>(limit); ++i) {
                QJsonObject map, button, member;
                if (i < info.maps.size()) map = {{"id", QString::number(info.maps[i].id)}, {"path", label(info.maps[i].path)},
                    {"text_truncated", Text(info.maps[i].path).size() > 512}};
                if (i < info.buttons.size()) {
                    const auto& item = info.buttons[i];
                    button = {{"id", QString::number(item.id)}, {"chapter_title", label(item.chapterTitle)},
                        {"map_title", label(item.mapTitle)}, {"map_path", label(item.mapPath)}, {"visible", item.visible != 0},
                        {"text_truncated", Text(item.chapterTitle).size() > 512 || Text(item.mapTitle).size() > 512 || Text(item.mapPath).size() > 512}};
                }
                if (rowCount < page.value->members.size()) {
                    const auto& item = page.value->members[rowCount];
                    member = {{"path", label(item.path)}, {"bytes", QString::number(item.size)}, {"changed", item.changed},
                        {"text_truncated", Text(item.path).size() > 512}};
                }
                const auto size = QJsonDocument(QJsonObject{{"map", map}, {"button", button}, {"member", member}}).toJson(QJsonDocument::Compact).size();
                if (payloadBytes + static_cast<std::size_t>(size) > 64u * 1024u) break;
                payloadBytes += static_cast<std::size_t>(size); ++rowCount;
                if (!map.isEmpty()) maps.append(map); if (!button.isEmpty()) buttons.append(button); if (!member.isEmpty()) members.append(member);
            }
            result["maps"] = maps; result["buttons"] = buttons; result["members"] = members;
            result["map_count"] = static_cast<double>(info.maps.size()); result["button_count"] = static_cast<double>(info.buttons.size());
            result["member_count"] = static_cast<double>(page.value->total); result["next_offset"] = static_cast<double>((std::min)(static_cast<std::size_t>(offset) + rowCount, total));
            result["eof"] = static_cast<std::size_t>(offset) + rowCount >= total;
            result["can_undo"] = session->CanUndo(); result["can_redo"] = session->CanRedo();
            return result;
        });
    add("query_campaign_objects", "查询战役共享物编的对象清单；指定 rawcode 后分页读取精确字段身份与值，每页最多 64。", ToolPermission::Query,
        BasicSchema({{"document", text}, {"revision", text}, {"object_type", text}, {"rawcode", text}, {"offset", integer},
            {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}}}, {"document", "revision", "object_type"}),
        [&server](const QJsonObject& p) -> QJsonValue {
            const auto& session = server.Campaign(); const auto revision = Revision(p); const auto type = ObjectType(p["object_type"]);
            if (!session || !revision || !type) return Error();
            const auto check = session->Members(*revision, 0, 1); if (!check.Succeeded()) return Failure(check);
            for (const auto* key : {"offset", "limit"}) if (p.contains(key) && (!p[key].isDouble() || p[key].toDouble() < 0
                || p[key].toDouble() > 2147483000 || p[key].toDouble() != std::floor(p[key].toDouble()))) return Failure("CAMPAIGN_QUERY_LIMIT", "分页参数无效。");
            const auto offset = p.value("offset").toInt(0), limit = p.value("limit").toInt(64);
            if (limit < 1 || limit > 64) return Failure("CAMPAIGN_QUERY_LIMIT", "limit 必须在 1 至 64 之间。");
            const auto parsed = Objects(*session, *type); if (!parsed.Succeeded() || !parsed.value) return Failure(parsed);
            const auto& document = *parsed.value; QJsonArray rows; std::size_t total = 0, bytes = 0;
            if (p.contains("rawcode")) {
                const auto* object = document.FindObject({*type, Utf8(p["rawcode"])});
                if (!object) return Failure("CAMPAIGN_OBJECT_MISSING", "共享物编中没有此对象。");
                total = object->modifications.size();
                for (std::size_t i = static_cast<std::size_t>(offset); i < total && rows.size() < limit; ++i) {
                    const auto& modification = object->modifications[i]; const auto& key = modification.key;
                    const auto value = Text(ObjectData::ValueToDisplayString(modification.value));
                    QJsonObject row{{"field_id", Text(key.fieldId)}, {"value", value.left(4096)}, {"text_truncated", value.size() > 4096},
                        {"value_kind", Text(ObjectData::ValueKindName(modification.value))},
                        {"level", key.level ? QJsonValue(static_cast<double>(*key.level)) : QJsonValue{}},
                        {"data_index", key.dataIndex ? QJsonValue(static_cast<double>(*key.dataIndex)) : QJsonValue{}},
                        {"variation", key.variation ? QJsonValue(static_cast<double>(*key.variation)) : QJsonValue{}}};
                    const auto size = static_cast<std::size_t>(QJsonDocument(row).toJson(QJsonDocument::Compact).size());
                    if (bytes + size > 96u * 1024u) break;
                    bytes += size; rows.append(row);
                }
            } else {
                total = document.Objects().size();
                for (std::size_t i = static_cast<std::size_t>(offset); i < total && rows.size() < limit; ++i) {
                    const auto& object = document.Objects()[i];
                    rows.append(QJsonObject{{"rawcode", Text(object.key.rawcode)}, {"parent", Text(object.parentRawcode.value_or(""))},
                        {"field_count", static_cast<double>(object.modifications.size())}});
                }
            }
            auto result = Success(*session); result["rows"] = rows; result["total"] = static_cast<double>(total);
            result["next_offset"] = static_cast<double>((std::min)(static_cast<std::size_t>(offset) + static_cast<std::size_t>(rows.size()), total));
            result["eof"] = static_cast<std::size_t>(offset) + static_cast<std::size_t>(rows.size()) >= total;
            return result;
        });
    add("edit_campaign", "通过同一事务编辑战役设置、章节、菜单、资源及共享物编。set_object_field 修改精确既有字段；replace_shared_objects 导入一类原生物编。旧修订拒绝。", ToolPermission::Edit,
        BasicSchema({{"document", text}, {"revision", text}, {"operation", text}, {"id", text}, {"position", integer},
            {"path", text}, {"source_path", text}, {"name", text}, {"difficulty", text}, {"author", text}, {"description", text},
            {"chapter_title", text}, {"map_title", text}, {"visible", QJsonObject{{"type", "boolean"}}},
            {"background_id", QJsonObject{{"type", "integer"}}}, {"background_path", text}, {"minimap_path", text},
            {"ambient_id", QJsonObject{{"type", "integer"}}}, {"ambient_path", text}, {"fog_style", QJsonObject{{"type", "integer"}}},
            {"cursor_id", QJsonObject{{"type", "integer"}}}, {"editor_version", QJsonObject{{"type", "integer"}}}, {"flags", integer},
            {"fog_bits", QJsonObject{{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", integer}}}, {"fog_color", text},
            {"object_type", text}, {"rawcode", text}, {"field_id", text}, {"level", integer}, {"data_index", integer}, {"variation", integer}, {"value", QJsonObject{}}},
            {"document", "revision", "operation"}),
        [&server](const QJsonObject& p) -> QJsonValue {
            const auto& session = server.Campaign(); const auto revision = Revision(p); if (!session || !revision) return Error();
            Services::CampaignCommand command; const auto operation = p["operation"].toString();
            command.id = Number(p["id"]).value_or(0); command.path = Utf8(p["path"]);
            if (p.contains("id") && !Number(p["id"])) return Failure("CAMPAIGN_ID_INVALID", "id 必须是十进制字符串。");
            if (p.contains("position") && (!p["position"].isDouble() || p["position"].toDouble(-1) < 0 || p["position"].toDouble() > 4096 || std::floor(p["position"].toDouble()) != p["position"].toDouble()))
                return Failure("CAMPAIGN_POSITION_INVALID", "position 必须是有效整数。");
            command.position = static_cast<std::size_t>(p["position"].toInt());
            if (operation == "set_info") {
                command.operation = Services::CampaignOperation::SetInfo; command.info = session->Info();
                for (auto pair : {std::pair{"name", &command.info.name}, {"difficulty", &command.info.difficulty}, {"author", &command.info.author}, {"description", &command.info.description},
                    {"background_path", &command.info.backgroundPath}, {"minimap_path", &command.info.minimapPath}, {"ambient_path", &command.info.ambientSoundPath}}) {
                    if (!p.contains(pair.first)) continue;
                    if (!p[pair.first].isString() || p[pair.first].toString().size() > 65536) return Failure("CAMPAIGN_TEXT_INVALID", "设置文本必须是最多 65536 字符的字符串。");
                    *pair.second = Utf8(p[pair.first]);
                }
                for (auto pair : {std::pair{"background_id", &command.info.background}, {"ambient_id", &command.info.ambientSound},
                    {"fog_style", &command.info.fogStyle}, {"cursor_id", &command.info.cursor}, {"editor_version", &command.info.editorVersion}}) {
                    if (!p.contains(pair.first)) continue;
                    const auto value = p[pair.first];
                    if (!value.isDouble() || value.toDouble() < -2147483648.0 || value.toDouble() > 2147483647.0 || value.toDouble() != std::floor(value.toDouble()))
                        return Failure("CAMPAIGN_SETTING_INTEGER", "设置字段需要 32 位整数。");
                    *pair.second = static_cast<std::int32_t>(value.toDouble());
                }
                if (p.contains("flags")) {
                    const auto value = p["flags"];
                    if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 4294967295.0 || value.toDouble() != std::floor(value.toDouble()))
                        return Failure("CAMPAIGN_SETTING_FLAGS", "flags 需要无符号 32 位整数。");
                    command.info.flags = static_cast<std::uint32_t>(value.toDouble());
                }
                if (p.contains("fog_bits")) {
                    const auto values = p["fog_bits"].toArray();
                    if (!p["fog_bits"].isArray() || values.size() != 3) return Failure("CAMPAIGN_FOG_BITS", "fog_bits 需要三个无符号 32 位整数。");
                    for (int i = 0; i < 3; ++i) {
                        const auto value = values[i];
                        if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 4294967295.0 || value.toDouble() != std::floor(value.toDouble()))
                            return Failure("CAMPAIGN_FOG_BITS", "fog_bits 需要三个无符号 32 位整数。");
                        command.info.fogBits[i] = static_cast<std::uint32_t>(value.toDouble());
                    }
                }
                if (p.contains("fog_color")) {
                    const auto value = p["fog_color"].toString().toLatin1();
                    if (!p["fog_color"].isString() || value.size() != 8 || !std::all_of(value.begin(), value.end(), [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); })) return Failure("CAMPAIGN_FOG_COLOR", "fog_color 需要八个十六进制字符。");
                    const auto bytes = QByteArray::fromHex(value);
                    for (int i = 0; i < 4; ++i) command.info.fogColor[i] = static_cast<std::uint8_t>(bytes[i]);
                }
            } else if (operation == "set_object_field") {
                const auto result = SetObjectField(*session, p, command); if (!result["ok"].toBool()) return result;
            } else if (operation == "replace_shared_objects") {
                const auto type = ObjectType(p["object_type"]); if (!type) return Failure("CAMPAIGN_OBJECT_TYPE", "未知共享物编类别。");
                const auto path = ParseAbsolutePath(p, "source_path"); if (!path.Succeeded()) return Failure(path);
                const auto bytes = IO::ReadFileBytes(*path.value, 64u * 1024u * 1024u); if (!bytes.Succeeded() || !bytes.value) return Failure(bytes);
                command.operation = Services::CampaignOperation::ReplaceSharedData;
                command.replacements.push_back({Services::CampaignSession::ObjectMember(*type), *bytes.value});
            } else if (operation == "add_map" || operation == "replace_map") {
                command.operation = operation == "add_map" ? Services::CampaignOperation::AddMap : Services::CampaignOperation::ReplaceMap;
                const auto path = ParseAbsolutePath(p, "source_path"); if (!path.Succeeded()) return Failure(path);
                command.mapSource = *path.value;
            } else if (operation == "remove_map") command.operation = Services::CampaignOperation::RemoveMap;
            else if (operation == "move_map") command.operation = Services::CampaignOperation::MoveMap;
            else if (operation == "add_button" || operation == "update_button") {
                if (!p["visible"].isBool() || !p["chapter_title"].isString() || !p["map_title"].isString() || !p["path"].isString())
                    return Failure("CAMPAIGN_BUTTON_INVALID", "菜单需要 chapter_title、map_title、path 字符串及 visible 布尔值。");
                command.operation = operation == "add_button" ? Services::CampaignOperation::AddButton : Services::CampaignOperation::UpdateButton;
                command.button = {command.id, p["visible"].toBool() ? 1 : 0, Utf8(p["chapter_title"]), Utf8(p["map_title"]), command.path};
            } else if (operation == "remove_button") command.operation = Services::CampaignOperation::RemoveButton;
            else if (operation == "move_button") command.operation = Services::CampaignOperation::MoveButton;
            else if (operation == "import_resource") {
                command.operation = Services::CampaignOperation::ImportResource;
                const auto path = ParseAbsolutePath(p, "source_path"); if (!path.Succeeded()) return Failure(path);
                const auto bytes = IO::ReadFileBytes(*path.value, 64u * 1024u * 1024u); if (!bytes.Succeeded() || !bytes.value) return Failure(bytes);
                command.replacements.push_back({command.path, *bytes.value});
            } else if (operation == "remove_resource") {
                command.operation = Services::CampaignOperation::RemoveResource; command.removals.push_back(command.path);
            } else return Failure("CAMPAIGN_OPERATION_UNSUPPORTED", "不支持的战役操作。");
            const auto result = session->Apply(*revision, command); if (!result.Succeeded() || !result.value) return Failure(result);
            auto response = Success(*session); response["id"] = QString::number(*result.value); return response;
        });
    for (bool redo : {false, true}) add(redo ? "redo_campaign" : "undo_campaign", "撤销或重做一次战役事务，修订必须仍然匹配。", ToolPermission::Edit,
        BasicSchema({{"document", text}, {"revision", text}}, {"document", "revision"}), [&server, redo](const QJsonObject& p) -> QJsonValue {
            const auto& session = server.Campaign(); const auto revision = Revision(p); if (!session || !revision) return Error();
            const auto result = redo ? session->Redo(*revision) : session->Undo(*revision);
            return result.Succeeded() ? QJsonValue{Success(*session)} : Failure(result);
        });
    add("save_campaign", "保存战役候选到不存在的 .w3n；in_place=true 时经原子保存及备份写回。原版 WE/游戏验收单独确认。", ToolPermission::Save,
        BasicSchema({{"document", text}, {"revision", text}, {"output_path", text}, {"in_place", QJsonObject{{"type", "boolean"}}}}, {"document", "revision", "in_place"}),
        [&server](const QJsonObject& p) -> QJsonValue {
            const auto& session = server.Campaign(); const auto revision = Revision(p); if (!session || !revision || !p["in_place"].isBool()) return Error();
            std::filesystem::path target;
            if (!p["in_place"].toBool()) { const auto path = ParseAbsolutePath(p, "output_path"); if (!path.Succeeded()) return Failure(path); target = *path.value; }
            auto backup = Result<InPlaceSaveOptions>::Success({});
            if (p["in_place"].toBool()) backup = server.CampaignBackupOptions();
            if (!backup.Succeeded() || !backup.value) return Failure(backup);
            const auto saved = session->Save(*revision, target, p["in_place"].toBool(), *backup.value);
            if (!saved.Succeeded() || !saved.value) return Failure(saved);
            auto result = Success(*session); result["path"] = QString::fromStdWString(saved.value->written.path.wstring());
            result["sha256"] = Text(saved.value->written.sha256); result["backup"] = QString::fromStdWString(saved.value->backup.wstring());
            result["original_editor_verified"] = false; result["runtime_verified"] = false; return result;
        });
    add("export_campaign_member", "将一个战役成员导出到不存在的文件；不覆盖原文件。", ToolPermission::Save,
        BasicSchema({{"document", text}, {"revision", text}, {"path", text}, {"output_path", text}}, {"document", "revision", "path", "output_path"}),
        [&server](const QJsonObject& p) -> QJsonValue {
            const auto& session = server.Campaign(); const auto revision = Revision(p); if (!session || !revision) return Error();
            const auto path = ParseAbsolutePath(p, "output_path"); if (!path.Succeeded()) return Failure(path);
            const auto result = session->ExportMember(*revision, Utf8(p["path"]), *path.value);
            return result.Succeeded() ? QJsonValue{Success(*session)} : Failure(result);
        });
    return definitions;
}
void RegisterCampaignTools(McpServer& server) {
    for (auto definition : CampaignToolDefinitions(server)) server.RegisterTool(std::move(definition));
}
} // namespace War3Map::Mcp
