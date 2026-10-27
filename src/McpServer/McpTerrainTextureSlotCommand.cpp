#include "McpTerrainTextureSlotCommand.h"
#include "McpToolInternal.h"

namespace War3Map::Mcp {
std::optional<Services::TerrainTextureSlotCommand> ParseTerrainTextureSlotCommand(
    const QJsonObject& object, QJsonObject& error) {
    const auto kind = object.value("kind").toString();
    const bool append = kind == "texture_slot_append";
    if (!append && kind != "texture_slot_replace") {
        error = Failure("MCP_TEXTURE_SLOT_KIND", "贴图槽命令仅接受texture_slot_replace或texture_slot_append。");
        return {};
    }
    const QString index = append ? "expected_count" : "slot";
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() != "kind" && it.key() != "table" && it.key() != index && it.key() != "rawcode"
            && (append || it.key() != "expected_rawcode")) {
            error = Failure("MCP_MAP_EDIT_FIELD_UNSUPPORTED", "贴图槽命令包含不支持的字段：" + it.key().toStdString());
            return {};
        }
    }
    for (const char* field : {"table", "rawcode", append ? "rawcode" : "expected_rawcode"}) {
        if (!object.value(field).isString()) {
            error = Failure("MCP_TEXTURE_SLOT_TEXT", "贴图表与rawcode必须是字符串。"); return {};
        }
    }
    const auto parsed = ParseInt32(object, append ? "expected_count" : "slot");
    if (!parsed.Succeeded()) { error = Failure(parsed); return {}; }
    if (*parsed.value < 0) { error = Failure("TERRAIN_TILESET_SLOT", "槽号与期望表长不能为负。"); return {}; }
    const auto table = Services::ParseTerrainTextureTable(object.value("table").toString().toStdString());
    if (!table.Succeeded()) { error = Failure(table); return {}; }
    Services::TerrainTextureSlotCommand command{append ? Services::TerrainTextureSlotOperation::Append : Services::TerrainTextureSlotOperation::Replace,
        *table.value, static_cast<std::uint32_t>(*parsed.value), object.value("expected_rawcode").toString().toStdString(),
        object.value("rawcode").toString().toStdString()};
    const auto valid = Services::NormalizeTerrainTextureSlotCommand(command);
    if (!valid.Succeeded()) { error = Failure(valid); return {}; }
    return command;
}

QJsonArray TerrainTextureSlotCommandSchemas() {
    QJsonArray schemas;
    for (const bool append : {false, true}) {
        const QJsonObject rawcode{{"type", "string"}, {"pattern", "^[ -~]{4}$"}};
        const QString index = append ? "expected_count" : "slot";
        QJsonObject properties{{"kind", QJsonObject{{"const", append ? "texture_slot_append" : "texture_slot_replace"}, {"type", "string"}}},
            {"table", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"ground", "cliff"}}}},
            {index, QJsonObject{{"type", "integer"}, {"minimum", append ? 1 : 0}, {"maximum", 15}}}, {"rawcode", rawcode}};
        QJsonArray required{"kind", "table", index, "rawcode"};
        if (!append) { properties["expected_rawcode"] = rawcode; required.append("expected_rawcode"); }
        schemas.append(QJsonObject{{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false},
            {"description", "槽号从0起；地面最多16槽、悬崖最多15槽。执行到本条时核对旧rawcode或当前表长；失败时整批不保存。资源与重复值仍由共用保存门校验。"}});
    }
    return schemas;
}
} // namespace War3Map::Mcp
