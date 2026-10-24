#include "GameUiToolService.h"

#include "GameUiModule.h"
#include "War3MapCore/GameUi/UiTriggers.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
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

QString SiteText(const ScriptCallSite& site) {
    return QStringLiteral("%1:%2").arg(Text(site.scriptFile)).arg(site.line);
}

const char* ReasonName(ScriptUiCallReason reason) {
    switch (reason) {
    case ScriptUiCallReason::UntracedTarget: return "untraced-target";
    case ScriptUiCallReason::Unknown: return "unknown";
    default: return "not-previewed";
    }
}

const char* ValueTypeName(SimulationValueType type) {
    switch (type) {
    case SimulationValueType::Text: return "text";
    case SimulationValueType::Boolean: return "boolean";
    default: return "number";
    }
}

QJsonObject SimulationJson(const GameUiModule& module) {
    QJsonObject json{{"function", module.SimulationFunction()}};
    const SimulationResult* result = module.Simulation();
    if (!result) return json;
    QJsonArray inputs;
    for (const auto& input : result->inputs) {
        QJsonObject row{{"key", Text(input.key)}, {"label", Text(input.label)}, {"type", ValueTypeName(input.type)},
            {"value", Text(input.value)}, {"line", static_cast<qint64>(input.line)}};
        if (input.integer) row.insert("integer", true);
        if (input.condition) row.insert("condition", true);
        inputs.append(row);
    }
    json.insert("inputs", inputs);
    QJsonArray touched;
    for (const std::uint32_t frame : result->touched) {
        if (frame < result->frames.size()) touched.append(Text(result->frames[frame].name));
    }
    json.insert("touched", touched);
    json.insert("applied_calls", static_cast<qint64>(result->appliedCalls));
    QJsonArray notes;
    for (const auto& diagnostic : result->diagnostics) notes.append(Text(diagnostic.message));
    if (!notes.isEmpty()) json.insert("notes", notes);
    return json;
}

// MCP 输入值统一成文字：布尔为 true/false，数值取最短往返写法。
std::map<std::string, std::string> InputValues(const QJsonObject& given) {
    std::map<std::string, std::string> values;
    for (auto it = given.begin(); it != given.end(); ++it) {
        const QJsonValue& value = it.value();
        const QString text = value.isBool() ? (value.toBool() ? QStringLiteral("true") : QStringLiteral("false"))
            : value.isDouble()             ? QString::number(value.toDouble(), 'g', 17)
                                           : value.toString();
        values[it.key().toStdString()] = text.toStdString();
    }
    return values;
}

const char* EventKindName(UiEventKind kind) {
    switch (kind) {
    case UiEventKind::Enter: return "enter";
    case UiEventKind::Leave: return "leave";
    default: return "click";
    }
}

QJsonObject DescriptionJson(const GameUiModule& module, const GameUiDescriptionFile& file) {
    const UiDescription& description = file.description;
    const UiInstanceState preview = module.DescriptionPreview(Text(description.id));
    QJsonObject json{{"id", Text(description.id)}, {"path", Text(file.path)}, {"state", Text(preview.state)}};
    if (!description.label.empty()) json.insert("label", Text(description.label));
    QJsonArray states;
    for (const auto& state : description.states) states.append(QJsonObject{{"id", Text(state.id)}, {"label", Text(state.label)}});
    json.insert("states", states);
    QJsonArray values;
    for (const auto& value : description.values) {
        QJsonObject row{{"id", Text(value.id)}, {"type", Text(std::string(UiValueTypeName(value.type)))}};
        if (!value.label.empty()) row.insert("label", Text(value.label));
        if (value.min) row.insert("min", *value.min);
        if (value.max) row.insert("max", *value.max);
        if (value.size > 0) {
            row.insert("size", static_cast<qint64>(value.size));
            QJsonArray current;
            for (std::uint32_t i = 0; i < value.size; ++i) current.append(Text(UiValueText(description, preview, value.id, i).value_or("")));
            row.insert("current", current);
        } else {
            row.insert("current", Text(UiValueText(description, preview, value.id).value_or("")));
        }
        values.append(row);
    }
    json.insert("values", values);
    QJsonArray events;
    for (const auto& event : description.events) {
        events.append(QJsonObject{{"id", Text(event.id)}, {"frame", Text(event.frame)}, {"on", EventKindName(event.kind)}});
    }
    if (!events.isEmpty()) json.insert("events", events);
    return json;
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
    if (const ScriptFramePlan* plan = module.ScriptFrames()) {
        result.insert("script_frame_count", static_cast<qint64>(plan->frames.size()));
        QJsonArray calls;
        for (std::size_t i = 0; i < plan->calls.size() && i < static_cast<std::size_t>(kMaxListed); ++i) {
            const auto& group = plan->calls[i];
            QJsonArray sites;
            for (const auto& sample : group.samples) sites.append(SiteText(sample));
            calls.append(QJsonObject{{"function", Text(group.function)},
                {"family", Text(std::string(UiApiFamilyName(group.family)))}, {"reason", ReasonName(group.reason)},
                {"count", static_cast<qint64>(group.count)}, {"sites", sites}});
        }
        result.insert("script_calls", calls);
    }
    result.insert("simulation_functions", StringArray(module.SimulationFunctions(), kMaxListed));
    result.insert("simulation", SimulationJson(module));
    result.insert("show_hidden", module.ShowHidden());
    QJsonArray descriptions;
    const auto& files = module.Descriptions();
    for (std::size_t i = 0; i < files.size() && i < static_cast<std::size_t>(kMaxListed); ++i) {
        descriptions.append(DescriptionJson(module, files[i]));
    }
    result.insert("descriptions", descriptions);
    result.insert("description_count", static_cast<qint64>(files.size()));
    return result;
}

