#include "McpAuxiliaryTools.h"
#include "McpToolInternal.h"

#include <QJsonDocument>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>

namespace War3Map::Mcp {
namespace {
using namespace Services;
using namespace MapData;
struct Invalid { const char* code; std::string message; };
void Keys(const QJsonObject& object, const QJsonObject& properties, const QJsonArray& required) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!properties.contains(it.key())) throw Invalid{"AUX_ARGUMENT_UNKNOWN", "未知字段：" + it.key().toStdString()};
    for (const auto& field : required)
        if (!object.contains(field.toString())) throw Invalid{"AUX_ARGUMENT_MISSING", "缺少字段：" + field.toString().toStdString()};
}
std::string Text(const QJsonObject& object, const char* key, std::size_t max = 4096) {
    const auto value = object.value(key);
    if (!value.isString()) throw Invalid{"AUX_ARGUMENT_TYPE", std::string(key) + " 须为字符串。"};
    const auto utf8 = value.toString().toUtf8();
    if (static_cast<std::size_t>(utf8.size()) > max || QString::fromUtf8(utf8) != value.toString() || utf8.contains('\0'))
        throw Invalid{"AUX_ARGUMENT_TEXT", std::string(key) + " 超过预算、含空字节或不能无损编码为 UTF-8。"};
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}
float Number(const QJsonObject& object, const char* key) {
    const auto value = object.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 1e6)
        throw Invalid{"AUX_ARGUMENT_NUMBER", std::string(key) + " 须为 ±1000000 内有限数值。"};
    return static_cast<float>(value.toDouble());
}
std::int32_t Integer(const QJsonObject& object, const char* key) {
    const auto value = object.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() != std::floor(value.toDouble())
        || value.toDouble() < 0 || value.toDouble() > (std::numeric_limits<std::int32_t>::max)())
        throw Invalid{"AUX_ARGUMENT_INTEGER", std::string(key) + " 须为非负 int32 整数。"};
    return static_cast<std::int32_t>(value.toDouble());
}
QJsonObject StringSchema(std::size_t max = 4096) { return {{"type", "string"}, {"maxLength", static_cast<double>(max)}}; }
QJsonObject FieldsSchema(bool region, bool create) {
    const QJsonObject number{{"type", "number"}, {"minimum", -1000000}, {"maximum", 1000000}};
    QJsonObject properties{{"name", StringSchema()}};
    QJsonArray required;
    if (create) required.append("name");
    if (region) {
        for (const auto* key : {"left", "bottom", "right", "top"}) { properties.insert(key, number); if (create) required.append(key); }
        properties.insert("creation_number", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 2147483647}});
        properties.insert("weather_id", StringSchema(4)); properties.insert("ambient_sound", StringSchema());
        properties.insert("color_bgr", QJsonObject{{"type", "array"}, {"minItems", 3}, {"maxItems", 3},
            {"items", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}}});
        if (create) for (const auto* key : {"creation_number", "weather_id", "ambient_sound", "color_bgr"}) required.append(key);
    } else {
        for (const auto* key : {"target_x", "target_y", "z_offset", "rotation", "angle_of_attack", "target_distance", "roll", "field_of_view", "far_clipping_plane"}) {
            properties.insert(key, number); if (create) required.append(key);
        }
    }
    return BasicSchema(properties, required);
}
void SetFields(AuxiliaryEditRequest& request, const QJsonObject& fields, bool create) {
    const bool region = request.kind == MapDataKind::Regions;
    const auto schema = FieldsSchema(region, create);
    Keys(fields, schema.value("properties").toObject(), schema.value("required").toArray());
    if (fields.isEmpty()) throw Invalid{"AUX_ARGUMENT_EMPTY", "fields 不能是空对象。"};
    if (region) {
        auto& record = std::get<RegionRecord>(request.record);
        if (fields.contains("name")) record.name = Text(fields, "name");
        const std::array coordinates{std::pair{"left", &RegionRecord::left}, std::pair{"bottom", &RegionRecord::bottom},
            std::pair{"right", &RegionRecord::right}, std::pair{"top", &RegionRecord::top}};
        for (const auto& [key, member] : coordinates) if (fields.contains(key)) record.*member = Number(fields, key);
        if (fields.contains("creation_number")) record.creationNumber = Integer(fields, "creation_number");
        if (fields.contains("weather_id")) {
            const auto weather = Text(fields, "weather_id", 4);
            if (!weather.empty() && weather.size() != 4) throw Invalid{"AUX_ARGUMENT_TEXT", "weather_id 须为空或四字节 rawcode。"};
            record.weatherId = {};
            for (std::size_t i = 0; i < weather.size(); ++i) record.weatherId[i] = static_cast<std::byte>(weather[i]);
        }
        if (fields.contains("ambient_sound")) record.ambientSound = Text(fields, "ambient_sound");
        if (fields.contains("color_bgr")) {
            const auto value = fields.value("color_bgr");
            if (!value.isArray() || value.toArray().size() != 3) throw Invalid{"AUX_ARGUMENT_TYPE", "color_bgr 须为 B/G/R 三个字节。"};
            for (qsizetype i = 0; i < 3; ++i) {
                const auto byte = value.toArray()[i];
                if (!byte.isDouble() || byte.toDouble() < 0 || byte.toDouble() > 255 || byte.toDouble() != std::floor(byte.toDouble()))
                    throw Invalid{"AUX_ARGUMENT_INTEGER", "color_bgr 每项须为 0..255 整数。"};
                record.colorBgra[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(byte.toInt());
            }
        }
    } else {
        auto& record = std::get<CameraRecord>(request.record);
        if (fields.contains("name")) record.name = Text(fields, "name");
        const std::array numbers{std::pair{"target_x", &CameraRecord::targetX}, std::pair{"target_y", &CameraRecord::targetY},
            std::pair{"z_offset", &CameraRecord::zOffset}, std::pair{"rotation", &CameraRecord::rotation},
            std::pair{"angle_of_attack", &CameraRecord::angleOfAttack}, std::pair{"target_distance", &CameraRecord::targetDistance},
            std::pair{"roll", &CameraRecord::roll}, std::pair{"field_of_view", &CameraRecord::fieldOfView},
            std::pair{"far_clipping_plane", &CameraRecord::farClippingPlane}};
        for (const auto& [key, member] : numbers) if (fields.contains(key)) record.*member = Number(fields, key);
    }
}
}

