#include "ProjectDocumentHistory.h"

#include "EditorProject.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QDataStream>
#include <QIODevice>
#include <QSignalBlocker>
#include <QUndoStack>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>

namespace War3Map::QtProject {
namespace {
constexpr quint32 kMagic = 0x46445531; // FDU1
constexpr std::size_t kMaximumCommands = 64;
using ObjectData::LniObjectCodec;
using ObjectData::LniObjectDocument;

QByteArray Array(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())};
}
std::span<const std::byte> Bytes(const QByteArray& array) {
    return std::as_bytes(std::span(array.constData(), static_cast<std::size_t>(array.size())));
}
void Put(QDataStream& out, const QByteArray& data) {
    out << quint32(data.size());
    if (!data.isEmpty()) out.writeRawData(data.constData(), int(data.size()));
}
bool Get(QDataStream& in, QByteArray& data) {
    quint32 size = 0;
    in >> size;
    if (in.status() != QDataStream::Ok || size > ProjectDocumentHistory::MaximumBytes
        || in.device()->bytesAvailable() < size) return false;
    data.resize(size);
    return !size || in.readRawData(data.data(), int(size)) == int(size);
}

Status Warning(std::string code, std::string message) {
    auto result = Status::Failure("Project.History", std::move(code), std::move(message));
    for (auto& diagnostic : result.diagnostics) diagnostic.severity = DiagnosticSeverity::Warning;
    return result;
}
std::string ScopeName(std::size_t scope) {
    return scope == 0 ? "触发器和变量" : "物编 " + std::string(ObjectData::ToString(static_cast<ObjectData::ObjectType>(scope - 1)));
}
std::string Source(const EditorProject& project) {
    return "document-undo:" + project.uiCatalog_->Sha256();
}

// 安装时文档已在目标位置；只让Qt建立同一撤销栈的游标和clean标记。
class RestoredCommand final : public QUndoCommand {
public:
    RestoredCommand(QString label, std::function<void()> before, std::function<void()> after, std::shared_ptr<bool> install)
        : QUndoCommand(std::move(label)), before_(std::move(before)), after_(std::move(after)), install_(std::move(install)) {}
    void undo() override { if (!*install_) before_(); }
    void redo() override { if (!*install_) after_(); }
private:
    std::function<void()> before_, after_;
    std::shared_ptr<bool> install_;
};
} // namespace

ProjectDocumentHistory::ProjectDocumentHistory(EditorProject& project) : project_(project) {}
ProjectDocumentHistory::~ProjectDocumentHistory() {
    for (auto& track : tracks_) QObject::disconnect(track.connection);
    QObject::disconnect(mapConnection_);
}
std::size_t ProjectDocumentHistory::State::Bytes() const noexcept {
    return std::size_t(first.size() + second.size() + extra.size() + assets.size()) + 256;
}

std::optional<QByteArray> ProjectDocumentHistory::Context(std::size_t scope) const {
    QByteArray bytes;
    if (!scope) {
        // 联动导入或运行库批准的命令不能只恢复一半。
        for (const auto& [name, value] : project_.editRevision_.Current()) {
            if (!name.starts_with("assets/") && !name.starts_with("imports/") && name != "runtime/approval") continue;
            bytes += QByteArray::fromStdString(name);
            bytes += '\0';
            bytes += QByteArray::fromStdString(value);
            bytes += '\0';
        }
    } else if (const auto& object = project_.objectDocuments_[scope - 1]) {
        for (const auto& source : object->CloneSources()) {
            if (source.newRawcode.size() != 4 || source.sourceRawcode.size() != 4) return {};
            bytes += QByteArray::fromStdString(source.newRawcode + source.sourceRawcode);
        }
    }
    return bytes;
}

