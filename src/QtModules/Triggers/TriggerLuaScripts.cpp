#include "TriggerLuaScripts.h"

#include "TriggerCodeChecking.h"
#include "LuaCodeAssist.h"
#include "ProjectLuaFolder.h"
#include "War3MapCore/Services/LuaRuntimeCatalog.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Workspace.h"

#include <QHBoxLayout>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStringDecoder>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>

#include <algorithm>
#include <utility>

namespace War3Map::Modules {
namespace {
QString Problem(const std::vector<Diagnostic>& diagnostics) {
    QStringList messages;
    for (const auto& item : diagnostics) messages.push_back(QString::fromUtf8(item.message));
    return messages.join('\n');
}
std::shared_ptr<const std::vector<std::byte>> Payload(const QByteArray& text) {
    const auto* start = reinterpret_cast<const std::byte*>(text.constData());
    return std::make_shared<const std::vector<std::byte>>(start, start + text.size());
}
}

TriggerLuaScripts::TriggerLuaScripts(QtProject::EditorProject& project, QWidget* parent)
    : QWidget(parent), project_(project) {
    setObjectName("triggerLuaScripts");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(8);
    hluaToolbar_ = new QToolBar(tr("h-lua 工程"), this);
    hluaToolbar_->setObjectName("hluaProjectToolbar");
    for (const auto& entry : {std::pair{"inspect",tr("检查")},std::pair{"test-build",tr("构建测试图")},
        std::pair{"test",tr("测试")},std::pair{"build",tr("构建")},std::pair{"dist",tr("发布版")},
        std::pair{"ids",tr("名称与 ID 表")},std::pair{"framework",tr("工程框架…")}}) {
        auto* action=hluaToolbar_->addAction(entry.second);
        action->setObjectName(QStringLiteral("hlua-")+QString::fromLatin1(entry.first));
        action->setData(QString::fromLatin1(entry.first));
        connect(action,&QAction::triggered,this,[this,key=QString::fromLatin1(entry.first)]{emit HLuaActionRequested(key);});
    }
    layout->addWidget(hluaToolbar_);hluaToolbar_->hide();
    connect(&project_, &QtProject::EditorProject::LuaSourceChanged,this,&TriggerLuaScripts::RefreshHLuaToolbar);
    auto* splitter = new QSplitter(this);
    auto* directoryPane = new QWidget(splitter); auto* directoryLayout = new QVBoxLayout(directoryPane);
    directoryLayout->setContentsMargins(0, 0, 8, 0);
    auto* buttons = new QHBoxLayout;
    const auto button = [&](const char* name, const QString& title) {
        auto* control = new QPushButton(title, directoryPane); control->setObjectName(name); buttons->addWidget(control); return control;
    };
    auto* create = button("luaNewScript", tr("新建")); auto* rename = button("luaRenameScript", tr("改名")); auto* remove = button("luaDeleteScript", tr("删除"));
    directoryLayout->addLayout(buttons);
    auto* folderActions = new QHBoxLayout;
    auto* associate = new QPushButton(tr("关联外部文件夹…"), directoryPane);
    auto* unlink = new QPushButton(tr("取消关联"), directoryPane);
    auto* reconnect = new QPushButton(tr("重新连接"), directoryPane);
    associate->setObjectName("luaLinkFolder"); unlink->setObjectName("luaUnlinkFolder"); reconnect->setObjectName("luaReconnectFolder");
    folderActions->addWidget(associate); folderActions->addWidget(unlink); folderActions->addWidget(reconnect);
    directoryLayout->addLayout(folderActions);
    connect(associate, &QPushButton::clicked, this, [this] {
        if (!CommitPendingDraft()) return;
        const auto path = QFileDialog::getExistingDirectory(this, tr("关联外部Lua文件夹"));
        if (path.isEmpty()) return;
        auto& folder = project_.LuaFolderSource();
        const auto review = folder.Review(std::filesystem::path(path.toStdWString()));
        if (!review.Succeeded()) { Status(Problem(review.diagnostics)); return; }
        auto choice = Services::LuaFolderChoice::Map;
        if (!review.value->emptyDirectory) {
            const auto& diff = review.value->difference;
            QMessageBox dialog(QMessageBox::Question, tr("选择Lua来源"),
                tr("只在地图：%1　只在文件夹：%2　内容不同：%3\n写入文件夹的修改不随地图撤销。")
                    .arg(diff.mapOnly.size()).arg(diff.folderOnly.size()).arg(diff.different.size()), QMessageBox::NoButton, this);
            QStringList details;
            const auto section = [&](const QString& title, const auto& paths) {
                details << title;
                for (const auto& item : paths) details << QString::fromStdString(item);
            };
            section(tr("只在地图"), diff.mapOnly); section(tr("只在文件夹"), diff.folderOnly); section(tr("内容不同"), diff.different);
            dialog.setDetailedText(details.join('\n'));
            auto* disk = dialog.addButton(tr("以文件夹为准"), QMessageBox::AcceptRole);
            auto* map = dialog.addButton(tr("以地图为准"), QMessageBox::DestructiveRole);
            dialog.addButton(QMessageBox::Cancel); dialog.exec();
            if (dialog.clickedButton() == disk) choice = Services::LuaFolderChoice::Folder;
            else if (dialog.clickedButton() != map) return;
        }
        const auto applied = folder.Associate(*review.value, choice);
        if (!applied.Succeeded()) Status(Problem(applied.diagnostics));
        else Status(tr("已关联外部文件夹"));
        RefreshHLuaToolbar();
    });
    connect(unlink, &QPushButton::clicked, this, [this] {
        if (!CommitPendingDraft()) return;
        const auto result = project_.LuaFolderSource().Unlink();
        Status(result.Succeeded() ? tr("已取消关联") : Problem(result.diagnostics));
    });
    connect(reconnect, &QPushButton::clicked, this, [this] { project_.LuaFolderSource().Refresh(); });
    directory_ = new QTreeWidget(directoryPane); directory_->setObjectName("luaScriptDirectory");
    directory_->setHeaderLabels({tr("Lua 脚本"), tr("状态")}); directory_->setRootIsDecorated(false);
    directory_->setUniformRowHeights(true); directoryLayout->addWidget(directory_, 1);
    // 入口沿用 YDWE 约定：触发器“自定义代码”动作调用 Cheat("exec-lua: \"模块\"")；用法写在使用帮助里。
    auto* reveal = new QPushButton(tr("在资源中定位"), directoryPane); reveal->setObjectName("luaRevealAsset"); directoryLayout->addWidget(reveal);
    auto* codePane = new QWidget(splitter); auto* codeLayout = new QVBoxLayout(codePane); codeLayout->setContentsMargins(8, 0, 0, 0);
    sourceNote_ = new QLabel(codePane); sourceNote_->setObjectName("luaSourceNote"); sourceNote_->setTextFormat(Qt::PlainText); codeLayout->addWidget(sourceNote_);
    editor_ = new UiKit::CodeView(codePane); editor_->setObjectName("luaCodeEditor"); editor_->SetLanguage(UiKit::CodeLanguage::Lua);
    editor_->SetDecoder(DecodeMapCodeForView); editor_->SetEditable(false); codeLayout->addWidget(editor_, 1);
    problems_ = new UiKit::CodeProblemsView(codePane); problems_->setObjectName("luaCodeProblems"); problems_->SetView(editor_); codeLayout->addWidget(problems_);
    auto* codeActions = new QHBoxLayout;
    apply_ = new QPushButton(tr("应用草稿"), codePane); apply_->setObjectName("luaApplyDraft"); codeActions->addWidget(apply_);
    discard_ = new QPushButton(tr("放弃草稿"), codePane); discard_->setObjectName("luaDiscardDraft"); codeActions->addWidget(discard_);
    auto* check = new QPushButton(tr("检查语法"), codePane); check->setObjectName("luaCheckSyntax"); codeActions->addWidget(check);
    auto* undo = new QPushButton(tr("撤销资源修改"), codePane); auto* redo = new QPushButton(tr("重做资源修改"), codePane);
    codeActions->addWidget(undo); codeActions->addWidget(redo); codeActions->addStretch(); codeLayout->addLayout(codeActions);
    status_ = new QLabel(this); status_->setObjectName("luaScriptStatus"); status_->setTextFormat(Qt::PlainText);
    splitter->setChildrenCollapsible(false); splitter->addWidget(directoryPane); splitter->addWidget(codePane); splitter->setStretchFactor(1, 1); splitter->setSizes({280, 760});
    layout->addWidget(splitter, 1); layout->addWidget(status_);
    connect(create, &QPushButton::clicked, this, [this] {
        bool accepted = false; const auto path = QInputDialog::getText(this, tr("新建 Lua 脚本"), tr("地图内的导入路径："), QLineEdit::Normal, "Lua\\main.lua", &accepted);
        if (accepted) (void)CreateScript(path);
    });
    connect(rename, &QPushButton::clicked, this, [this] {
        if (selectedPath_.isEmpty()) return; bool accepted = false;
        const auto path = QInputDialog::getText(this, tr("重命名 Lua 脚本"), tr("新的导入路径："), QLineEdit::Normal, selectedPath_, &accepted);
        if (accepted) (void)RenameScript(selectedPath_, path);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!selectedPath_.isEmpty() && QMessageBox::question(this, tr("删除 Lua 脚本"), tr("删除 %1？").arg(selectedPath_)) == QMessageBox::Yes) (void)DeleteScript(selectedPath_);
    });
    connect(reveal, &QPushButton::clicked, this, [this] { if (!selectedPath_.isEmpty()) emit RevealAssetRequested(selectedPath_); });
    connect(apply_, &QPushButton::clicked, this, [this] { (void)CommitPendingDraft(); });
    connect(discard_, &QPushButton::clicked, this, &TriggerLuaScripts::DiscardPendingDraft);
    connect(check, &QPushButton::clicked, editor_, &UiKit::CodeView::RequestCheck);
    connect(undo, &QPushButton::clicked, project_.AssetUndoStack(), &QUndoStack::undo);
    connect(redo, &QPushButton::clicked, project_.AssetUndoStack(), &QUndoStack::redo);
    connect(project_.AssetUndoStack(), &QUndoStack::canUndoChanged, undo, &QPushButton::setEnabled);
    connect(project_.AssetUndoStack(), &QUndoStack::canRedoChanged, redo, &QPushButton::setEnabled);
    undo->setEnabled(project_.AssetUndoStack()->canUndo()); redo->setEnabled(project_.AssetUndoStack()->canRedo());
    connect(editor_, &QPlainTextEdit::textChanged, this, [this] { if (!suppress_) { UpdateDraftState(); emit DraftEdited(); } });
    connect(editor_, &UiKit::CodeView::TextLoaded, this, [this] {
        if (!editableSource_) { baselineText_ = editor_->toPlainText(); suppress_ = false; UpdateDraftState(); }
    });
    connect(directory_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (suppress_ || !item) return;
        const auto path = item->data(0, Qt::UserRole).toString();
        if (!SelectScript(path)) { QSignalBlocker block(directory_); for (int row = 0; row < directory_->topLevelItemCount(); ++row) if (directory_->topLevelItem(row)->data(0, Qt::UserRole).toString() == selectedPath_) directory_->setCurrentItem(directory_->topLevelItem(row)); }
    });
    connect(&project_, &QtProject::EditorProject::ImportsChanged, this, [this] {
        RefreshDirectory();
        if (project_.LuaFolderSource().Active()) return;
        if (publishing_ || selectedPath_.isEmpty()) return;
        if (HasPendingDraft()) return;
        const auto entries = project_.AssetEntries();
        const auto existing = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return QtProject::EditorProject::AssetKey(item.path) == QtProject::EditorProject::AssetKey(selectedPath_); });
        if (existing != entries.end()) { (void)LoadScript(existing->path); return; }
        QString renamed; std::size_t matches = 0;
        if (source_) for (const auto& item : entries) {
            if (!item.path.endsWith(".lua", Qt::CaseInsensitive) || item.ambiguous) continue;
            const bool same = (item.ownedPayload && item.ownedPayload == source_->bytes)
                || (!source_->originalPath.isEmpty() && QtProject::EditorProject::AssetKey(item.originalPath) == QtProject::EditorProject::AssetKey(source_->originalPath))
                || (!source_->diskPath.isEmpty() && item.diskPath == source_->diskPath);
            if (same) { renamed = item.path; ++matches; }
        }
        if (matches == 1) { (void)LoadScript(renamed); RefreshDirectory(); return; }
        source_.reset(); selectedPath_.clear(); editableSource_ = false; suppress_ = true;
        editor_->SetPrepareCheck({}); editor_->SetText({}); suppress_ = false; editor_->SetEditable(false);
        Status(matches > 1 ? tr("请重新选择文件") : tr("文件已删除")); UpdateDraftState();
    });
    connect(&project_, &QtProject::EditorProject::SourceStateChanged, this, [this] {
        if (publishing_ || !source_ || selectedPath_.isEmpty()) return;
        // 打开、恢复与工程另存的最终来源通知发生在世代和工程路径更新之后。
        // 只刷新干净视图；已有草稿保留旧身份，提交时仍必须拒绝来源替换。
        if (HasPendingDraft()) return;
        if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) return;
        (void)LoadScript(selectedPath_);
    });
    connect(&project_, &QtProject::EditorProject::LuaSourceChanged, this, [this] {
        RefreshDirectory();
        const auto& folder = project_.LuaFolderSource();
        if (folder.Missing()) Status(tr("外部文件夹不存在，使用地图内Lua"));
        else if (folder.Linked() && !folder.Active()) Status(tr("外部Lua不可用"));
        else Status({});
        if (publishing_ || selectedPath_.isEmpty()) return;
        const auto current = QtProject::ReadOwnedAsset(project_, selectedPath_);
        if (HasPendingDraft()) {
            if (!current.Succeeded() || current.value->sha256 != source_->sha256) Status(tr("外部文件已改变，草稿保留"));
            return;
        }
        if (folder.Linked() && !folder.Missing() && !folder.Active()) {
            editableSource_ = false; editor_->SetEditable(false); UpdateDraftState();
            return;
        }
        if (current.Succeeded()) LoadAcceptedScript(*current.value);
        else {
            source_.reset(); selectedPath_.clear(); editableSource_ = false; suppress_ = true;
            editor_->SetEditable(false); editor_->SetText({}); suppress_ = false; UpdateDraftState();
        }
    });
    RefreshDirectory(); UpdateDraftState();
}