std::vector<ToolDefinition> AuxiliaryToolDefinitions() {
    QJsonObject common{{"project_identity", StringSchema()}, {"expected_revision", StringSchema()},
        {"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"regions", "cameras"}}}},
        {"expected_member_sha256", QJsonObject{{"type", "string"}, {"pattern", "^([0-9a-f]{64}|missing)$"}, {"maxLength", 64}}},
        {"dry_run", QJsonObject{{"type", "boolean"}}}};
    const auto source = BasicSchema({{"source_index", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}},
        {"row_key", StringSchema(16384)}}, {"source_index", "row_key"});
    std::vector<ToolDefinition> tools;
    for (const auto* action : {"create", "update", "delete", "copy"}) {
        auto properties = common;
        QJsonArray required{"project_identity", "expected_revision", "kind", "expected_member_sha256"};
        const std::string operation(action);
        if (operation != "create") { properties.insert("source", source); required.append("source"); }
        if (operation != "delete") {
            // kind 决定实际 schema；运行时对字段、完整 create 记录和预算再严格核对。
            auto fields = FieldsSchema(true, false).value("properties").toObject();
            const auto camera = FieldsSchema(false, false).value("properties").toObject();
            for (auto it = camera.begin(); it != camera.end(); ++it) fields.insert(it.key(), it.value());
            properties.insert("fields", BasicSchema(fields)); required.append("fields");
        }
        ToolDefinition tool; tool.name = QString::fromStdString("auxiliary_" + operation);
        tool.description = QStringLiteral("区域/镜头结构命令：%1。读取用 entities_query；字段名按工具 schema，角度为度。共享 UI 的布局、编号、引用与符号检查，原字段未提供则逐位保留。create 提供完整已知字段；copy 提供新 name，区域还需新 creation_number。dry_run 只返回候选；实际命令进入原事务，不保存。").arg(QString::fromUtf8(action));
        tool.permission = ToolPermission::Edit; tool.inputSchema = BasicSchema(properties, required);
        tool.maxRequestIdBytes = 4096; tool.maxResponseBytes = 1024U * 1024U;
        tools.push_back(std::move(tool));
    }
    return tools;
}

