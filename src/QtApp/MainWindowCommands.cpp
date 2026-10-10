#include "MainWindow.h"
#include "GlobalSearchDialog.h"
#include "MapSizeDialog.h"
#include "ProjectQuerySources.h"

#include "ObjectModule.h"
#include "TriggerModule.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <QAction>
#include <QLineEdit>
#include <QMenu>
#include <QMetaMethod>
#include <QPointer>
#include <QSettings>
#include <QShortcut>
#include <QUndoStack>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace {
namespace Services = War3Map::Services;

QString Text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

CommandPalette::Entry Notice(const QString& title, const QString& detail = {}) {
    return {title, detail, {}, {}, {}, QObject::tr("工程内容"), false};
}

QString CategoryLabel(Services::ProjectSearchCategory category) {
    static const QStringList labels{QObject::tr("触发器"), QObject::tr("变量"), QObject::tr("地图物体"),
        QObject::tr("原版物体"), QObject::tr("声音"), QObject::tr("资源"), QObject::tr("单位实例"),
        QObject::tr("地物实例"), QObject::tr("区域"), QObject::tr("镜头"), QObject::tr("代码声明")};
    const auto index = static_cast<std::size_t>(category);
    return index < static_cast<std::size_t>(labels.size()) ? labels[static_cast<qsizetype>(index)] : QString{};
}
} // namespace

void MainWindow::RegisterCommandAction(const std::string& id, QAction* action, const QString& group) {
    if (!action) return;
    const QPointer<QAction> guarded(action);
    Services::CommandDefinition command;
    command.id = id;
    command.title = action->text().remove('&').toStdString();
    command.group = group.toStdString();
    command.shortcut = action->shortcut().toString(QKeySequence::PortableText).toStdString();
    command.keywords = action->toolTip().toStdString();
    command.availability = [guarded]() {
        return Services::CommandAvailability{guarded && guarded->isEnabled(),
            guarded && guarded->isEnabled() ? std::string{} : std::string("当前状态不可用")};
    };
    // 面板与菜单/快捷键复用同一个 QAction，保留原保存、撤销及对话框路径。
    command.execute = [guarded]() {
        if (!guarded || !guarded->isEnabled()) return Services::CommandExecution{
            Services::CommandExecutionStatus::Disabled, {}, "当前状态不可用"};
        guarded->trigger();
        return Services::CommandExecution{};
    };
    std::string error;
    if (!commandRegistry_.Register(std::move(command), &error)) ShowStatusMessage(Text(error));
    action->setProperty("forgeCommandId", Text(id));
}