std::optional<ProjectDocumentHistory::State> ProjectDocumentHistory::Capture(std::size_t scope, Encoding bytes) const {
    State state;
    auto context = Context(scope);
    if (!context) return {};
    if (!scope) {
        if (!project_.triggerDocument_ || !project_.uiCatalog_) return {};
        std::optional<Trigger::EncodedTriggerDocument> owned;
        if (!bytes.trigger) {
            auto encoded = Trigger::WtgWctCodec{}.EncodeForIdentity(*project_.triggerDocument_);
            if (!encoded.Succeeded() || !encoded.value) return {};
            owned = std::move(encoded.value);
            bytes.trigger = &*owned;
        }
        if (bytes.trigger->wtg.size() > MaximumBytes || bytes.trigger->wct.size() > MaximumBytes - bytes.trigger->wtg.size())
            return {};
        state.first = Array(bytes.trigger->wtg); state.second = Array(bytes.trigger->wct);
        if (!project_.guiConversionSnapshots_.Empty()) {
            const auto snapshot = project_.guiConversionSnapshots_.Serialize(*project_.triggerDocument_, *project_.uiCatalog_, Source(project_));
            if (!snapshot.Succeeded() || !snapshot.value || snapshot.value->size() > MaximumBytes - std::size_t(state.first.size() + state.second.size()))
                return {};
            state.extra = Array(*snapshot.value);
        }
        state.assets = std::move(*context);
    } else if (const auto& object = project_.objectDocuments_[scope - 1]) {
        std::optional<std::vector<std::byte>> owned;
        if (!bytes.object) {
            auto encoded = LniObjectCodec(object->Type()).Serialize(*object);
            if (!encoded.Succeeded() || !encoded.value) return {};
            owned = std::move(encoded.value);
            bytes.object = &*owned;
        }
        if (bytes.object->size() > MaximumBytes) return {};
        state.first = Array(*bytes.object);
        state.extra = std::move(*context);
    }
    if (state.Bytes() > MaximumBytes) return {};
    return state;
}

void ProjectDocumentHistory::Attach(std::size_t scope, QUndoStack* stack) {
    auto& track = tracks_[scope];
    QObject::disconnect(track.connection);
    track = {};
    if (!stack) return;
    track.stack = stack;
    for (int i = 0; i < stack->count(); ++i) track.commands.push_back(stack->command(i));
    track.index = std::size_t(stack->index());
    track.states.resize(track.commands.size() + 1);
    track.states[track.index] = Capture(scope, {});
    track.connection = QObject::connect(stack, &QUndoStack::indexChanged, &project_,
        [this, scope, stack] {
            const auto index = stack->index();
            // 可合并的命令可能在相同游标发布新值，指针列表不能证明该值没变。
            const bool merged = index > 0 && std::size_t(index) == tracks_[scope].index
                && stack->command(index - 1)->id() >= 0;
            Observe(scope, {}, merged);
        });
    EnforceBudget(scope, track.index);
}
void ProjectDocumentHistory::Reset() {
    Attach(0, project_.triggerUndoStack_);
    for (std::size_t i = 0; i < project_.objectUndoStacks_.size(); ++i) Attach(i + 1, project_.objectUndoStacks_[i]);
    QObject::disconnect(mapConnection_);
    auto* map = project_.mapDataUndoStack_;
    firstMapCommand_ = map && map->count() ? map->command(0) : nullptr;
    if (map) mapConnection_ = QObject::connect(map, &QUndoStack::indexChanged, &project_, [this, map] {
        const auto* previous = firstMapCommand_;
        firstMapCommand_ = map->count() ? map->command(0) : nullptr;
        if (previous && firstMapCommand_ != previous && map->undoLimit() > 0 && map->count() == map->undoLimit())
            project_.AddDiagnostics(Warning("PROJECT_UNDO_LIMIT", "地图数据与地形达到撤销预算，最早可撤销的操作为“"
                + map->text(0).toStdString() + "”。").diagnostics);
    });
    ++revision_;
}
void ProjectDocumentHistory::ResetTriggers() {
    Attach(0, project_.triggerUndoStack_);
    ++revision_;
}
void ProjectDocumentHistory::ReportLimit(std::size_t scope) {
    auto& track = tracks_[scope];
    if (track.limitReported || !track.stack) return;
    track.limitReported = true;
    const auto oldest = track.stack->count() ? track.stack->text(0).toStdString() : std::string{};
    project_.AddDiagnostics(Warning("PROJECT_UNDO_LIMIT",
        ScopeName(scope) + "达到撤销预算。当前会话最早可撤销的操作为“" + oldest + "”。").diagnostics);
}
bool ProjectDocumentHistory::MatchesStack(const Track& track) const {
    if (!track.stack || std::size_t(track.stack->count()) != track.commands.size()) return false;
    for (int i = 0; i < track.stack->count(); ++i)
        if (track.stack->command(i) != track.commands[i]) return false;
    return true;
}

