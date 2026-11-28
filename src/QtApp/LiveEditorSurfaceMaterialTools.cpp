// 表面材质表（WarVK\materials.ini）的 Agent 命令：列出、读取、修改、删除与校验。与材质面板共用同一份
// 表文档、校验与工程读写（ProjectSurfaceMaterials）；身份/修订核对与写锁由活动编辑器工具宿主统一处理。
#include "LiveEditorWarVKTools.h"

#include "EditorProject.h"
#include "LiveEditorDiskImports.h"
#include "McpToolInternal.h"
#include "ProjectSurfaceMaterials.h"
#include "War3MapCore/Hash.h"

#include <QObject>

#include <algorithm>
#include <set>

namespace War3Map::QtApp {
namespace {
using QtProject::EditorProject;
using Services::SurfaceMaterialImageSource;
using Services::SurfaceMaterialTableDocument;
using Services::SurfaceMaterialTypeCategory;

constexpr int kDefaultLimit = 64;
constexpr int kMaxLimit = 256;
constexpr int kMaxImports = 5;

// 图片检查按内容哈希缓存，跨工程共用也不会串（同一内容同一结果）。
Services::SurfaceMaterialImageCache& ImageCache() {
    static Services::SurfaceMaterialImageCache cache;
    return cache;
}
QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QJsonArray Diagnostics(const std::vector<Diagnostic>& diagnostics) {
    return Mcp::DiagnosticsJson(diagnostics).value("diagnostics").toArray();
}
QString Category(SurfaceMaterialTypeCategory category) {
    switch (category) {
    case SurfaceMaterialTypeCategory::Doodad: return "doodad";
    case SurfaceMaterialTypeCategory::Destructible: return "destructible";
    case SurfaceMaterialTypeCategory::Ambiguous: return "ambiguous";
    case SurfaceMaterialTypeCategory::Unknown: break;
    }
    return "unknown";
}
QString State(SurfaceMaterialImageSource::State state) {
    switch (state) {
    case SurfaceMaterialImageSource::State::Found: return "found";
    case SurfaceMaterialImageSource::State::Failed: return "failed";
    case SurfaceMaterialImageSource::State::Missing: break;
    }
    return "missing";
}
QString Kind(Render::SurfaceMaterialKeyKind kind) {
    return kind == Render::SurfaceMaterialKeyKind::Image ? "image" : kind == Render::SurfaceMaterialKeyKind::Number ? "number" : "choice";
}
std::size_t KeyIndex(std::string_view name) {
    return static_cast<std::size_t>(Render::FindSurfaceMaterialKey(name) - Render::kSurfaceMaterialKeys.data());
}
QString Revision(const EditorProject& project) { return QString::number(project.editRevision_.Revision()); }
QString Slashes(QString path) { return path.replace(QLatin1Char('/'), QLatin1Char('\\')); }

QJsonObject StatsJson(const Render::SurfaceMaterialPixelStats& stats) {
    QJsonArray channels;
    constexpr const char* kNames[] = {"R", "G", "B", "A"};
    for (std::size_t c = 0; c < 4; ++c) {
        const auto& channel = stats.channels[c];
        channels.append(QJsonObject{{"channel", kNames[c]}, {"minimum", channel.minimum}, {"mean", channel.mean},
            {"maximum", channel.maximum}, {"deviation", channel.deviation}});
    }
    return {{"pixels", static_cast<qint64>(stats.pixels)}, {"channels", channels}, {"brightness_mean", stats.brightnessMean},
        {"metal_texels", static_cast<qint64>(stats.metalTexels)},
        {"orm_uncompressed", Render::SurfaceMaterialOrmUncompressed(stats)}};
}

QJsonObject ImageJson(const Services::SurfaceMaterialImageReport& image, bool withStats) {
    const auto* key = Render::FindSurfaceMaterialKey(image.key);
    QJsonObject row{{"key", Text(image.key)}, {"label", key ? Text(key->label) : Text(image.key)}, {"path", Text(image.path)},
        {"state", State(image.state)}};
    if (const auto& inspection = image.inspection) {
        row.insert("sha256", Text(inspection->sha256));
        row.insert("bytes", static_cast<qint64>(inspection->bytes));
        row.insert("format", Text(Render::SurfaceMaterialImageFormatName(inspection->header.format)));
        row.insert("width", static_cast<qint64>(inspection->header.width));
        row.insert("height", static_cast<qint64>(inspection->header.height));
        row.insert("game_readable", inspection->header.gameReadable);
        if (withStats) row.insert("stats", inspection->stats ? QJsonValue(StatsJson(*inspection->stats)) : QJsonValue());
    }
    return row;
}

QJsonArray FieldsJson(const std::optional<Services::SurfaceMaterialSection>& section) {
    QJsonArray fields;
    for (std::size_t i = 0; i < Render::kSurfaceMaterialKeys.size(); ++i) {
        const auto& key = Render::kSurfaceMaterialKeys[i];
        const auto& value = section ? section->values[i] : std::optional<std::string>{};
        QJsonObject field{{"key", Text(key.name)}, {"label", Text(key.label)}, {"kind", Kind(key.kind)},
            {"value", value ? QJsonValue(Text(*value)) : QJsonValue()}, {"written", value.has_value()},
            {"invalid", section && section->invalid[i]}};
        if (key.kind == Render::SurfaceMaterialKeyKind::Number) {
            field.insert("default", key.defaultValue);
            field.insert("minimum", key.minimum);
            field.insert("maximum", key.maximum);
        } else if (key.kind == Render::SurfaceMaterialKeyKind::Choice) {
            field.insert("default", Text(key.choices[0]));
            field.insert("choices", QJsonArray{Text(key.choices[0]), Text(key.choices[1])});
        }
        fields.append(field);
    }
    return fields;
}

QJsonObject TypeHeader(std::string_view rawcode, const Services::SurfaceMaterialTypeInfo& info) {
    return {{"rawcode", Text(rawcode)}, {"name", Text(info.name)}, {"category", Category(info.category)},
        {"placements", static_cast<qint64>(info.placements)}};
}

// 一个类型的完整结果；document 与 sources 可以是未提交的候选（dry_run）。
QJsonObject TypeJson(const SurfaceMaterialTableDocument& document, std::string_view rawcode,
    const QtProject::SurfaceMaterialTypeIndex& index, const Services::SurfaceMaterialValidationSources& sources) {
    auto result = TypeHeader(rawcode, index.Info(rawcode));
    const auto section = document.Section(rawcode);
    const auto validation = Services::ValidateSurfaceMaterials(document, rawcode, sources);
    result.insert("in_table", section.has_value());
    result.insert("headers", section ? static_cast<qint64>(section->headers) : 0);
    result.insert("line", section ? static_cast<qint64>(section->line) : 0);
    result.insert("merged", section && section->headers > 1);
    const auto& shader = section ? section->values[KeyIndex("shader")] : std::optional<std::string>{};
    result.insert("shader", section ? Text(shader ? *shader : "surface") : QJsonValue());
    result.insert("fields", FieldsJson(section));
    QJsonArray unknown;
    if (section) for (const auto& key : section->unknownKeys) unknown.append(Text(key));
    result.insert("unknown_keys", unknown);
    QJsonArray images, diagnostics;
    for (const auto& type : validation.types) {
        for (const auto& image : type.images) images.append(ImageJson(image, true));
        diagnostics = Diagnostics(type.diagnostics);
    }
    result.insert("images", images);
    result.insert("diagnostics", diagnostics);
    result.insert("table_diagnostics", Diagnostics(validation.table));
    return result;
}

std::optional<QJsonObject> ParsePage(const QJsonObject& arguments, int& offset, int& limit) {
    offset = arguments.value("offset").toInt(0);
    limit = arguments.value("limit").toInt(kDefaultLimit);
    if (offset < 0 || limit < 1 || limit > kMaxLimit)
        return Mcp::Failure("SURFACE_MATERIAL_ARGUMENT", "offset 须不小于 0，limit 须在 1–256 之间。");
    return std::nullopt;
}
std::optional<std::string> Rawcode(const QJsonObject& arguments) {
    const auto code = arguments.value("rawcode").toString().toStdString();
    if (!Render::SurfaceMaterialRawcode(code)) return std::nullopt;
    return code;
}
QJsonObject BadRawcode() { return Mcp::Failure("SURFACE_MATERIAL_RAWCODE_INVALID", "rawcode 须是四个可打印字符。"); }

QJsonObject WithTable(QJsonObject result, const EditorProject& project, bool exists, const std::string& sha256) {
    result.insert("ok", true);
    result.insert("table_path", Text(Render::kSurfaceMaterialTablePath));
    result.insert("table_exists", exists);
    result.insert("table_sha256", Text(sha256));
    result.insert("revision", Revision(project));
    return result;
}

QJsonObject List(EditorProject& project, const QJsonObject& arguments) {
    int offset = 0, limit = 0;
    if (auto failed = ParsePage(arguments, offset, limit)) return *failed;
    const auto filter = arguments.value("filter").toString("all");
    if (filter != "all" && filter != "with_material")
        return Mcp::Failure("SURFACE_MATERIAL_ARGUMENT", "filter 只能是 all 或 with_material。");
    const auto state = QtProject::ReadSurfaceMaterialTable(project);
    if (!state.Succeeded() || !state.value) return Mcp::Failure(state);
    const QtProject::SurfaceMaterialTypeIndex index(project);
    const auto sources = QtProject::SurfaceMaterialSources(project, index, &ImageCache());
    const auto sections = state.value->document.Sections();
    std::set<std::string, std::less<>> codes;
    for (const auto& section : sections) codes.insert(section.rawcode);
    if (filter == "all")
        for (const auto& entry : index.Placements()) if (Render::SurfaceMaterialRawcode(entry.first)) codes.insert(entry.first);
    QJsonArray types;
    int position = 0;
    for (const auto& code : codes) {
        if (position++ < offset || types.size() >= limit) continue;
        auto row = TypeHeader(code, index.Info(code));
        const auto section = std::find_if(sections.begin(), sections.end(), [&](const auto& item) { return item.rawcode == code; });
        row.insert("in_table", section != sections.end());
        if (section != sections.end()) {
            const auto& shader = section->values[KeyIndex("shader")];
            row.insert("shader", Text(shader ? *shader : "surface"));
            QJsonObject images;
            for (std::size_t i = 0; i < Render::kSurfaceMaterialKeys.size(); ++i)
                if (Render::kSurfaceMaterialKeys[i].kind == Render::SurfaceMaterialKeyKind::Image && section->values[i])
                    images.insert(Text(Render::kSurfaceMaterialKeys[i].name), Text(*section->values[i]));
            row.insert("images", images);
            const auto validation = Services::ValidateSurfaceMaterials(state.value->document, code, sources);
            std::size_t errors = 0, warnings = 0;
            for (const auto& type : validation.types)
                for (const auto& item : type.diagnostics) {
                    errors += item.severity == DiagnosticSeverity::Error;
                    warnings += item.severity == DiagnosticSeverity::Warning;
                }
            row.insert("errors", static_cast<qint64>(errors));
            row.insert("warnings", static_cast<qint64>(warnings));
        }
        types.append(row);
    }
    QJsonObject result{{"types", types}, {"types_total", static_cast<qint64>(codes.size())}, {"offset", offset},
        {"table_types", static_cast<qint64>(sections.size())}, {"table_type_limit", static_cast<qint64>(Render::kSurfaceMaterialTableMaxTypes)},
        {"table_warnings", Diagnostics(state.value->document.Table().warnings)}};
    return WithTable(std::move(result), project, state.value->exists, state.value->sha256);
}

QJsonObject Get(EditorProject& project, const QJsonObject& arguments) {
    const auto code = Rawcode(arguments);
    if (!code) return BadRawcode();
    const auto state = QtProject::ReadSurfaceMaterialTable(project);
    if (!state.Succeeded() || !state.value) return Mcp::Failure(state);
    const QtProject::SurfaceMaterialTypeIndex index(project);
    const auto sources = QtProject::SurfaceMaterialSources(project, index, &ImageCache());
    return WithTable(TypeJson(state.value->document, *code, index, sources), project, state.value->exists, state.value->sha256);
}

QJsonObject Validate(EditorProject& project, const QJsonObject& arguments) {
    int offset = 0, limit = 0;
    if (auto failed = ParsePage(arguments, offset, limit)) return *failed;
    std::string code;
    if (arguments.contains("rawcode")) {
        const auto parsed = Rawcode(arguments);
        if (!parsed) return BadRawcode();
        code = *parsed;
    }
    const auto state = QtProject::ReadSurfaceMaterialTable(project);
    if (!state.Succeeded() || !state.value) return Mcp::Failure(state);
    const QtProject::SurfaceMaterialTypeIndex index(project);
    const auto sources = QtProject::SurfaceMaterialSources(project, index, &ImageCache());
    const auto validation = Services::ValidateSurfaceMaterials(state.value->document, code, sources);
    QJsonArray types;
    for (int i = offset; i < static_cast<int>(validation.types.size()) && types.size() < limit; ++i) {
        const auto& type = validation.types[static_cast<std::size_t>(i)];
        auto row = TypeHeader(type.rawcode, type.type ? *type.type : Services::SurfaceMaterialTypeInfo{});
        QJsonArray images;
        for (const auto& image : type.images) images.append(ImageJson(image, false));
        row.insert("images", images);
        row.insert("diagnostics", Diagnostics(type.diagnostics));
        types.append(row);
    }
    QJsonObject counts{{"error", static_cast<qint64>(validation.Count(DiagnosticSeverity::Error))},
        {"warning", static_cast<qint64>(validation.Count(DiagnosticSeverity::Warning))},
        {"info", static_cast<qint64>(validation.Count(DiagnosticSeverity::Info))}};
    QJsonObject result{{"counts", counts}, {"table_diagnostics", Diagnostics(validation.table)}, {"types", types},
        {"types_total", static_cast<qint64>(validation.types.size())}, {"offset", offset}};
    if (!code.empty() && validation.types.empty())
        result.insert("note", "材质表里没有 " + Text(code) + "。");
    return WithTable(std::move(result), project, state.value->exists, state.value->sha256);
}

std::optional<QJsonObject> ParseDryRun(const QJsonObject& arguments, bool& dryRun) {
    if (arguments.contains("dry_run") && !arguments.value("dry_run").isBool())
        return Mcp::Failure("SURFACE_MATERIAL_ARGUMENT", "dry_run 须为布尔值。");
    dryRun = arguments.value("dry_run").toBool(false);
    return std::nullopt;
}

QJsonObject Set(EditorProject& project, const QJsonObject& arguments) {
    const auto code = Rawcode(arguments);
    if (!code) return BadRawcode();
    bool dryRun = false;
    if (auto failed = ParseDryRun(arguments, dryRun)) return *failed;
    const auto values = arguments.value("values").toObject();
    const auto imports = arguments.value("imports").toArray();
    if (values.isEmpty() && imports.isEmpty())
        return Mcp::Failure("SURFACE_MATERIAL_ARGUMENT", "values 与 imports 至少给出一项。");
    if (imports.size() > kMaxImports) return Mcp::Failure("SURFACE_MATERIAL_ARGUMENT", "一次最多导入 5 张图片。");
    const auto state = QtProject::ReadSurfaceMaterialTable(project);
    if (!state.Succeeded() || !state.value) return Mcp::Failure(state);
    auto document = state.value->document;
    for (auto field = values.begin(); field != values.end(); ++field) {
        const auto name = field.key().toStdString();
        const auto* key = Render::FindSurfaceMaterialKey(name);
        if (!key) return Mcp::Failure("SURFACE_MATERIAL_KEY_UNKNOWN", "未知键 " + name + "。");
        const auto value = field.value();
        Status status;
        if (value.isNull()) status = document.Unset(*code, name);
        else if (value.isDouble() && key->kind == Render::SurfaceMaterialKeyKind::Number)
            status = document.Set(*code, name, QByteArray::number(value.toDouble(), 'g', 9).toStdString());
        else if (value.isString())
            status = document.Set(*code, name, (key->kind == Render::SurfaceMaterialKeyKind::Image
                ? Slashes(value.toString()) : value.toString()).toStdString());
        else return Mcp::Failure("SURFACE_MATERIAL_VALUE_INVALID", name + " 的值须为字符串、数（数值键）或 null（删除）。");
        if (!status.Succeeded()) return Mcp::Failure(status);
    }
    std::vector<EditorProject::ImportPayload> payloads;
    if (auto failed = ReadDiskImports(imports, payloads)) return *failed;
    for (auto& payload : payloads) payload.archivePath = Slashes(payload.archivePath);
    // 预演与提交后的结果都按“当前资源 + 本次导入”读图。
    const QtProject::SurfaceMaterialTypeIndex index(project);
    auto sources = QtProject::SurfaceMaterialSources(project, index, &ImageCache());
    const auto projectRead = sources.readImage;
    sources.readImage = [&payloads, projectRead](std::string_view path) {
        const auto key = EditorProject::AssetKey(Text(path));
        for (const auto& payload : payloads)
            if (EditorProject::AssetKey(payload.archivePath) == key)
                return SurfaceMaterialImageSource{SurfaceMaterialImageSource::State::Found, payload.bytes, {}};
        return projectRead(path);
    };
    const bool changed = document.Text() != state.value->document.Text() || !payloads.empty();
    QJsonArray imported;
    for (const auto& payload : payloads) imported.append(payload.archivePath);
    auto result = TypeJson(document, *code, index, sources);
    result.insert("dry_run", dryRun);
    result.insert("changed", changed);
    result.insert("imports", imported);
    if (dryRun || !changed) {
        const auto sha = Sha256Text(document.Text());
        return WithTable(std::move(result), project, state.value->exists || changed, sha.value ? *sha.value : std::string{});
    }
    const auto rejected = QtProject::CommitSurfaceMaterialTable(project, state.value->sha256, document, std::move(payloads),
        arguments.value("replace_existing").toBool(false), QObject::tr("AI 修改材质 %1").arg(Text(*code)));
    if (!rejected.isEmpty()) return Mcp::Failure("SURFACE_MATERIAL_SET_REJECTED", rejected.toStdString());
    const auto after = QtProject::ReadSurfaceMaterialTable(project);
    if (!after.Succeeded() || !after.value) return Mcp::Failure(after);
    return WithTable(std::move(result), project, after.value->exists, after.value->sha256);
}

QJsonObject Remove(EditorProject& project, const QJsonObject& arguments) {
    const auto code = Rawcode(arguments);
    if (!code) return BadRawcode();
    bool dryRun = false;
    if (auto failed = ParseDryRun(arguments, dryRun)) return *failed;
    const auto state = QtProject::ReadSurfaceMaterialTable(project);
    if (!state.Succeeded() || !state.value) return Mcp::Failure(state);
    auto document = state.value->document;
    if (const auto status = document.RemoveSection(*code); !status.Succeeded()) return Mcp::Failure(status);
    QJsonObject result{{"removed", Text(*code)}, {"dry_run", dryRun}};
    if (dryRun) {
        const auto sha = Sha256Text(document.Text());
        return WithTable(std::move(result), project, true, sha.value ? *sha.value : std::string{});
    }
    const auto rejected = QtProject::CommitSurfaceMaterialTable(project, state.value->sha256, document, {}, false,
        QObject::tr("AI 删除材质 %1").arg(Text(*code)));
    if (!rejected.isEmpty()) return Mcp::Failure("SURFACE_MATERIAL_REMOVE_REJECTED", rejected.toStdString());
    const auto after = QtProject::ReadSurfaceMaterialTable(project);
    if (!after.Succeeded() || !after.value) return Mcp::Failure(after);
    return WithTable(std::move(result), project, after.value->exists, after.value->sha256);
}

QJsonObject RawcodeSchema() { return {{"type", "string"}, {"minLength", 4}, {"maxLength", 4}}; }
QJsonObject PageSchema(QJsonObject properties) {
    properties.insert("offset", QJsonObject{{"type", "integer"}, {"minimum", 0}});
    properties.insert("limit", QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxLimit}});
    return properties;
}
QJsonObject ValuesSchema() {
    QJsonObject properties;
    for (const auto& key : Render::kSurfaceMaterialKeys) {
        QJsonObject schema;
        if (key.kind == Render::SurfaceMaterialKeyKind::Image)
            schema = {{"type", QJsonArray{"string", "null"}}, {"maxLength", 259}};
        else if (key.kind == Render::SurfaceMaterialKeyKind::Number)
            schema = {{"type", QJsonArray{"number", "string", "null"}}};
        else
            schema = {{"type", QJsonArray{"string", "null"}}, {"enum", QJsonArray{Text(key.choices[0]), Text(key.choices[1]), QJsonValue()}}};
        properties.insert(Text(key.name), schema);
    }
    return {{"type", "object"}, {"additionalProperties", false}, {"properties", properties}};
}
} // namespace