void TriggerLuaScripts::RefreshHLuaToolbar() {
    const auto state=QtProject::HLuaFolderSourceInfo(project_);
    hluaToolbar_->setVisible(state.value("recognized").toBool());
    for(auto* action:hluaToolbar_->actions()) {
        action->setEnabled(action->data()=="framework" || state.value("available").toBool());
        action->setToolTip(action->isEnabled()?action->text():state.value("reason").toString());
    }
}

QString TriggerLuaScripts::StatusText() const { return status_->text(); }
void TriggerLuaScripts::Status(const QString& text) { status_->setText(text); }
QStringList TriggerLuaScripts::ScriptPaths() const {
    QStringList paths; for (int row = 0; row < directory_->topLevelItemCount(); ++row) paths.push_back(directory_->topLevelItem(row)->data(0, Qt::UserRole).toString()); return paths;
}
bool TriggerLuaScripts::HasPendingDraft() const { return editableSource_ && source_ && editor_->toPlainText() != baselineText_; }
void TriggerLuaScripts::UpdateDraftState() {
    const bool pending = HasPendingDraft(); apply_->setEnabled(pending); discard_->setEnabled(pending);
    const bool selected = source_.has_value() && !selectedPath_.isEmpty();
    for (const auto* name : {"luaRenameScript", "luaDeleteScript", "luaRevealAsset"}) {
        if (auto* button = findChild<QPushButton*>(name)) button->setEnabled(selected);
    }
    if (auto* button = findChild<QPushButton*>("luaCheckSyntax")) button->setEnabled(selected && editableSource_);
    if (!selected) { sourceNote_->clear(); sourceNote_->setToolTip({}); }
}
void TriggerLuaScripts::RefreshDirectory() {
    if (auto* button = findChild<QPushButton*>("luaUnlinkFolder")) button->setEnabled(project_.LuaFolderSource().Linked());
    if (auto* button = findChild<QPushButton*>("luaReconnectFolder")) button->setEnabled(project_.LuaFolderSource().Linked());
    QSignalBlocker block(directory_); directory_->clear();
    for (const auto& entry : QtProject::ListLuaScripts(project_)) {
        auto* item = new QTreeWidgetItem(directory_, {entry.path, entry.state});
        item->setData(0, Qt::UserRole, entry.path); item->setToolTip(0, entry.path);
        if (QtProject::EditorProject::AssetKey(entry.path) == QtProject::EditorProject::AssetKey(selectedPath_)) directory_->setCurrentItem(item);
    }
    directory_->sortItems(0, Qt::AscendingOrder);
}
bool TriggerLuaScripts::LoadScript(const QString& path) {
    const auto read = QtProject::ReadOwnedAsset(project_, path);
    if (!read.Succeeded()) { Status(Problem(read.diagnostics)); return false; }
    LoadAcceptedScript(*read.value);
    return true;
}
bool TriggerLuaScripts::SelectAcceptedScript(QtProject::OwnedAssetSource source) {
    // 此入口仅由导航适配器在当前草稿提交成功后调用。拒绝发生在任何正文 / 目录改变前。
    if (HasPendingDraft() || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_
        || source.archive != project_.archiveSession_.get() || source.directory != project_.directorySnapshot_.get()
        || source.sourcePath != project_.sourceArchivePath_ || source.projectPath != project_.projectFilePath_
        || source.generation != project_.editRevision_.Generation()
        || (source.archive ? source.archive->SourceSha256() != source.sourceSha256
            : source.directory && source.directory->Snapshot().aggregateSha256 != source.sourceSha256)) return false;
    if (!source.externalLuaRoot.empty()) {
        if (!QtProject::VerifyOwnedAsset(project_, source).Succeeded()) return false;
        LoadAcceptedScript(std::move(source)); RefreshDirectory(); return true;
    }
    const auto entries = project_.AssetEntries();
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& item) { return item.path == source.path; });
    if (entry == entries.end() || entry->ambiguous || !source.bytes || source.bytes->size() > QtProject::kOwnedLuaAssetBytes) return false;
    if (entry->ownedPayload && entry->ownedPayload != source.bytes) {
        // 同路径草稿刚由共同提交门发布时，队列现拥有新正文；只从内存重新捕获，不能显示旧预读正文。
        const auto current = QtProject::ReadOwnedAsset(project_, source.path);
        if (!current.Succeeded()) return false;
        source = *current.value;
    } else if (entry->size < 0 || static_cast<std::uint64_t>(entry->size) != source.bytes->size()
        || entry->originalPath != source.originalPath || entry->diskPath != source.diskPath
        || entry->runtimeLuaDraft != source.runtimeLuaDraft) return false;
    LoadAcceptedScript(std::move(source));
    RefreshDirectory();
    return true;
}
void TriggerLuaScripts::LoadAcceptedScript(QtProject::OwnedAssetSource source) {
    source_ = std::move(source); selectedPath_ = source_->path;
    const auto& bytes = *source_->bytes;
    const QByteArray raw(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless); const QString text = decoder.decode(raw);
    const bool utf8 = !decoder.hasError();
    // QTextDocument 会把这些合法字符投影成空格或换行；尚不能将任意编辑无损映射回原文。
    // 拒绝授予编辑能力，避免一次无关修改同时改变字符串、注释或原有行号。
    const bool displayLossless = !text.contains(QChar{0x00a0}) && !text.contains(QChar{0x2028}) && !text.contains(QChar{0x2029});
    editableSource_ = utf8 && displayLossless; suppress_ = true;
    editor_->SetSourceName(selectedPath_); editor_->SetEditable(editableSource_);
    if (editableSource_) { editor_->SetText(text); baselineText_ = editor_->toPlainText(); suppress_ = false; }
    else editor_->SetBytesAsync(raw);
    if (editableSource_) {
        // 补全、参数提示与 jass 接口检查共用同一份接口目录和模块快照。
        const auto catalog = project_.LuaRuntimeCatalog();
        const auto modules = CaptureLuaModules(project_, selectedPath_);
        const auto module = LuaModuleName(selectedPath_);
        editor_->SetAssist(MakeLuaCodeAssist(catalog, modules, module,
            project_.uiCatalog_ ? UiFunctionNotes(*project_.uiCatalog_) : QHash<QString, QString>{}, editor_));
        editor_->SetPrepareCheck([source = selectedPath_, catalog, modules, module](const QString& text, std::uint64_t generation) {
            Services::CodeCheckContext context; context.complete = false;
            context.luaSemantic.runtime = Services::OwnedRuntimeSemantics(*catalog);
            context.luaCatalog = catalog; context.luaModules = *modules; context.luaModuleName = module;
            return PrepareTriggerCodeCheck(text, generation, source, UiKit::CodeLanguage::Lua, std::move(context));
        });
    } else {
        editor_->SetAssist({});
        editor_->SetPrepareCheck({});
    }
    sourceNote_->setText(editableSource_ ? selectedPath_ : tr("%1 · 只读").arg(selectedPath_));
    sourceNote_->setToolTip(editableSource_ ? QString{} : utf8 ? tr("正文含无法无损编辑的特殊空白字符") : tr("不是 UTF-8 编码"));
    UpdateDraftState();
}
bool TriggerLuaScripts::SelectScript(const QString& path) {
    if (QtProject::EditorProject::AssetKey(path) == QtProject::EditorProject::AssetKey(selectedPath_) && source_) {
        if (HasPendingDraft() || QtProject::VerifyOwnedAsset(project_, *source_).Succeeded()) return true;
        return LoadScript(path);
    }
    if (!CommitPendingDraft()) return false;
    const bool loaded = LoadScript(path); if (loaded) RefreshDirectory(); return loaded;
}
bool TriggerLuaScripts::CreateScript(const QString& path, const QString& initialText) {
    if (!CommitPendingDraft()) return false;
    if (const auto& folder = project_.LuaFolderSource(); folder.Linked() && !folder.Missing() && !folder.Active()) {
        Status(tr("外部Lua来源不可用，请先重新连接")); return false;
    }
    if (project_.LuaFolderSource().Active()) {
        publishing_ = true;
        const auto problem = QtProject::WriteLuaScript(project_, path, initialText, {}, tr("新建外部Lua"));
        publishing_ = false;
        if (!problem.isEmpty()) { Status(problem); return false; }
        RefreshDirectory(); return LoadScript(path);
    }
    if (project_.directorySnapshot_ && !project_.archiveSession_) { Status(tr("目录扫描不完整，不能新建")); return false; }
    const auto bytes = initialText.toUtf8();
    const auto problem = project_.RecordAssetEdit(tr("新建 Lua 脚本草稿"), [&](QtProject::AssetState& candidate) -> QString {
        const auto entries = project_.AssetEntries(candidate);
        if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return QtProject::EditorProject::AssetKey(entry.path) == QtProject::EditorProject::AssetKey(path); })) return tr("已有同名资源。");
        return QtProject::PutOwnedAsset(project_, candidate, path, Payload(bytes));
    });
    if (!problem.isEmpty()) { Status(problem); return false; }
    const bool loaded = LoadScript(path);
    RefreshDirectory();
    Status(tr("已新建"));
    emit DraftEdited(); return loaded;
}
bool TriggerLuaScripts::RenameScript(const QString& path, const QString& newPath) {
    if (!CommitPendingDraft()) return false;
    if (const auto& folder = project_.LuaFolderSource(); folder.Linked() && !folder.Missing() && !folder.Active()) {
        Status(tr("外部Lua来源不可用，请先重新连接")); return false;
    }
    if (!newPath.endsWith(".lua", Qt::CaseInsensitive)) { Status(tr("扩展名必须是 .lua")); return false; }
    if (auto& folder = project_.LuaFolderSource(); folder.Active()) {
        const auto read = QtProject::ReadOwnedAsset(project_, path);
        if (!read.Succeeded()) { Status(Problem(read.diagnostics)); return false; }
        publishing_ = true;
        const auto result = Services::RenameLuaFolderFile(folder.Root(), {path.toStdString(), read.value->sha256, read.value->bytes}, newPath.toStdString());
        if (result.Succeeded()) folder.Refresh();
        publishing_ = false;
        if (!result.Succeeded()) { Status(Problem(result.diagnostics)); return false; }
        RefreshDirectory(); return LoadScript(newPath);
    }
    publishing_ = true;
    const auto problem = project_.RecordAssetEdit(tr("重命名 Lua 脚本"), [&](QtProject::AssetState& candidate) { return project_.MoveAsset(candidate, path, newPath); });
    publishing_ = false;
    if (!problem.isEmpty()) { Status(problem); return false; }
    RefreshDirectory(); return LoadScript(newPath);
}
bool TriggerLuaScripts::DeleteScript(const QString& path) {
    if (!CommitPendingDraft()) return false;
    if (const auto& folder = project_.LuaFolderSource(); folder.Linked() && !folder.Missing() && !folder.Active()) {
        Status(tr("外部Lua来源不可用，请先重新连接")); return false;
    }
    if (auto& folder = project_.LuaFolderSource(); folder.Active()) {
        const auto read = QtProject::ReadOwnedAsset(project_, path);
        if (!read.Succeeded()) { Status(Problem(read.diagnostics)); return false; }
        const auto result = Services::RemoveLuaFolderFile(folder.Root(), {path.toStdString(), read.value->sha256, read.value->bytes});
        if (!result.Succeeded()) { Status(Problem(result.diagnostics)); return false; }
        folder.Refresh(); return true;
    }
    publishing_ = true;
    const auto problem = project_.RecordAssetEdit(tr("删除 Lua 脚本"), [&](QtProject::AssetState& candidate) { return project_.RemoveAsset(candidate, path); });
    publishing_ = false;
    if (!problem.isEmpty()) { Status(problem); return false; }
    if (QtProject::EditorProject::AssetKey(path) == QtProject::EditorProject::AssetKey(selectedPath_)) {
        source_.reset(); selectedPath_.clear(); editableSource_ = false; suppress_ = true;
        editor_->SetPrepareCheck({}); editor_->SetText({}); suppress_ = false; editor_->SetEditable(false);
    }
    RefreshDirectory(); UpdateDraftState(); emit DraftEdited(); return true;
}
bool TriggerLuaScripts::CommitPendingDraft() {
    if (!HasPendingDraft()) return true;
    if (!source_->externalLuaRoot.empty()) project_.LuaFolderSource().Refresh();
    // 工程草稿保留未完成代码；后台语法诊断与地图写出门禁独立核对。
    auto expected = source_->sha256;
    const auto verified = QtProject::VerifyOwnedAsset(project_, *source_);
    if (!verified.Succeeded()) {
        if (source_->externalLuaRoot.empty()) { Status(Problem(verified.diagnostics)); return false; }
        QMessageBox dialog(QMessageBox::Warning, tr("Lua文件冲突"), tr("外部文件已改变，选择保留哪一份。"), QMessageBox::NoButton, this);
        auto* draft = dialog.addButton(tr("保留编辑器草稿"), QMessageBox::AcceptRole);
        auto* disk = dialog.addButton(tr("使用外部文件"), QMessageBox::DestructiveRole);
        dialog.addButton(QMessageBox::Cancel); dialog.exec();
        if (dialog.clickedButton() == disk) { DiscardPendingDraft(); return true; }
        if (dialog.clickedButton() != draft) return false;
        const auto current = QtProject::ReadOwnedAsset(project_, selectedPath_);
        if (!current.Succeeded() || current.value->externalLuaRoot != source_->externalLuaRoot) { Status(tr("来源已改变，请重新选择文件")); return false; }
        expected = current.value->sha256;
    }
    publishing_ = true;
    const auto problem = QtProject::WriteLuaScript(project_, selectedPath_, editor_->toPlainText(), expected, tr("编辑 Lua 脚本草稿"));
    publishing_ = false;
    if (!problem.isEmpty()) { Status(problem); return false; }
    const auto path = selectedPath_; RefreshDirectory(); (void)LoadScript(path);
    Status(tr("已应用")); emit DraftEdited(); return true;
}
void TriggerLuaScripts::DiscardPendingDraft() {
    const auto path = selectedPath_; source_.reset(); editableSource_ = false;
    if (path.isEmpty() || !LoadScript(path)) {
        selectedPath_.clear(); suppress_ = true; editor_->SetPrepareCheck({}); editor_->SetText({}); suppress_ = false;
        editor_->SetEditable(false);
    }
    RefreshDirectory();
    UpdateDraftState(); emit DraftEdited();
}

} // namespace War3Map::Modules