bool ProjectDocumentHistory::ContextMatches(std::size_t scope, const State& state) const {
    const auto current = Context(scope);
    return current && *current == (scope ? state.extra : state.assets);
}

bool ProjectDocumentHistory::NeedsCapture(std::size_t scope) const {
    const auto& track = tracks_[scope];
    if (installing_ || !track.stack) return false;
    if (!MatchesStack(track)) return true;
    const auto index = std::size_t(track.stack->index());
    if (index == track.index) return false;
    return !track.states[index] || Trigger::TriggerCodeEditor::FullReplayVerificationEnabled()
        || !ContextMatches(scope, *track.states[index]);
}

void ProjectDocumentHistory::RecordTrigger(const Trigger::EncodedTriggerDocument* bytes) {
    Observe(0, {bytes, nullptr});
}

void ProjectDocumentHistory::RecordObject(std::size_t type, const std::vector<std::byte>* bytes) {
    Observe(type + 1, {nullptr, bytes});
}

void ProjectDocumentHistory::Observe(std::size_t scope, Encoding bytes, bool refresh) {
    auto& track = tracks_[scope];
    auto* stack = track.stack.data();
    if (installing_ || !stack) return;
    const bool sameCommands = MatchesStack(track);
    const auto index = std::size_t(stack->index());
    if (!refresh && sameCommands && index == track.index) return;
    const bool reuse = !refresh && sameCommands && track.states[index]
        && ContextMatches(scope, *track.states[index]);
    if (!sameCommands) {
        std::vector<const QUndoCommand*> commands;
        for (int i = 0; i < stack->count(); ++i) commands.push_back(stack->command(i));
        std::vector<std::optional<State>> states(commands.size() + 1);
        if (!commands.empty()) {
            const auto first = std::find(track.commands.begin(), track.commands.end(), commands.front());
            if (first != track.commands.end()) {
                const auto offset = std::size_t(first - track.commands.begin());
                states[0] = std::move(track.states[offset]);
                for (std::size_t i = 0; i < commands.size() && offset + i < track.commands.size()
                    && commands[i] == track.commands[offset + i]; ++i)
                    states[i + 1] = std::move(track.states[offset + i + 1]);
                if (offset) { track.prefixLost = true; ReportLimit(scope); }
            } else if (index == 1 && track.index < track.states.size()) {
                states[0] = std::move(track.states[track.index]);
            }
        } else {
            track.prefixLost = false;
            track.limitReported = false;
        }
        track.states = std::move(states);
        track.commands = std::move(commands);
    }
    track.index = index;
    ++revision_;
    if (reuse) {
        if (Trigger::TriggerCodeEditor::FullReplayVerificationEnabled() && Capture(scope, bytes) != track.states[index]) {
            track.states[index].reset();
            project_.editTrackingFailed_ = true;
            project_.AddDiagnostic(QObject::tr("错误"), QObject::tr("工程历史"),
                QStringLiteral("PROJECT_UNDO_HISTORY_MISMATCH"),
                QObject::tr("完整核验：撤销或重做后的文档与已记录的状态不一致。"));
        }
        return;
    }
    track.states[index] = Capture(scope, bytes);
    EnforceBudget(scope, index);
}
void ProjectDocumentHistory::EnforceBudget(std::size_t keepScope, std::size_t keepIndex) {
    std::size_t total = 0;
    for (const auto& track : tracks_) for (const auto& state : track.states) if (state) total += state->Bytes();
    for (std::size_t scope = 0; scope < tracks_.size() && total > MaximumBytes; ++scope) {
        auto& track = tracks_[scope];
        for (std::size_t i = 0; i < track.states.size() && total > MaximumBytes; ++i) {
            auto& state = track.states[i];
            if (!state || (scope == keepScope && i == keepIndex)) continue;
            total -= state->Bytes();
            state.reset();
            track.prefixLost = true;
        }
    }
}
void ProjectDocumentHistory::RefreshSavedObjects() {
    for (std::size_t scope = 1; scope < tracks_.size(); ++scope)
        if (tracks_[scope].stack && tracks_[scope].stack->count()) Observe(scope, {}, true);
}