QJsonObject NodeJson(const FrameTree& tree, const FrameNode& node, double aspect, const ScriptFramePlan* plan) {
    QJsonObject row{{"id", static_cast<qint64>(node.id)}, {"name", Text(node.name)}, {"kind", KindName(node.kind)},
        {"type", Text(node.frameType)}, {"layout", LayoutName(node.layout)},
        {"parent", node.parent == kScreenNode || node.parent == kNoNode ? QJsonValue() : QJsonValue(static_cast<qint64>(node.parent))}};
    if (!node.codeLabel.empty()) row.insert("code_area", Text(node.codeLabel));
    if (node.codePlaced) row.insert("placed_by", "game-code");
    if (node.scriptFrame && plan && *node.scriptFrame < plan->frames.size()) {
        const ScriptFrame& frame = plan->frames[*node.scriptFrame];
        QJsonObject script{{"created", SiteText(frame.created)}, {"function", Text(frame.created.function)},
            {"call", Text(frame.created.call)}, {"family", Text(std::string(UiApiFamilyName(frame.family)))},
            {"runtime_calls", static_cast<qint64>(frame.runtimeCalls)}};
        if (frame.object != ScriptObjectKind::Frame) script.insert("object", Text(std::string(ScriptObjectKindName(frame.object))));
        if (!frame.variable.empty()) script.insert("variable", Text(frame.variable));
        if (frame.visible) script.insert("visible", *frame.visible);
        QJsonArray buttons;
        for (const auto& button : frame.buttons) buttons.append(Text(button));
        if (!buttons.isEmpty()) script.insert("buttons", buttons);
        if (frame.board) {
            // 格子文字只回前 kBoardRowsOut 行、kBoardColumnsOut 列，其余以 values_truncated 标明。
            constexpr std::uint32_t kBoardRowsOut = 32;
            constexpr std::uint32_t kBoardColumnsOut = 16;
            const ScriptBoard& board = *frame.board;
            QJsonArray values;
            for (std::uint32_t r = 0; r < std::min(board.rows, kBoardRowsOut); ++r) {
                QJsonArray cells;
                for (std::uint32_t c = 0; c < std::min(board.columns, kBoardColumnsOut); ++c) cells.append(Text(board.At(r, c)->value));
                values.append(cells);
            }
            script.insert("board", QJsonObject{{"rows", static_cast<qint64>(board.rows)},
                {"columns", static_cast<qint64>(board.columns)}, {"minimized", board.minimized}, {"values", values},
                {"values_truncated", board.rows > kBoardRowsOut || board.columns > kBoardColumnsOut}});
        }
        if (frame.leaderboard) {
            // 项数在分析时已限制在 kMaxLeaderboardItems 以内。
            const ScriptLeaderboard& board = *frame.leaderboard;
            QJsonArray items;
            for (const auto& item : board.items) {
                items.append(QJsonObject{{"label", Text(item.label)}, {"value", item.value},
                    {"player", item.player ? QJsonValue(static_cast<qint64>(*item.player)) : QJsonValue()},
                    {"show_label", item.showLabel}, {"show_value", item.showValue}, {"show_icon", item.showIcon}});
            }
            script.insert("leaderboard", QJsonObject{{"size", board.size}, {"show_title", board.showTitle},
                {"items", items}, {"clipped", board.clipped}, {"sort_ties", board.sortTies}});
        }
        QJsonArray dynamic;
        for (const auto& item : frame.dynamic) dynamic.append(Text(item));
        if (!dynamic.isEmpty()) script.insert("dynamic", dynamic);
        row.insert("script", script);
    }
    if (node.uiFrame) {
        QJsonObject link{{"description", Text(node.uiFrame->description)}, {"frame", Text(node.uiFrame->frame)}};
        if (node.uiFrame->instance) link.insert("instance", static_cast<qint64>(*node.uiFrame->instance));
        row.insert("description", link);
    }
    if (node.hidden) row.insert("hidden", true);
    if (node.fill) row.insert("fill", *node.fill);
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
        if (args.contains("show_hidden")) module->SetShowHidden(args.value("show_hidden").toBool());
        if (args.contains("show_only")) {
            const auto node = module->Tree() ? module->Tree()->FindByName(args.value("show_only").toString().toStdString())
                                             : std::nullopt;
            if (node) module->ShowOnly(*node);
            else missing.append(args.value("show_only"));
        }
        binding.insert("not_found", missing);
        return View(*module, binding);
    }

    if (name == QLatin1String("game_ui_simulate")) {
        if (!module->ScriptFrames()) return Failure("GAME_UI_LOADING", QObject::tr("界面定义仍在读取，请稍后重试。"));
        if (args.contains("function") && args.value("function").toString() != module->SimulationFunction()) {
            QString error;
            if (!module->SetSimulation(args.value("function").toString(), &error)) return Failure("GAME_UI_SIMULATION", error);
        }
        if (args.contains("values")) {
            if (module->SimulationFunction().isEmpty())
                return Failure("GAME_UI_SIMULATION", QObject::tr("未选择模拟函数，不能设置输入。"));
            module->SetSimulationValues(InputValues(args.value("values").toObject()));
        }
        binding.insert("simulation", SimulationJson(*module));
        return binding;
    }

    if (name == QLatin1String("game_ui_description_read") || name == QLatin1String("game_ui_description_create") ||
        name == QLatin1String("game_ui_description_edit")) {
        if (!module->Tree()) return Failure("GAME_UI_LOADING", QObject::tr("界面定义仍在读取，请稍后重试。"));
        const QString id = args.value("description").toString();
        QString error;
        if (name == QLatin1String("game_ui_description_create") &&
            !module->CreateDescription(id, args.value("label").toString(), &error, args.value("preset").toString())) {
            return Failure("GAME_UI_DESCRIPTION", error);
        }
        if (name == QLatin1String("game_ui_description_edit")) {
            const QString edits = QString::fromUtf8(QJsonDocument(args.value("edits").toArray()).toJson(QJsonDocument::Compact));
            if (!module->EditDescription(id, edits, QObject::tr("界面 %1：Agent 修改").arg(id), &error))
                return Failure("GAME_UI_DESCRIPTION", error);
        }
        const auto& files = module->Descriptions();
        const auto file = std::find_if(files.begin(), files.end(),
            [&](const GameUiDescriptionFile& candidate) { return Text(candidate.description.id) == id; });
        if (file == files.end()) return Failure("GAME_UI_DESCRIPTION", QObject::tr("地图里没有界面描述 %1。").arg(id));
        binding.insert("path", Text(file->path));
        binding.insert("text", Text(SerializeUiDescription(file->description)));
        return binding;
    }

    if (name == QLatin1String("game_ui_description_triggers")) {
        if (!module->Tree()) return Failure("GAME_UI_LOADING", QObject::tr("界面定义仍在读取，请稍后重试。"));
        const bool preview = args.value("preview").toBool();
        QString error;
        const auto generated = module->GenerateDescriptionTriggers(args.value("description").toString(), preview, &error);
        if (!generated) return Failure("GAME_UI_TRIGGERS", error);
        binding.insert("applied", !preview);
        binding.insert("category", Text(std::string(kUiTriggerCategory)));
        binding.insert("feature_text", generated->featureText);
        binding.insert("triggers", QJsonArray::fromStringList(generated->triggers));
        binding.insert("variables", QJsonArray::fromStringList(generated->variables));
        binding.insert("template_files", QJsonArray::fromStringList(generated->templateFiles));
        QJsonArray warnings;
        for (const auto& warning : generated->warnings) {
            warnings.append(QJsonObject{{"code", Text(warning.code)}, {"message", Text(warning.message)}});
        }
        binding.insert("warnings", warnings);
        return binding;
    }

    if (name == QLatin1String("game_ui_cast_system")) {
        const bool preview = args.value("preview").toBool();
        GameUiModule::CastSystemRequest request;
        for (const auto& item : args.value("abilities").toArray()) {
            const QJsonObject ability = item.toObject();
            request.abilities.push_back(CastAbility{ability.value("ability").toString().toStdString(), ability.value("cast").toDouble(),
                ability.value("channel").toDouble(), ability.value("tick").toDouble(), ability.value("effect").toString().toStdString()});
        }
        const QJsonObject bar = args.value("bar").toObject();
        request.bar = bar.value("description").toString();
        request.visible = bar.value("visible").toString();
        request.progress = bar.value("progress").toString();
        request.text = bar.value("text").toString();
        request.remaining = bar.value("remaining").toString();
        request.channel = bar.value("channel").toString();
        request.interrupted = bar.value("interrupted").toString();
        QString error;
        const auto generated = module->GenerateCastSystem(request, preview, &error);
        if (!generated) return Failure("GAME_UI_CAST_SYSTEM", error);
        binding.insert("applied", !preview);
        binding.insert("category", Text(std::string(kCastTriggerCategory)));
        binding.insert("feature_text", generated->featureText);
        binding.insert("triggers", QJsonArray::fromStringList(generated->triggers));
        binding.insert("variables", QJsonArray::fromStringList(generated->variables));
        binding.insert("registration_kept", generated->registrationKept);
        return binding;
    }

    if (name == QLatin1String("game_ui_party_system")) {
        const bool preview = args.value("preview").toBool();
        const QJsonObject frame = args.value("frame").toObject();
        GameUiModule::PartySystemRequest request;
        request.frame = frame.value("description").toString();
        request.shown = frame.value("shown").toString();
        request.health = frame.value("health").toString();
        request.mana = frame.value("mana").toString();
        request.name = frame.value("name").toString();
        request.dead = frame.value("dead").toString();
        request.outOfRange = frame.value("out_of_range").toString();
        request.select = frame.value("select").toString();
        if (args.value("name_source").toString() == QLatin1String("unit")) request.nameSource = PartyNameSource::Unit;
        request.farRange = args.value("far_range").toDouble(request.farRange);
        request.autoHeroes = args.value("auto_heroes").toBool(true);
        QString error;
        const auto generated = module->GeneratePartySystem(request, preview, &error);
        if (!generated) return Failure("GAME_UI_PARTY_SYSTEM", error);
        binding.insert("applied", !preview);
        binding.insert("category", Text(std::string(kPartyTriggerCategory)));
        binding.insert("feature_text", generated->featureText);
        binding.insert("triggers", QJsonArray::fromStringList(generated->triggers));
        binding.insert("variables", QJsonArray::fromStringList(generated->variables));
        binding.insert("slots", static_cast<qint64>(generated->slotCount));
        binding.insert("member_variable", generated->memberVariable);
        binding.insert("join_trigger", generated->joinTrigger);
        binding.insert("leave_trigger", generated->leaveTrigger);
        return binding;
    }

    if (name == QLatin1String("game_ui_preview_set")) {
        if (!module->Tree()) return Failure("GAME_UI_LOADING", QObject::tr("界面定义仍在读取，请稍后重试。"));
        const QString id = args.value("description").toString();
        const auto& files = module->Descriptions();
        const auto file = std::find_if(files.begin(), files.end(),
            [&](const GameUiDescriptionFile& candidate) { return Text(candidate.description.id) == id; });
        if (file == files.end()) return Failure("GAME_UI_DESCRIPTION", QObject::tr("地图里没有界面描述 %1。").arg(id));
        QString error;
        // 先检查数值再换状态：任何一项不合法都不改预览。
        if (args.contains("state")) {
            const QString state = args.value("state").toString();
            if (!state.isEmpty() && !std::any_of(file->description.states.begin(), file->description.states.end(),
                                       [&](const UiState& candidate) { return Text(candidate.id) == state; })) {
                return Failure("GAME_UI_DESCRIPTION", QObject::tr("界面 %1 没有状态 %2。").arg(id, state));
            }
        }
        if (args.contains("values") && !module->SetDescriptionValues(id, InputValues(args.value("values").toObject()), &error))
            return Failure("GAME_UI_DESCRIPTION", error);
        if (args.contains("state") && !module->SetDescriptionState(id, args.value("state").toString(), &error))
            return Failure("GAME_UI_DESCRIPTION", error);
        binding.insert("description", DescriptionJson(*module, *file));
        return binding;
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
            if (matched >= offset && rows.size() < limit)
                rows.append(NodeJson(tree, node, module->Aspect(), module->ScriptFrames()));
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