QJsonObject InvokeAuxiliaryTool(const QString& name, const QJsonObject& arguments, const AuxiliaryToolHost& host) {
    try {
        if (!host.identity || !host.context || !host.references) return Failure("AUX_HOST_UNAVAILABLE", "区域/镜头宿主未接通。");
        const auto definitions = AuxiliaryToolDefinitions();
        const auto tool = std::find_if(definitions.begin(), definitions.end(), [&](const auto& value) { return value.name == name; });
        if (tool == definitions.end()) return Failure("AUX_TOOL_UNKNOWN", "未知区域/镜头工具。");
        if (QJsonDocument(arguments).toJson(QJsonDocument::Compact).size() > 1024 * 1024)
            return Failure("AUX_ARGUMENT_BUDGET", "区域/镜头请求超过 1 MiB。");
        Keys(arguments, tool->inputSchema.value("properties").toObject(), tool->inputSchema.value("required").toArray());
        if (arguments.contains("dry_run") && !arguments.value("dry_run").isBool()) return Failure("AUX_ARGUMENT_TYPE", "dry_run 须为布尔值。");
        const bool dry = arguments.value("dry_run").toBool(false);
        const auto identity = host.identity();
        if (QString::fromStdString(Text(arguments, "project_identity")) != identity.value("project_identity")
            || QString::fromStdString(Text(arguments, "expected_revision")) != identity.value("revision"))
            return Failure("AUX_REVISION_CONFLICT", "工程或修订已变化，请重新查询。");
        const auto context = host.context();
        if (!context.Succeeded()) return Failure(context);
        if (!context.value->document || !context.value->original) return Failure("AUX_HOST_UNAVAILABLE", "宿主没有地图数据快照。");
        const auto refs = host.references();
        if (identity != host.identity()) return Failure("AUX_REVISION_CONFLICT", "捕获地图/引用来源时修订已变化。");
        AuxiliaryEditRequest request;
        const auto kind = Text(arguments, "kind", 16);
        if (kind != "regions" && kind != "cameras") return Failure("AUX_KIND_INVALID", "kind 只接受 regions/cameras。");
        request.kind = kind == "regions" ? MapDataKind::Regions : MapDataKind::Cameras;
        request.expectedMemberSha256 = Text(arguments, "expected_member_sha256", 64);
        if (request.expectedMemberSha256 != "missing" && !IsLowerHexSha256(request.expectedMemberSha256))
            return Failure("AUX_MEMBER_STALE", "成员身份须为完整小写 SHA 或 missing。");
        const bool create = name == "auxiliary_create", copy = name == "auxiliary_copy", remove = name == "auxiliary_delete";
        request.action = create || copy ? AuxiliaryEditAction::Create : remove ? AuxiliaryEditAction::Delete : AuxiliaryEditAction::Update;
        if (!create) {
            if (!arguments.value("source").isObject()) return Failure("AUX_ARGUMENT_TYPE", "source 须为行定位对象。");
            const auto source = arguments.value("source").toObject();
            Keys(source, {{"source_index", QJsonObject{}}, {"row_key", QJsonObject{}}}, {"source_index", "row_key"});
            const auto index = ParseUnsignedId(source, "source_index", true);
            if (!index.Succeeded()) return Failure(index);
            if (Text(source, "source_index", 20) != std::to_string(*index.value))
                return Failure("AUX_ARGUMENT_INTEGER", "source_index 须为不含前导零的十进制字符串。");
            request.sourceIndex = *index.value; request.expectedRowKey = Text(source, "row_key", 16384);
            const auto sha = AuxiliaryEditing::MemberIdentity(*context.value->document, request.kind);
            if (!sha.Succeeded()) return Failure(sha);
            const auto row = MapFieldRowKey(*context.value->document, request.kind, request.sourceIndex);
            if (*sha.value != request.expectedMemberSha256 || !row.Succeeded() || *row.value != request.expectedRowKey)
                return Failure("AUX_ROW_STALE", "成员或完整行身份已变化。");
            if (!remove) {
                if (request.kind == MapDataKind::Regions) request.record = context.value->document->Regions()->entries[static_cast<std::size_t>(request.sourceIndex)];
                else request.record = context.value->document->Cameras()->entries[static_cast<std::size_t>(request.sourceIndex)];
            }
        } else if (request.kind == MapDataKind::Regions) request.record = RegionRecord{};
        else request.record = CameraRecord{};
        if (!remove) {
            if (!arguments.value("fields").isObject()) return Failure("AUX_ARGUMENT_TYPE", "fields 须为属性对象。");
            const auto fields = arguments.value("fields").toObject();
            if (copy) {
                if (!fields.contains("name") || (request.kind == MapDataKind::Regions && !fields.contains("creation_number")))
                    return Failure("AUX_COPY_IDENTITY_REQUIRED", "复制须明确提供新名称；区域还须提供新创建编号。");
                request.sourceIndex = 0; request.expectedRowKey.clear();
            }
            SetFields(request, fields, create);
        }
        auto plan = AuxiliaryEditing::Prepare(context.value->document, request, refs);
        if (!plan.Succeeded()) return Failure(plan);
        const auto member = AuxiliaryEditing::MemberIdentity(plan.value->After(), request.kind);
        if (!member.Succeeded()) return Failure(member);
        QJsonObject result{{"ok", true}, {"kind", QString::fromStdString(kind)}, {"operation", name.mid(10)},
            {"changed", plan.value->HasChanges()}, {"dry_run", dry}, {"applied", false}, {"saved", false}, {"runtime_verified", false},
            {"before_member_sha256", QString::fromStdString(request.expectedMemberSha256)}, {"after_member_sha256", QString::fromStdString(*member.value)},
            {"locator_scope", "snapshot-revision"}, {"angle_unit", "degrees"}};
        if (!remove) {
            const auto row = request.action == AuxiliaryEditAction::Create
                ? (request.kind == MapDataKind::Regions ? plan.value->After().Regions()->entries.size() : plan.value->After().Cameras()->entries.size()) - 1
                : static_cast<std::size_t>(request.sourceIndex);
            const auto key = MapFieldRowKey(plan.value->After(), request.kind, row);
            if (!key.Succeeded()) return Failure(key);
            result.insert(dry ? "candidate_identity" : "result_identity", QJsonObject{{"source_index", QString::number(static_cast<qulonglong>(row))},
                {"row_key", QString::fromStdString(*key.value)}, {"member_sha256", QString::fromStdString(*member.value)}});
        }
        const auto current = [&]() { return identity == host.identity(); };
        if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 1024 * 1024 - 32768)
            return Failure("AUX_RESPONSE_BUDGET", "区域/镜头候选回执超过响应预算。");
        if (!current()) return Failure("AUX_REVISION_CONFLICT", "准备区域/镜头命令时修订已变化。");
        if (!dry && plan.value->HasChanges()) {
            if (!host.apply) return Failure("AUX_HOST_UNAVAILABLE", "区域/镜头编辑宿主未接通。");
            bool validated = false;
            const auto applied = host.apply(*plan.value, [&](const MapDocument& before, const MapDocument& after,
                const MapDataEditSession::EditReceipt& receipt, const QJsonObject&) -> Status {
                if (validated || !current() || !receipt.SessionIdentity() || !receipt.CommandIdentity()
                    || receipt.AfterRevision() != receipt.BeforeRevision() + 1)
                    return Status::Failure("MCP.Auxiliary", "AUX_HOST_PROTOCOL", "宿主未在同一修订上预检一次原编辑命令。");
                const auto a = AuxiliaryEditing::MemberIdentity(before, request.kind), b = AuxiliaryEditing::MemberIdentity(after, request.kind);
                if (!a.Succeeded() || !b.Succeeded() || *a.value != request.expectedMemberSha256 || *b.value != *member.value)
                    return Status::Failure("MCP.Auxiliary", "AUX_HOST_PROTOCOL", "宿主预检成员不属于同一计划。");
                validated = true;
                result.insert("session_identity", QString::number(receipt.SessionIdentity()));
                result.insert("command_identity", QString::number(receipt.CommandIdentity()));
                return Status::Ok();
            });
            if (!applied.Succeeded()) return Failure(applied);
            if (!validated || !applied.value || QString::number(applied.value->SessionIdentity()) != result.value("session_identity")
                || QString::number(applied.value->CommandIdentity()) != result.value("command_identity"))
                return Failure("AUX_HOST_PROTOCOL", "宿主没有发布预检的同一原编辑命令。");
            result.insert("applied", true);
        }
        const auto afterIdentity = host.identity();
        for (auto it = afterIdentity.begin(); it != afterIdentity.end(); ++it) result.insert(it.key(), it.value());
        return result;
    } catch (const Invalid& error) { return Failure(error.code, error.message); }
    catch (const std::exception&) { return Failure("AUX_COMMAND_FAILED", "区域/镜头命令准备失败；请重新查询当前事务。"); }
}

} // namespace War3Map::Mcp