Result<std::vector<std::byte>> ProjectDocumentHistory::Freeze() const {
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out << kMagic;
    Result<std::vector<std::byte>> result = Result<std::vector<std::byte>>::Success({});
    for (std::size_t scope = 0; scope < tracks_.size(); ++scope) {
        const auto& track = tracks_[scope];
        const auto* stack = track.stack.data();
        if (!stack || stack->count() == 0 || track.index >= track.states.size()) continue;
        const auto current = Capture(scope, {});
        const auto matches = [&](std::size_t index) {
            return current && track.states[index] && current->assets == track.states[index]->assets;
        };
        std::size_t begin = track.index, end = track.index;
        if (matches(track.index) && *current == *track.states[track.index]) {
            while (begin > 0 && matches(begin - 1) && end - begin < kMaximumCommands) --begin;
            while (end + 1 < track.states.size() && matches(end + 1) && end - begin < kMaximumCommands) ++end;
        }
        if (begin > 0 || end + 1 < track.states.size() || track.prefixLost) {
            const auto oldest = begin < end ? stack->text(int(begin)).toStdString() : std::string{};
            const auto warning = Warning("PROJECT_UNDO_HISTORY_LIMIT", oldest.empty()
                ? ScopeName(scope) + "没有可完整恢复的撤销步骤，当前草稿仍保存。"
                : ScopeName(scope) + "的工程历史最早恢复到“" + oldest + "”之前，当前草稿仍保存。");
            result.diagnostics.insert(result.diagnostics.end(), warning.diagnostics.begin(), warning.diagnostics.end());
        }
        if (begin == end) continue;
        const auto clean = stack->cleanIndex();
        out << quint8(scope) << quint8(end - begin) << quint8(track.index - begin)
            << quint8(clean >= int(begin) && clean <= int(end) ? clean - int(begin) : 255);
        for (std::size_t i = begin; i <= end; ++i) {
            const auto& state = *track.states[i];
            Put(out, state.first); Put(out, state.second); Put(out, state.extra);
            if (i != end) Put(out, stack->text(int(i)).toUtf8());
        }
    }
    if (data.size() > 4) out << quint8(255);
    if (out.status() != QDataStream::Ok || std::size_t(data.size()) > MaximumBytes) {
        auto warning = Warning("PROJECT_UNDO_HISTORY_LIMIT", "工程撤销记录超过64MiB，触发器和物编的撤销位置未恢复，当前草稿仍保存。");
        result.diagnostics.insert(result.diagnostics.end(), warning.diagnostics.begin(), warning.diagnostics.end());
    } else if (data.size() > 4) {
        const auto bytes = Bytes(data);
        result.value->assign(bytes.begin(), bytes.end());
    }
    return result;
}

