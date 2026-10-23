#include "GameUiToolService.h"

#include "GameUiModule.h"

#include <QBuffer>
#include <QJsonArray>
#include <QThread>

#include <algorithm>
#include <cmath>

namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

// PNG 上限与 MCP 图片结果一致。
constexpr qsizetype kMaxPngBytes = 4 * 1024 * 1024;
constexpr int kMaxListed = 200;

QJsonObject Failure(const char* code, const QString& message) {
    return {{"ok", false}, {"diagnostics", QJsonArray{QJsonObject{{"severity", "error"}, {"subsystem", "GameUi"},
        {"code", code}, {"message", message}}}}};
}

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

const char* SeverityName(DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::Error: return "error";
    case DiagnosticSeverity::Warning: return "warning";
    default: return "info";
    }
}

QString AspectName(double aspect) {
    if (std::abs(aspect - 16.0 / 10.0) < 0.01) return QStringLiteral("16:10");
    if (std::abs(aspect - 4.0 / 3.0) < 0.01) return QStringLiteral("4:3");
    return QStringLiteral("16:9");
}

double AspectValue(const QString& name) {
    if (name == QLatin1String("16:10")) return 16.0 / 10.0;
    if (name == QLatin1String("4:3")) return 4.0 / 3.0;
    return 16.0 / 9.0;
}

const char* KindName(DefinitionKind kind) {
    switch (kind) {
    case DefinitionKind::Texture: return "texture";
    case DefinitionKind::String: return "string";
    case DefinitionKind::Layer: return "layer";
    default: return "frame";
    }
}

const char* LayoutName(LayoutState state) {
    switch (state) {
    case LayoutState::Placed: return "placed";
    case LayoutState::SizeFromContent: return "size-from-content";
    case LayoutState::Cycle: return "cycle";
    default: return "unplaced";
    }
}

QJsonArray StringArray(const QStringList& values, int limit) {
    QJsonArray array;
    for (qsizetype i = 0; i < values.size() && i < limit; ++i) array.append(values[i]);
    return array;
}

QJsonObject View(const GameUiModule& module, QJsonObject result) {
    result.insert("race", module.Race());
    result.insert("expansion", module.Expansion());
    result.insert("aspect", AspectName(module.Aspect()));
    result.insert("roots", QJsonArray::fromStringList(module.Roots()));
    result.insert("loading", module.IsLoading() || !module.Tree());
    const auto problems = module.Problems();
    QJsonArray rows;
    for (std::size_t i = 0; i < problems.size() && i < static_cast<std::size_t>(kMaxListed); ++i) {
        rows.append(QJsonObject{{"severity", SeverityName(problems[i].severity)}, {"code", Text(problems[i].code)},
            {"message", Text(problems[i].message)}});
    }
    result.insert("problems", rows);
    result.insert("problem_count", static_cast<qint64>(problems.size()));
    const QStringList mapTemplates = module.MapTemplates();
    result.insert("map_templates", StringArray(mapTemplates, kMaxListed));
    result.insert("map_template_count", mapTemplates.size());
    return result;
}

QJsonObject NodeJson(const FrameTree& tree, const FrameNode& node, double aspect) {
    QJsonObject row{{"id", static_cast<qint64>(node.id)}, {"name", Text(node.name)}, {"kind", KindName(node.kind)},
        {"type", Text(node.frameType)}, {"layout", LayoutName(node.layout)},
        {"parent", node.parent == kScreenNode || node.parent == kNoNode ? QJsonValue() : QJsonValue(static_cast<qint64>(node.parent))}};
    if (!node.codeLabel.empty()) row.insert("code_area", Text(node.codeLabel));
    if (node.codePlaced) row.insert("placed_by", "game-code");
    if (!node.templateName.empty()) row.insert("template", Text(node.templateName));
    if (!node.sourceFile.empty()) row.insert("source", QStringLiteral("%1:%2").arg(Text(node.sourceFile)).arg(node.sourceLine));
    if (node.layout == LayoutState::Placed || node.layout == LayoutState::SizeFromContent) {
        row.insert("rect", QJsonArray{node.rect.left, node.rect.bottom, node.rect.right, node.rect.top});
        const QRect pixels = GameUiPixelRect(node.rect, tree.Options(), aspect);
        row.insert("pixels_1080p", QJsonArray{pixels.x(), pixels.y(), pixels.width(), pixels.height()});
    }
    QJsonArray anchors;
    for (const auto& anchor : node.anchors) {
        anchors.append(QJsonObject{{"point", Text(std::string(FramePointName(anchor.point)))},
            {"relative", anchor.relativeName.empty() ? QStringLiteral("(parent)") : Text(anchor.relativeName)},
            {"relative_point", Text(std::string(FramePointName(anchor.relativePoint)))}, {"x", anchor.x}, {"y", anchor.y}});
    }
    if (!anchors.isEmpty()) row.insert("anchors", anchors);
    if (node.texture) {
        row.insert("texture", QJsonObject{{"path", Text(node.texture->path)}, {"key", Text(node.texture->key)},
            {"skin_source", Text(node.texture->source)}});
    }
    if (node.text) {
        row.insert("text", QJsonObject{{"text", Text(node.text->text)}, {"key", Text(node.text->key)},
            {"font", Text(node.text->fontPath)}, {"font_height", node.text->fontHeight}});
    }
    return row;
}

} // namespace

