#include "GameUiModule.h"

#include "EditorProject.h"
#include "GameUiAuthoringJson.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <string_view>

// 控件事件 → 触发器：界面里的点击、鼠标进入、鼠标离开对应描述里的一个事件，作者的触发器用“界面事件”订阅它。
// 选已有触发器时在它的事件段加一行，解除时删去那一行；界面触发器（生成的部分）随之重新生成。
// 描述、界面触发器与作者触发器的修改经共用触发器事务一次写入。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr std::string_view kRegisterEvent = "AudastraUITriggerRegisterEvent";

const char* EventOn(UiEventKind kind) {
    switch (kind) {
    case UiEventKind::Enter: return "enter";
    case UiEventKind::Leave: return "leave";
    default: return "click";
    }
}

const UiEvent* FindEvent(const UiDescription& description, std::string_view frame, UiEventKind kind) {
    const auto found = std::find_if(description.events.begin(), description.events.end(),
        [&](const UiEvent& event) { return event.frame == frame && event.kind == kind; });
    return found == description.events.end() ? nullptr : &*found;
}

// 新事件的编号：控件编号_事件，太长或已用时改用 event 加序号。
std::string NewEventId(const UiDescription& description, const std::string& frame, UiEventKind kind) {
    const auto used = [&](const std::string& id) {
        return std::any_of(description.events.begin(), description.events.end(), [&](const UiEvent& event) { return event.id == id; });
    };
    std::string id = frame + "_" + EventOn(kind);
    if (IsValidUiId(id) && !used(id)) return id;
    for (int n = 1;; ++n) {
        id = "event" + std::to_string(n);
        if (!used(id)) return id;
    }
}

// 描述的事件表（set_events 的写法）。
QJsonArray EventsJson(const std::vector<UiEvent>& events) {
    QJsonArray items;
    for (const auto& event : events) items.append(QJsonObject{{"id", Text(event.id)}, {"frame", Text(event.frame)}, {"on", EventOn(event.kind)}});
    return items;
}

QString FirstError(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) return Text(diagnostic.message);
    }
    return diagnostics.empty() ? QString() : Text(diagnostics.front().message);
}

std::vector<std::string_view> Lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto end = text.find('\n');
        lines.push_back(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    return lines;
}

} // namespace

GameUiModule::FrameEventLink GameUiModule::FrameEventTriggers(const QString& descriptionId, const QString& frame, UiEventKind kind) const {
    FrameEventLink link;
    const UiDescription* description = Description(descriptionId);
    const UiEvent* event = description ? FindEvent(*description, frame.toStdString(), kind) : nullptr;
    if (!event) return link;
    link.eventId = Text(event->id);
    const std::string key = UiEventKey(description->id, event->id);
    link.key = Text(key);
    const auto* document = project_.triggerDocument_.get();
    if (!document) return link;
    const std::string quoted = Trigger::TriggerTextQuoted(key);
    std::vector<StableId> subscribers;
    for (const auto& node : document->ecaNodes) {
        if (node.type != 0 || node.name.text != kRegisterEvent || node.arguments.empty()) continue;
        const auto argument = node.arguments.front();
        if (argument >= document->argumentNodes.size()) continue;
        const std::string& value = document->argumentNodes[argument].value.text;
        if (value != key && value != quoted) continue;
        if (std::find(subscribers.begin(), subscribers.end(), node.sourceTriggerId) == subscribers.end()) subscribers.push_back(node.sourceTriggerId);
    }
    for (const auto& trigger : document->triggers) {
        if (std::find(subscribers.begin(), subscribers.end(), trigger.id) != subscribers.end()) link.triggers.append({trigger.id, Text(trigger.name.text)});
    }
    return link;
}