Status ProjectDocumentHistory::Install(std::span<const std::byte> bytes) {
    if (bytes.empty()) return Status::Ok();
    const auto reject = [](std::string message) {
        return Warning("PROJECT_UNDO_HISTORY_INVALID", std::move(message) + "；当前草稿保持不变。");
    };
    if (bytes.size() > MaximumBytes) return reject("工程撤销记录超过64MiB");
    struct Tape {
        std::size_t scope = 0, index = 0;
        int clean = -1;
        std::vector<State> states;
        std::vector<QString> labels;
    };
    const auto parseTriggers = [&](const State& state, Trigger::TriggerDocument& document,
        Trigger::GuiConversionSnapshots& snapshots) {
        if (!project_.uiCatalog_) return false;
        auto parsed = Trigger::WtgWctCodec{}.Parse(Bytes(state.first), Bytes(state.second), *project_.uiCatalog_);
        if (!parsed.Succeeded() || !parsed.value) return false;
        if (!state.extra.isEmpty()) {
            auto decoded = Trigger::GuiConversionSnapshots::Parse(Bytes(state.extra), *parsed.value,
                *project_.uiCatalog_, Source(project_));
            if (!decoded.Succeeded() || !decoded.value) return false;
            snapshots = std::move(*decoded.value);
        }
        document = std::move(*parsed.value);
        return true;
    };
    const auto parseObjects = [&](std::size_t scope, const State& state) -> std::unique_ptr<LniObjectDocument> {
        const auto type = static_cast<ObjectData::ObjectType>(scope - 1);
        if (!state.second.isEmpty() || state.extra.size() % 8
            || std::size_t(state.extra.size() / 8) > LniObjectDocument::kMaximumCloneSources) return {};
        auto parsed = LniObjectCodec(type).Parse(Bytes(state.first));
        auto* document = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
        if (!document || !document->IsStructurallySafe()) return {};
        if (!state.extra.isEmpty()) {
            const auto& native = project_.nativeObjectDocuments_[scope - 1];
            if (!native) return {};
            std::vector<LniObjectDocument::CloneSource> sources;
            for (qsizetype i = 0; i < state.extra.size(); i += 8) {
                LniObjectDocument::CloneSource source{
                    std::string(state.extra.constData() + i, 4), std::string(state.extra.constData() + i + 4, 4)};
                const auto* target = document->FindObject({type, source.newRawcode});
                const auto* saved = native->FindObject({type, source.newRawcode});
                if (target && saved && target->parentRawcode == saved->parentRawcode) continue;
                sources.push_back(std::move(source));
            }
            if (!document->RestoreCloneSources(sources, *native, project_.objectFieldSchema_.get(), project_.standardCatalog_.get()).Succeeded())
                return {};
        }
        parsed.document.release();
        return std::unique_ptr<LniObjectDocument>(document);
    };
    const auto apply = [this, parseTriggers, parseObjects](std::size_t scope, const State& state) {
        if (scope == 0) {
            if (!project_.triggerDocument_) return;
            Trigger::TriggerDocument document;
            Trigger::GuiConversionSnapshots snapshots;
            if (!parseTriggers(state, document, snapshots)) {
                project_.AddDiagnostics(Warning("PROJECT_UNDO_HISTORY_INVALID", "已恢复的触发器历史不再匹配当前定义。").diagnostics);
                return;
            }
            emit project_.TriggerDocumentAboutToChange();
            *project_.triggerDocument_ = std::move(document);
            project_.guiConversionSnapshots_ = std::move(snapshots);
            project_.ObserveTriggerEdit();
            emit project_.TriggerDocumentChanged();
        } else {
            auto& target = project_.objectDocuments_[scope - 1];
            auto document = parseObjects(scope, state);
            if (!document || !target) {
                project_.AddDiagnostics(Warning("PROJECT_UNDO_HISTORY_INVALID", "已恢复的物编历史不再匹配当前文档。").diagnostics);
                return;
            }
            emit project_.ObjectDocumentsAboutToReplace();
            *target = std::move(*document);
            project_.ObserveObjectEdit(static_cast<int>(scope - 1));
            emit project_.ObjectDocumentsReplaced();
        }
    };
    std::vector<QUndoStack*> installedStacks;
    try {
        const auto payload = QByteArray::fromRawData(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
        QDataStream in(payload);
        quint32 magic = 0;
        in >> magic;
        if (magic != kMagic) return reject("工程撤销记录版本未知");
        std::vector<Tape> tapes;
        bool ended = false;
        while (!in.atEnd()) {
            quint8 scope = 255, count = 0, index = 0, clean = 255;
            in >> scope;
            if (scope == 255) { ended = true; break; }
            in >> count >> index >> clean;
            if (in.status() != QDataStream::Ok || scope >= tracks_.size() || !count || count > kMaximumCommands
                || index > count || (clean != 255 && clean > count) || (!tapes.empty() && tapes.back().scope >= scope))
                return reject("工程撤销栈数量、范围或索引无效");
            const auto current = Capture(scope, {});
            const auto* stack = scope == 0 ? project_.triggerUndoStack_ : project_.objectUndoStacks_[scope - 1];
            if (!current || !stack || stack->count() || (stack->undoLimit() > 0 && count > stack->undoLimit()))
                return reject("当前工程缺少可安装的撤销栈");
            Tape tape;
            tape.scope = scope; tape.index = index; tape.clean = clean == 255 ? -1 : clean;
            for (unsigned i = 0; i <= count; ++i) {
                State state;
                if (!Get(in, state.first) || !Get(in, state.second) || !Get(in, state.extra)) return reject("工程撤销记录截断");
                state.assets = current->assets;
                if (i == index && state != *current) return reject("撤销游标的文档指纹与实际工程草稿不符");
                if (scope == 0) {
                    Trigger::TriggerDocument document;
                    Trigger::GuiConversionSnapshots snapshots;
                    if (!parseTriggers(state, document, snapshots)) return reject("触发器撤销记录无法按当前定义解析");
                } else if (!parseObjects(scope, state)) return reject("物编撤销记录或克隆来源无法核对");
                tape.states.push_back(std::move(state));
                if (i != count) {
                    QByteArray label;
                    if (!Get(in, label) || label.size() > 4096) return reject("撤销操作名称无效");
                    tape.labels.push_back(QString::fromUtf8(label));
                }
            }
            tapes.push_back(std::move(tape));
        }
        if (!ended || !in.atEnd() || in.status() != QDataStream::Ok || tapes.empty())
            return reject("工程撤销记录未正常结束");
        const auto installing = std::make_shared<bool>(true);
        struct InstallFlag {
            bool& live;
            std::shared_ptr<bool> suspended;
            ~InstallFlag() { live = false; *suspended = false; }
        } flag{installing_, installing};
        installing_ = true;
        for (auto& tape : tapes) {
            auto* stack = tape.scope == 0 ? project_.triggerUndoStack_ : project_.objectUndoStacks_[tape.scope - 1];
            const QSignalBlocker block(stack);
            std::vector<std::unique_ptr<QUndoCommand>> commands;
            for (std::size_t i = 0; i < tape.labels.size(); ++i) {
                const auto action = [&](std::size_t at) { return [apply, scope = tape.scope, state = tape.states[at]] { apply(scope, state); }; };
                commands.push_back(std::make_unique<RestoredCommand>(tape.labels[i], action(i), action(i + 1), installing));
            }
            installedStacks.push_back(stack);
            for (auto& command : commands) stack->push(command.release());
            if (tape.clean >= 0) { stack->setIndex(tape.clean); stack->setClean(); }
            else stack->resetClean();
            stack->setIndex(int(tape.index));
            Attach(tape.scope, stack);
            auto& track = tracks_[tape.scope];
            for (std::size_t i = 0; i < tape.states.size(); ++i) track.states[i] = std::move(tape.states[i]);
        }
        for (auto* stack : installedStacks) {
            emit stack->indexChanged(stack->index());
            emit stack->cleanChanged(stack->isClean());
            emit stack->canUndoChanged(stack->canUndo());
            emit stack->canRedoChanged(stack->canRedo());
            emit stack->undoTextChanged(stack->undoText());
            emit stack->redoTextChanged(stack->redoText());
        }
        ++revision_;
        return Status::Ok();
    } catch (const std::exception&) {
        for (auto* stack : installedStacks) { const QSignalBlocker block(stack); stack->clear(); }
        Reset();
        return reject("工程撤销记录超过可恢复容量");
    }
}

} // namespace War3Map::QtProject