GameUiToolService::GameUiToolService(ModuleProvider provider) : provider_(std::move(provider)) {}

QJsonObject GameUiToolService::Invoke(const QString& name, const QJsonObject& args) {
    GameUiModule* module = provider_ ? provider_() : nullptr;
    if (!module) return Failure("GAME_UI_UNAVAILABLE", QObject::tr("界面工作区不可用。"));
    if (QThread::currentThread() != module->thread())
        return Failure("GAME_UI_THREAD", QObject::tr("界面工具必须在编辑器线程执行。"));
    // 首次调用时创建工作区页面并开始后台读取。
    (void)module->Widget();
    QJsonObject binding{{"ok", true}, {"project_identity", args.value("project_identity")},
        {"revision", args.value("expected_revision")}};
    if (name == QLatin1String("game_ui_view_read")) return View(*module, binding);

    if (name == QLatin1String("game_ui_view_set")) {
        if (args.contains("race")) module->SetRace(args.value("race").toString());
        if (args.contains("expansion")) module->SetExpansion(args.value("expansion").toBool());
        if (args.contains("aspect")) module->SetAspect(AspectValue(args.value("aspect").toString()));
        QJsonArray missing;
        for (const auto& value : args.value("remove_roots").toArray()) {
            if (!module->RemoveRoot(value.toString())) missing.append(value);
        }
        if (!args.value("add_roots").toArray().isEmpty() && !module->Tree())
            return Failure("GAME_UI_LOADING", QObject::tr("界面定义仍在读取，稍后重试添加根界面。"));
        for (const auto& value : args.value("add_roots").toArray()) {
            if (!module->AddRoot(value.toString())) missing.append(value);
        }
        if (args.contains("show_only")) {
            const auto node = module->Tree() ? module->Tree()->FindByName(args.value("show_only").toString().toStdString())
                                             : std::nullopt;
            if (node) module->ShowOnly(*node);
            else missing.append(args.value("show_only"));
        }
        binding.insert("not_found", missing);
        return View(*module, binding);
    }

    if (!module->Tree()) return Failure("GAME_UI_LOADING", QObject::tr("界面定义仍在读取，请稍后重试。"));
    const FrameTree& tree = *module->Tree();
    if (name == QLatin1String("game_ui_nodes_query")) {
        const QString needle = args.value("name_contains").toString();
        const qint64 offset = static_cast<qint64>(args.value("offset").toDouble(0));
        const qint64 limit = static_cast<qint64>(args.value("limit").toDouble(50));
        QJsonArray rows;
        qint64 matched = 0;
        for (const auto& node : tree.Nodes()) {
            if (node.id == kScreenNode) continue;
            if (!needle.isEmpty() && !Text(node.name).contains(needle, Qt::CaseInsensitive) &&
                !Text(node.codeLabel).contains(needle, Qt::CaseInsensitive)) {
                continue;
            }
            if (matched >= offset && rows.size() < limit) rows.append(NodeJson(tree, node, module->Aspect()));
            ++matched;
        }
        binding.insert("rows", rows);
        binding.insert("total", matched);
        binding.insert("eof", offset + rows.size() >= matched);
        return binding;
    }

    if (name == QLatin1String("game_ui_render")) {
        if (module->IsLoading()) return Failure("GAME_UI_LOADING", QObject::tr("贴图或字体仍在读取，请稍后重试。"));
        const int width = static_cast<int>(args.value("width").toDouble(1920));
        const int height = static_cast<int>(args.value("height").toDouble(1080));
        const QImage image = module->RenderImage(QSize(width, height));
        if (image.isNull()) return Failure("GAME_UI_RENDER", QObject::tr("界面画布尚未创建。"));
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) return Failure("GAME_UI_RENDER", QObject::tr("PNG 编码失败。"));
        if (png.size() > kMaxPngBytes) return Failure("GAME_UI_RENDER_TOO_LARGE", QObject::tr("PNG 超过 4 MiB，请减小尺寸。"));
        binding.insert("width", width);
        binding.insert("height", height);
        binding.insert("roots", QJsonArray::fromStringList(module->Roots()));
        binding.insert("image", QJsonObject{{"mime_type", "image/png"}, {"encoding", "base64"},
            {"data", QString::fromLatin1(png.toBase64())}});
        return binding;
    }
    return Failure("GAME_UI_TOOL_UNKNOWN", QObject::tr("未知界面工具。"));
}

} // namespace War3Map::Modules