void MainWindow::SetupCommandPalette() {
    commandWindowId_ = "qt-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    RegisterCommandAction("edit.undo", UndoAction(), tr("编辑"));
    RegisterCommandAction("edit.redo", RedoAction(), tr("编辑"));
    RegisterCommandAction("navigation.back", BackAction(), tr("视图"));
    RegisterCommandAction("navigation.forward", ForwardAction(), tr("视图"));
    auto* search = new QAction(tr("全局搜索…"), this);
    search->setObjectName(QStringLiteral("globalProjectSearchAction"));
    search->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    search->setShortcutContext(Qt::ApplicationShortcut);
    addAction(search);
    connect(search, &QAction::triggered, this, &MainWindow::ShowGlobalSearch);
    RegisterCommandAction("search.global", search, tr("搜索"));
    Services::CommandDefinition replace;
    replace.id = "triggers.find-replace";
    replace.title = tr("触发器查找替换…").toStdString();
    replace.group = tr("搜索").toStdString();
    replace.keywords = "trigger parameter variable object reference replace";
    replace.availability = [this]() {
        const auto enabled = triggersModule_ && project_->triggerDocument_ && project_->uiCatalog_;
        return Services::CommandAvailability{enabled, enabled ? std::string{} : std::string("触发器或函数目录尚未载入")};
    };
    replace.execute = [this]() {
        const QPointer<MainWindow> window(this);
        const auto generation = project_->editRevision_.Generation();
        const auto* document = project_->triggerDocument_.get();
        const auto* catalog = project_->uiCatalog_.get();
        if (!TryNavigate({"triggers", QStringLiteral("find-replace"), {}, {}}) || !window)
            return Services::CommandExecution{Services::CommandExecutionStatus::Failed, {}, "当前草稿或来源未确认，查找窗口保持关闭。"};
        window->HideAgentWorkspace();
        if (!window) return Services::CommandExecution{Services::CommandExecutionStatus::Failed, {}, "工程窗口已关闭。"};
        window->SyncAssistantAction();
        if (!window || window->project_->editRevision_.Generation() != generation
            || window->project_->triggerDocument_.get() != document || window->project_->uiCatalog_.get() != catalog
            || window->project_->sourceChangeSuspected_ || window->project_->sourceChangeConfirmed_)
            return Services::CommandExecution{Services::CommandExecutionStatus::Failed, {}, "工程来源已变化，请重新打开查找窗口。"};
        window->triggersModule_->ShowFindReplace();
        if (!window) return Services::CommandExecution{Services::CommandExecutionStatus::Failed, {}, "工程窗口已关闭。"};
        return Services::CommandExecution{};
    };
    (void)commandRegistry_.Register(std::move(replace));
    for (auto* action : LayoutAction()->menu()->actions()) {
        RegisterCommandAction("layout." + action->data().toString().toStdString(), action, tr("布局"));
    }
    for (const auto& module : modules_.Modules()) {
        const auto info = module->Info();
        Services::CommandDefinition command;
        command.id = "module." + info.id;
        command.title = info.label.toStdString();
        command.group = tr("模块").toStdString();
        command.shortcut = info.shortcut.toString(QKeySequence::PortableText).toStdString();
        command.keywords = info.id;
        command.availability = [this, id = info.id]() {
            const auto* found = modules_.Find(id);
            if (!found) return Services::CommandAvailability{false, "模块未登记"};
            const auto current = found->Info();
            return Services::CommandAvailability{current.available, current.unavailableReason.toStdString()};
        };
        command.execute = [this, id = info.id]() { ActivateModule(id); return Services::CommandExecution{}; };
        (void)commandRegistry_.Register(std::move(command));
    }
    SetModuleCommandHandler([this](const auto& id) { ExecuteCommand("module." + id); });
    commandPalette_ = new CommandPalette(this);
    commandPalette_->SetQueryProvider([this](const QString& query, bool only) { return QueryCommandPalette(query, only); });
    for (const auto key : {Qt::Key_K, Qt::Key_P}) {
        auto* shortcut = new QShortcut(QKeySequence(Qt::CTRL | key), this);
        shortcut->setContext(Qt::ApplicationShortcut);
        connect(shortcut, &QShortcut::activated, this, [this]() { ShowCommandPalette(); });
    }
    auto* commandShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), this);
    commandShortcut->setContext(Qt::ApplicationShortcut);
    connect(commandShortcut, &QShortcut::activated, this, [this]() { ShowCommandPalette(">"); });
    if (globalSearch_) connect(globalSearch_, &QLineEdit::returnPressed, this,
        [this]() { ShowGlobalSearch(); });
    if (globalSearch_) globalSearch_->setToolTip(tr("全局搜索 · Ctrl+Shift+F；命令面板 · Ctrl+P"));
    const auto invalidate = [this]() { InvalidateCommandContext(); };
    connect(project_.get(), &War3Map::QtProject::EditorProject::TriggerDocumentAboutToChange, this, invalidate);
    connect(project_.get(), &War3Map::QtProject::EditorProject::ObjectDocumentsAboutToReplace, this, invalidate);
    connect(project_.get(), &War3Map::QtProject::EditorProject::ObjectDocumentsReplaced, this, invalidate);
    connect(project_.get(), &War3Map::QtProject::EditorProject::TriggerDocumentChanged, this, invalidate);
    connect(project_.get(), &War3Map::QtProject::EditorProject::MapDocumentChanged, this, invalidate);
    connect(project_.get(), &War3Map::QtProject::EditorProject::ImportsChanged, this, invalidate);
    connect(project_.get(), &War3Map::QtProject::EditorProject::SourceStateChanged, this, [this]() {
        // 来源监视通知不改变已接受文档；真实重载/修改仍由各文档信号失效，保存仍验证源 SHA。
        if (commandPalette_) commandPalette_->TraceSourceStateNotice();
        ScheduleChangesRefresh();
    });
    connect(project_.get(), &War3Map::QtProject::EditorProject::WorldEditStringsChanged, this, invalidate);
    connect(project_.get(), &War3Map::QtProject::EditorProject::UiLibrariesChanged, this, invalidate);
    connect(objectsModule_, &War3Map::Modules::ObjectModule::DocumentEdited, this, invalidate);
    connect(objectsModule_, &War3Map::Modules::ObjectModule::DocumentsChanged, this, invalidate);
    connect(project_->triggerUndoStack_, &QUndoStack::indexChanged, this, invalidate);
    connect(project_->mapDataUndoStack_, &QUndoStack::indexChanged, this, invalidate);
    for (auto* stack : project_->objectUndoStacks_) connect(stack, &QUndoStack::indexChanged, this, invalidate);
    if (startupMode_ == StartupMode::Normal) {
        std::vector<std::string> recent;
        for (const auto& id : QSettings().value("ui/commandRecent").toStringList()) recent.push_back(id.toStdString());
        (void)commandRegistry_.SetRecent(recent);
    }
}

