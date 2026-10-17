#include "McpLuaFileTools.h"
#include "McpToolInternal.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <cmath>
#include <cstring>

namespace War3Map::Mcp {
namespace {
constexpr qsizetype kMaxLuaText = 2 * 1024 * 1024;
constexpr int kMaxLuaPageIndex = 2147483647;
constexpr int kMaxLuaPageLimit = 256;
constexpr int kMaxImportFiles = 32;
bool ValidPageNumber(const QJsonValue& value, int minimum, int maximum) {
    if (!value.isDouble()) return false;
    const auto number = value.toDouble();
    return std::isfinite(number) && number >= minimum && number <= maximum && std::floor(number) == number;
}
QJsonObject Identity() {
    return {{"project_identity", QJsonObject{{"type", "string"}, {"maxLength", 128}}},
        {"expected_revision", QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}}}};
}
bool ValidPath(const QJsonValue& value) {
    if (!value.isString()) return false;
    const auto path = value.toString();
    if (path.isEmpty() || path.size() > 260 || path.contains(QChar::Null) || !path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive))
        return false;
    // 地图内相对路径：不允许盘符、开头分隔符或 .. 段。
    if (path.contains(QLatin1Char(':')) || path.startsWith(QLatin1Char('\\')) || path.startsWith(QLatin1Char('/'))) return false;
    for (const auto& part : path.split(QRegularExpression(QStringLiteral("[\\\\/]")))) if (part == QStringLiteral("..")) return false;
    return true;
}
// 导入目标：地图内相对路径；Lua 文件走 lua_file_write，以便检查语法和进入 Lua 草稿规则。
bool ValidImportTarget(const QJsonValue& value) {
    if (!value.isString()) return false;
    const auto path = value.toString();
    if (path.isEmpty() || path.size() > 260 || path.contains(QChar::Null) || path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive))
        return false;
    if (path.contains(QLatin1Char(':')) || path.startsWith(QLatin1Char('\\')) || path.startsWith(QLatin1Char('/'))) return false;
    for (const auto& part : path.split(QRegularExpression(QStringLiteral("[\\\\/]")))) if (part == QStringLiteral("..")) return false;
    return true;
}
bool ValidDiskPath(const QJsonValue& value) {
    if (!value.isString()) return false;
    const auto path = value.toString();
    // 本机绝对路径：盘符或 UNC 开头。
    return !path.isEmpty() && path.size() <= 1024 && !path.contains(QChar::Null)
        && (QRegularExpression(QStringLiteral("^[A-Za-z]:[\\\\/]")).match(path).hasMatch() || path.startsWith(QStringLiteral("\\\\")));
}
} // namespace