QList<std::pair<StableId, QString>> GameUiModule::EventTriggerCandidates() const {
    QList<std::pair<StableId, QString>> result;
    const auto* document = project_.triggerDocument_.get();
    if (!document) return result;
    std::vector<std::int32_t> generated;
    for (const auto& category : document->categories) {
        if (category.name.text == kUiTriggerCategory) generated.push_back(category.categoryId);
    }
    for (const auto& trigger : document->triggers) {
        if (trigger.customTextFlag != 0) continue;
        if (std::find(generated.begin(), generated.end(), trigger.categoryId) != generated.end()) continue;
        result.append({trigger.id, Text(trigger.name.text)});
    }
    return result;
}

std::optional<StableId> GameUiModule::LinkFrameEvent(const QString& descriptionId, const QString& frameId, UiEventKind kind,
    std::optional<StableId> trigger, QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<StableId> {
        if (error) *error = message;
        return std::nullopt;
    };
    const UiDescription* base = Description(descriptionId);
    if (!base) return fail(tr("地图里没有界面 %1").arg(descriptionId));
    const UiFrame* frame = FindUiFrame(*base, frameId.toStdString());
    if (!frame) return fail(tr("界面 %1 里没有控件 %2").arg(descriptionId, frameId));
    if (!project_.uiCatalog_ || !project_.triggerDocument_) return fail(tr("当前地图没有可编辑的触发器"));
    const Trigger::TriggerDocument& document = *project_.triggerDocument_;
    const UiEvent* existing = FindEvent(*base, frame->id, kind);
    const bool unlink = trigger && *trigger == 0;
    if (unlink && !existing) return StableId{0};

    // 描述：解除时去掉这个事件，否则没有时加上。
    UiDescription edited = *base;
    std::string eventId = existing ? existing->id : NewEventId(*base, frame->id, kind);
    if (unlink) {
        std::erase_if(edited.events, [&](const UiEvent& event) { return event.id == eventId; });
    } else if (!existing) {
        edited.events.push_back(UiEvent{eventId, frame->id, kind});
    }
    if (edited.events != base->events) {
        auto applied = ApplyUiEdits(*base, CompactEdits({QJsonObject{{"op", "set_events"}, {"events", EventsJson(edited.events)}}}).toStdString());
        if (!applied.value) return fail(FirstError(applied.diagnostics));
        edited = std::move(*applied.value);
    }
    const std::string key = UiEventKey(base->id, eventId);
    const std::string line = "    " + Trigger::TriggerTextName(kRegisterEvent) + "(" + Trigger::TriggerTextQuoted(key) + ")";

    const auto mapObjects = project_.CaptureTriggerMapObjectReferences();
    const Trigger::MapObjectReferenceSnapshot* snapshot = mapObjects.Succeeded() && mapObjects.value ? mapObjects.value->get() : nullptr;
    const auto render = [&](StableId id) -> Result<std::string> {
        return Trigger::RenderTriggerText(document, *project_.uiCatalog_, id, 1024U * 1024U, nullptr, snapshot);
    };
    std::vector<Trigger::GraphEditOperation> operations;
    std::string newTriggerName;
    const FrameEventLink current = FrameEventTriggers(descriptionId, frameId, kind);
    if (unlink) {
        // 各订阅它的触发器：事件段去掉这一行，段里没有别的事件时连同“events:”一起去掉。
        for (const auto& [id, name] : current.triggers) {
            const auto text = render(id);
            if (!text.value) return fail(tr("触发器 %1：%2").arg(name, FirstError(text.diagnostics)));
            const auto lines = Lines(*text.value);
            if (lines.empty() || lines.front() != "events:") continue;
            std::size_t end = 1;
            while (end < lines.size() && lines[end].starts_with(' ')) ++end;
            std::string expected, kept;
            std::size_t remaining = 0;
            for (std::size_t i = 0; i < end; ++i) {
                expected.append(lines[i]).push_back('\n');
                if (i == 0 || lines[i] == line) continue;
                kept.append(lines[i]).push_back('\n');
                if (!lines[i].starts_with("     ")) ++remaining;  // 缩进 4 格的是事件本身，更深的是它的续行
            }
            const std::string replacement = remaining == 0 ? std::string() : "events:\n" + kept;
            if (replacement == expected) continue;
            operations.emplace_back(Trigger::GraphPatchTriggerText{{Trigger::GraphRecordKind::Trigger, id, {}}, 1, end, expected, replacement});
        }
    } else if (trigger) {
        const auto& rows = document.triggers;
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.id == *trigger; });
        if (row == rows.end()) return fail(tr("没有编号为 %1 的触发器").arg(*trigger));
        const auto candidates = EventTriggerCandidates();
        if (std::none_of(candidates.begin(), candidates.end(), [&](const auto& item) { return item.first == *trigger; }))
            return fail(tr("触发器 %1 不能订阅界面事件：自定义代码触发器和生成的界面触发器不行").arg(Text(row->name.text)));
        const bool subscribed = std::any_of(current.triggers.begin(), current.triggers.end(), [&](const auto& item) { return item.first == *trigger; });
        if (!subscribed) {
            const auto text = render(*trigger);
            if (!text.value) return fail(tr("触发器 %1：%2").arg(Text(row->name.text), FirstError(text.diagnostics)));
            // 已有事件段时插在“events:”下一行，没有时连段名一起插到最前。
            const bool hasEvents = text.value->starts_with("events:\n") || *text.value == "events:";
            operations.emplace_back(Trigger::GraphPatchTriggerText{{Trigger::GraphRecordKind::Trigger, *trigger, {}},
                hasEvents ? 2u : 1u, hasEvents ? 1u : 0u, {}, (hasEvents ? line : "events:\n" + line) + "\n"});
        }
    } else {
        newTriggerName = UiTriggerName(edited, "事件 " + eventId, "On_" + eventId);
        const auto& rows = document.triggers;
        const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.name.text == newTriggerName; });
        if (row != rows.end()) {
            return LinkFrameEvent(descriptionId, frameId, kind, row->id, error);  // 同名触发器已有：让它订阅
        }
        const char* what = kind == UiEventKind::Click ? "点击" : kind == UiEventKind::Enter ? "鼠标进入" : "鼠标离开";
        const std::string text = "触发器 " + newTriggerName + ":\n    events:\n    " + line + "\n    actions:\n        CommentString(\""
            + "界面 " + base->id + " 的事件 " + eventId + "：" + frame->id + " " + what + "\")\n";
        const std::string category = base->label.empty() ? base->id : base->label;
        operations.emplace_back(Trigger::GraphApplyFeatureText{category, text, "uievent", std::nullopt});
    }

    const bool descriptionChanged = edited != *base;
    if (!descriptionChanged && operations.empty()) return unlink ? StableId{0} : *trigger;
    const QString what = kind == UiEventKind::Click ? tr("点击") : kind == UiEventKind::Enter ? tr("鼠标进入") : tr("鼠标离开");
    const QString label = unlink ? tr("界面 %1：%2 解除%3事件").arg(descriptionId, frameId, what)
                                 : tr("界面 %1：%2 的%3事件交给触发器").arg(descriptionId, frameId, what);
    QString staged;
    QString published;
    const auto generated = PublishDescriptionTriggers(edited, false, std::move(operations), label, [&]() -> Status {
        if (!descriptionChanged || StageDescription(edited, false, label, &staged)) return Status::Ok();
        return Status::Failure("GameUi", "AUI_EVENT_DESCRIPTION", staged.toStdString());
    }, &published);
    if (!generated) return fail(staged.isEmpty() ? published : staged);
    UpdateProperties();  // 事件一栏显示新的订阅
    if (unlink) return StableId{0};
    if (trigger) return *trigger;
    const auto& rows = project_.triggerDocument_->triggers;
    const auto created = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.name.text == newTriggerName; });
    if (created == rows.end()) return fail(tr("没有找到新建的触发器 %1").arg(Text(newTriggerName)));
    return created->id;
}

} // namespace War3Map::Modules