void MainWindow::ExecuteCommand(const std::string& id) {
    const QPointer<MainWindow> window(this);
    const auto executed = commandRegistry_.Execute(id);
    if (!window) return;
    if (!executed.Succeeded()) {
        window->ShowStatusMessage(Text(executed.message));
        return;
    }
    if (window->startupMode_ == StartupMode::Normal) {
        QStringList recent;
        for (const auto& item : window->commandRegistry_.Recent()) recent.append(Text(item));
        QSettings().setValue("ui/commandRecent", recent);
    }
}

void MainWindow::ShowCommandPalette(const QString& initial) { commandPalette_->Show(initial); }

std::string MainWindow::CommandContextRevision() const {
    return commandWindowId_ + "/" + std::to_string(commandContextGeneration_);
}

void MainWindow::InvalidateCommandContext() {
    if (globalSearchDialog_) globalSearchDialog_->ContextChanged();
    if (mapSizeDialog_) mapSizeDialog_->ContextChanged();
    if (commandContextGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
        commandWindowId_ = "qt-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        commandContextGeneration_ = 1;
    } else ++commandContextGeneration_;
    commandMapRevision_.clear();
    commandMapSnapshot_.reset();
    commandMapDocument_.reset();
    if (commandPalette_) {
        // 仅记录固定来源/信号编号；真实修订失效策略保持不变。
        int source = 0, signal = 0;
        if (qEnvironmentVariableIntValue("FORGE_INPUT_TRACE") != 0) {
            const auto* origin = sender();
            source = origin == project_.get() ? 1 : origin == objectsModule_ ? 2 :
                origin == project_->triggerUndoStack_ ? 3 : origin == project_->mapDataUndoStack_ ? 4 : 0;
            if (!source && origin) for (const auto* stack : project_->objectUndoStacks_) if (origin == stack) source = 5;
            const char* const names[]{"TriggerDocumentAboutToChange", "ObjectDocumentsAboutToReplace", "ObjectDocumentsReplaced",
                "TriggerDocumentChanged", "MapDocumentChanged", "ImportsChanged", "SourceStateChanged", "WorldEditStringsChanged",
                "UiLibrariesChanged", "DocumentEdited", "DocumentsChanged", "indexChanged"};
            const int index = senderSignalIndex();
            if (origin && index >= 0 && index < origin->metaObject()->methodCount()) {
                const auto name = origin->metaObject()->method(index).name();
                for (int i = 0; i < static_cast<int>(std::size(names)); ++i) if (name == names[i]) signal = i + 1;
            }
        }
        commandPalette_->DismissForContextChange(source, signal);
    }
    ScheduleChangesRefresh();
}

QString MainWindow::RefreshCommandMapSnapshot() {
    const auto revision = CommandContextRevision();
    if (commandMapRevision_ == revision) return {};
    commandMapDocument_.reset(); commandMapSnapshot_.reset();
    if (project_->mapDocument_) {
        auto captured = War3Map::QtProject::CaptureProjectMapQuery(*project_, commandWindowId_, commandContextGeneration_);
        if (!captured.Succeeded()) return tr("地图查询快照失败");
        commandMapDocument_ = std::make_unique<War3Map::MapDocument>(std::move(captured.value->document));
        commandMapSnapshot_ = std::make_unique<Services::MapDataQuerySnapshot>(std::move(captured.value->data));
    }
    commandMapRevision_ = revision;
    return {};
}