std::vector<ToolDefinition> LuaFileToolDefinitions() {
    std::vector<ToolDefinition> result;
    const auto add = [&](const char* name, const char* description, ToolPermission permission, QJsonObject extra, QStringList required) {
        ToolDefinition tool; tool.name = name; tool.description = description; tool.permission = permission;
        auto properties = Identity();
        for (auto it = extra.begin(); it != extra.end(); ++it) properties.insert(it.key(), it.value());
        required.prepend("expected_revision"); required.prepend("project_identity");
        tool.inputSchema = BasicSchema(properties, QJsonArray::fromStringList(required));
        tool.maxRequestIdBytes = 128;
        tool.maxResponseBytes = std::strcmp(name, "lua_file_read") == 0 ? 3 * 1024 * 1024 : 64 * 1024;
        result.push_back(std::move(tool));
    };
    const QJsonObject path{{"type", "string"}, {"maxLength", 260}};
    add("lua_files_list", "分页列出地图里的 Lua 文件、状态和大小；默认每页64项，按next_offset继续，超预算时保持offset并缩小limit", ToolPermission::Query,
        {{"offset", QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxLuaPageIndex}, {"default", 0}}},
            {"limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxLuaPageLimit}, {"default", 64}}}}, {});
    add("lua_file_read", "读取一个地图 Lua 文件的正文和字节身份（sha256）", ToolPermission::Query, {{"path", path}}, {"path"});
    add("lua_file_write", "新建或整体替换一个地图 Lua 文件，进入撤销栈；替换已有文件时必须给出读取时的 sha256", ToolPermission::Edit,
        {{"path", path}, {"text", QJsonObject{{"type", "string"}, {"maxLength", kMaxLuaText}}},
            {"expected_sha256", QJsonObject{{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}}}, {"path", "text"});
    add("imports_add", "把本机文件导入地图（模型、贴图、声音等），进入资源撤销栈，保存地图时写入；"
        "一次1..32个，单个不超过64 MiB。地图里已有同路径文件时须 replace_existing=true。Lua 文件请用 lua_file_write", ToolPermission::Edit,
        {{"files", QJsonObject{{"type", "array"}, {"minItems", 1}, {"maxItems", kMaxImportFiles},
            {"items", BasicSchema({{"disk_path", QJsonObject{{"type", "string"}, {"maxLength", 1024},
                    {"description", "本机文件的绝对路径"}}},
                {"archive_path", QJsonObject{{"type", "string"}, {"maxLength", 260},
                    {"description", "地图内路径，例如 war3mapImported\\hero.mdx"}}}}, {"disk_path", "archive_path"})}}},
            {"replace_existing", QJsonObject{{"type", "boolean"}, {"default", false}}}}, {"files"});
    add("map_test_start", "用当前编辑内容启动单人测试（与界面“测试”相同，独立副本，不改原图）", ToolPermission::External, {}, {});
    return result;
}

QJsonObject InvokeLuaFileTool(const QString& name, const QJsonObject& args, const LuaFileToolHost& host) {
    QStringList allowed{"project_identity", "expected_revision"};
    if (name == "lua_files_list") allowed << "offset" << "limit";
    else if (name == "lua_file_read") allowed << "path";
    else if (name == "lua_file_write") allowed << "path" << "text" << "expected_sha256";
    else if (name == "imports_add") allowed << "files" << "replace_existing";
    else if (name != "lua_files_list" && name != "map_test_start") return Failure("LUA_FILE_TOOL_UNKNOWN", "未知 Lua 文件工具。");
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!allowed.contains(it.key())) return Failure("LUA_FILE_ARGUMENT_INVALID", "不接受的参数：" + it.key().toStdString());
    const auto identity = args.value("project_identity");
    if (!identity.isString() || identity.toString().isEmpty() || identity.toString().size() > 128
        || identity.toString().contains(QChar::Null) || !ParseUnsignedId(args, "expected_revision", true).Succeeded())
        return Failure("LUA_FILE_ARGUMENT_INVALID", "需要当前工程身份和编辑修订。");
    if (allowed.contains("path") && !ValidPath(args.value("path")))
        return Failure("LUA_FILE_ARGUMENT_INVALID", "path 必须是地图内的相对 .lua 路径。");
    if (name == "lua_files_list"
        && ((args.contains("offset") && !ValidPageNumber(args.value("offset"), 0, kMaxLuaPageIndex))
            || (args.contains("limit") && !ValidPageNumber(args.value("limit"), 1, kMaxLuaPageLimit))))
        return Failure("LUA_FILE_ARGUMENT_INVALID", "offset 必须是0..2147483647的整数，limit 必须是1..256的整数。");
    if (name == "lua_file_write") {
        if (!args.value("text").isString() || args.value("text").toString().size() > kMaxLuaText || args.value("text").toString().contains(QChar::Null))
            return Failure("LUA_FILE_ARGUMENT_INVALID", "text 必须是不超过 2 MiB 的文本。");
        if (args.contains("expected_sha256")
            && !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(args.value("expected_sha256").toString()).hasMatch())
            return Failure("LUA_FILE_ARGUMENT_INVALID", "expected_sha256 必须是 64 位小写十六进制。");
    }
    if (name == "imports_add") {
        const auto files = args.value("files");
        if (!files.isArray() || files.toArray().isEmpty() || files.toArray().size() > kMaxImportFiles)
            return Failure("IMPORTS_ADD_ARGUMENT_INVALID", "files 必须是1..32项的数组。");
        for (const auto& item : files.toArray()) {
            const auto file = item.toObject();
            if (!item.isObject() || file.size() != 2 || !ValidDiskPath(file.value("disk_path")) || !ValidImportTarget(file.value("archive_path")))
                return Failure("IMPORTS_ADD_ARGUMENT_INVALID",
                    "每项只能有 disk_path（本机绝对路径）和 archive_path（地图内相对路径，不能是 .lua）。");
        }
        if (args.contains("replace_existing") && !args.value("replace_existing").isBool())
            return Failure("IMPORTS_ADD_ARGUMENT_INVALID", "replace_existing 必须是 true 或 false。");
    }
    if (!host.invoke) return Failure("LUA_FILE_NO_EDITOR", "此宿主没有打开的地图，请连接正在编辑该地图的编辑器 MCP 端口。");
    auto result = host.invoke(name, args);
    if (name == "lua_files_list" && result.value("ok").toBool()) {
        // MCP会把compact正文作为content[0].text再次JSON转义；给128B request id与外壳留512B。
        const auto inner = QJsonDocument(result).toJson(QJsonDocument::Compact);
        const auto encoded = QJsonDocument(QJsonArray{QString::fromUtf8(inner)}).toJson(QJsonDocument::Compact);
        if (encoded.size() - 2 > 64 * 1024 - 512)
            return Failure("LUA_FILES_RESPONSE_BUDGET", "该页未返回；保持offset并减小limit。limit=1仍失败表示单条元数据超预算。");
    }
    return result;
}

void RegisterLuaFileTools(McpServer& server) {
    for (auto tool : LuaFileToolDefinitions()) {
        const auto name = tool.name;
        tool.handler = [name](const QJsonObject& args) { return InvokeLuaFileTool(name, args, {}); };
        server.RegisterTool(std::move(tool));
    }
}
} // namespace War3Map::Mcp