void AppendSurfaceMaterialTools(std::vector<LiveWarVKTool>& tools) {
    tools.push_back({"world_surface_material_list",
        "列出可挂表面材质的类型（地图 WarVK\\materials.ini）：表里的类型和 war3map.doo 里摆放过的类型（filter=with_material 只列表里的），"
        "每行有 rawcode、名称、类别、摆放数；表里的类型另有着色方式、各图片槽路径、错误与提醒数。按 rawcode 排序分页。",
        Mcp::ToolPermission::Query,
        PageSchema({{"filter", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"all", "with_material"}}}}}), {}, List, {}});
    tools.push_back({"world_surface_material_get",
        "读取一个类型的表面材质：全部字段（INI 原名 key、中文名 label、生效值、是否写了、最后一次写的值是否无效、默认值与范围）、"
        "未知键、同名节合并情况、每张图的格式/尺寸/哈希/通道统计（最小、平均、最大、标准差，金属像素数），以及这一类型的诊断和表级诊断。",
        Mcp::ToolPermission::Query, {{"rawcode", RawcodeSchema()}}, {"rawcode"}, Get, {}});
    tools.push_back({"world_surface_material_set",
        "修改一个类型的表面材质：values 只改给出的键，值为 null 表示删去该键（同名节里的所有行），数值按最短十进制写出，图片路径的正斜杠按反斜杠写；"
        "类型不在表里时在表尾新加一节（最多 256 种）。imports 同时导入本机图片（{disk_path, archive_path}，最多 5 张，建议放 WarVK\\Materials\\），"
        "路径已有文件时须 replace_existing。表的其余行（注释、未知键、坏行、换行风格）原样保留。新表正文与导入的图片是资源撤销栈上的一步；"
        "dry_run=true 只返回结果与诊断，不改地图。返回该类型的完整结果（同 world_surface_material_get）。",
        Mcp::ToolPermission::Edit,
        QJsonObject{{"rawcode", RawcodeSchema()}, {"values", ValuesSchema()},
            {"imports", QJsonObject{{"type", "array"}, {"maxItems", kMaxImports}, {"items", QJsonObject{{"type", "object"},
                {"additionalProperties", false}, {"required", QJsonArray{"disk_path", "archive_path"}},
                {"properties", QJsonObject{{"disk_path", QJsonObject{{"type", "string"}, {"maxLength", 1024}}},
                    {"archive_path", QJsonObject{{"type", "string"}, {"maxLength", 259}}}}}}}}},
            {"replace_existing", QJsonObject{{"type", "boolean"}}}, {"dry_run", QJsonObject{{"type", "boolean"}}}},
        {"rawcode"}, {}, Set});
    tools.push_back({"world_surface_material_remove",
        "删除一个类型的表面材质：删去它的每一节（从节头到下一节头之前；紧挨下一节头的注释属于下一节，保留）。"
        "资源撤销栈上的一步；dry_run=true 不改地图。不删除图片文件。",
        Mcp::ToolPermission::Edit, {{"rawcode", RawcodeSchema()}, {"dry_run", QJsonObject{{"type", "boolean"}}}}, {"rawcode"}, {}, Remove});
    tools.push_back({"world_surface_material_validate",
        "校验表面材质表（整表，或给 rawcode 只校验一种类型）：表级（坏行、未知键、同名节、超过 256 种、表面类型没有图片）、"
        "类型级（不是装饰物/可破坏物、没有摆放）、图片级（不存在、格式游戏读不了、尺寸、平法线、非切线空间法线、常数 ORM、"
        "近镜面、金属使 ORM 不能压缩、细节图平均值近 0）。错误只有：图片不存在或读不了、格式游戏读不了、任一边超过 8192。",
        Mcp::ToolPermission::Query, PageSchema({{"rawcode", RawcodeSchema()}}), {}, Validate, {}});
}

} // namespace War3Map::QtApp