std::vector<CommandPalette::Entry> MainWindow::QueryCommandPalette(const QString& query, bool commandsOnly) {
    std::vector<CommandPalette::Entry> entries;
    const auto utf8 = query.toStdString();
    const auto commands = commandRegistry_.Search(utf8, 0, commandsOnly ? 60 : query.trimmed().isEmpty() ? 6 : 8);
    if (!commands.valid) return {Notice(tr("查询失败"), Text(commands.error))};
    for (const auto& match : commands.items) {
        const auto& command = match.definition;
        entries.push_back({Text(command.title), Text(command.group), [this, id = command.id]() { ExecuteCommand(id); },
            Text("command/" + command.id), Text(command.shortcut), tr("命令"), match.availability.available});
    }
    if (commandsOnly) return entries;
    const auto revision = CommandContextRevision();
    Services::ProjectSearchOptions options; options.limit = 128;
    const auto found = QueryProjectSearchPage(revision, utf8, options, 40);
    if (!found.Succeeded()) { entries.push_back(Notice(tr("工程查询失败"))); return entries; }
    const auto captured = projectSearchCapture_ ? projectSearchCapture_->Borrow(*project_, revision)
        : War3Map::Result<Services::ProjectSearchContext>{};
    const auto ownedMap = captured.Succeeded() && captured.value->mapData ? captured.value->mapData->OwnedDocument() : nullptr;
    for (const auto& row : found.value->rows) {
        const bool specialDoodad = row.category == Services::ProjectSearchCategory::DoodadPlacement && row.sourceIndex &&
            ownedMap && ownedMap->Doodads() && *row.sourceIndex >= ownedMap->Doodads()->doodads.size();
        entries.push_back({Text(row.title), specialDoodad ? tr("特殊地物 · 尚未接入界面定位") : CategoryLabel(row.category),
            specialDoodad ? std::function<void()>{} : std::function<void()>{[this, row]() { ActivateSearchResult(row); }},
            Text(row.stableId), {}, tr("工程内容"), !specialDoodad});
    }
    if (!found.value->complete) {
        QStringList missing;
        for (const auto& coverage : found.value->coverage) {
            if (coverage.state != Services::ProjectSearchState::Ready &&
                coverage.state != Services::ProjectSearchState::QueryTooShort) missing.append(CategoryLabel(coverage.category));
        }
        entries.push_back(Notice((found.value->next.has_value() || found.value->windowHasMore) ? tr("还有匹配项，请缩小查询") : tr("部分工程内容未接入或未载入"), missing.join("、")));
    }
    return entries;
}

void MainWindow::ActivateSearchResult(const Services::ProjectSearchRow& row) {
    if (row.contextRevision != CommandContextRevision()) {
        ShowStatusMessage(tr("工程已变化，请重新搜索")); return;
    }
    if (projectSearchCapture_ && !projectSearchCapture_->Borrow(*project_, row.contextRevision).Succeeded()) {
        ShowStatusMessage(tr("搜索来源已变化，请重新搜索")); return;
    }
    using Category = Services::ProjectSearchCategory;
    if (row.category == Category::CodeSymbol) {
        const auto current = project_->CurrentCodeCheckIndex();
        if (!current || current->contextBinding != row.codeBinding || !row.codeSource || !row.codeLine) {
            ShowStatusMessage(tr("代码声明来源已变化，请检查当前代码后重新搜索")); return;
        }
        Navigate({"triggers", "code", Text(*row.codeSource), {{"line", static_cast<int>(row.codeLine)},
            {"column", static_cast<int>(row.codeColumn)}, {"codeBinding", Text(row.codeBinding)}}});
    } else if (row.category == Category::Trigger || row.category == Category::Variable) {
        if (!row.sourceId) return;
        if (row.category == Category::Trigger && row.ecaId && row.argumentId) {
            Navigate({"triggers", "eca", QString::number(*row.ecaId),
                {{"triggerId", QString::number(*row.sourceId)}, {"argumentId", QString::number(*row.argumentId)},
                    {"stableIdentity", true}}});
            return;
        }
        Navigate({row.category == Category::Trigger ? "triggers" : "variables",
            row.category == Category::Trigger ? "trigger" : "variable",
            QString::number(*row.sourceId), {{"stableIdentity", true}}});
    } else if (row.object) {
        if (row.category == Category::MapObject) {
            const auto index = static_cast<std::size_t>(row.object->type);
            if (index >= project_->objectDocuments_.size() || !project_->objectDocuments_[index] ||
                !project_->objectDocuments_[index]->FindObject(*row.object)) {
                ShowStatusMessage(tr("此地图物体尚未连接界面定位")); return;
            }
        }
        Navigate({"objects", "object", Text(std::string(War3Map::ObjectData::ToString(row.object->type)) + ":" + row.object->rawcode),
            {{"source", row.category == Category::MapObject ? QStringLiteral("map") : QStringLiteral("standard")}}});
    } else if (row.category == Category::ImportedFile) {
        ActivateSearchImportedFile(row);
    }
    else if (row.sourceIndex) {
        const auto index = QString::number(*row.sourceIndex);
        if (row.category == Category::Sound) Navigate({"sounds", "sound", Text(row.title), {{"sourceIndex", index}}});
        else {
            const QString kind = row.category == Category::UnitPlacement ? "unit" : row.category == Category::DoodadPlacement
                ? "doodad" : row.category == Category::Region ? "region" : "camera";
            Navigate({"entities", kind, Text(row.title), {{"sourceIndex", index}}});
        }
    }
}
