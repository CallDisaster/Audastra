#include "TriggerLuaScripts.h"

#include "TriggerCodeChecking.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Workspace.h"

#include <QHBoxLayout>
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
    auto* boundary = new QLabel(tr("地图里的 .lua 文件随地图保存，保存前会检查语法（支持 YDWE 的 |xxxx| 四字码）。"
        "文件里用 require \"jass.common\" 调用 native；测试地图时修改并应用，可在工具栏“热重载”把新代码送进正在运行的游戏。"), this);
    boundary->setObjectName("luaRuntimeBoundary"); boundary->setWordWrap(true); layout->addWidget(boundary);
    auto* splitter = new QSplitter(this);
    auto* directoryPane = new QWidget(splitter); auto* directoryLayout = new QVBoxLayout(directoryPane);
    directoryLayout->setContentsMargins(0, 0, 8, 0);
    auto* buttons = new QHBoxLayout;
    const auto button = [&](const char* name, const QString& title) {
        auto* control = new QPushButton(title, directoryPane); control->setObjectName(name); buttons->addWidget(control); return control;
    };
    auto* create = button("luaNewScript", tr("新建")); auto* rename = button("luaRenameScript", tr("改名")); auto* remove = button("luaDeleteScript", tr("删除"));
    directoryLayout->addLayout(buttons);
    directory_ = new QTreeWidget(directoryPane); directory_->setObjectName("luaScriptDirectory");
    directory_->setHeaderLabels({tr("Lua 脚本"), tr("状态")}); directory_->setRootIsDecorated(false);
    directory_->setUniformRowHeights(true); directoryLayout->addWidget(directory_, 1);
    // 入口沿用 YDWE 约定：触发器“自定义代码”动作调用 Cheat("exec-lua: \"模块\"")，不另设专用入口。
    auto* entry = new QLabel(tr("游戏开始时加载：在地图初始化触发器中添加“自定义代码”动作\n"
        "call Cheat(\"exec-lua: \\\"main\\\"\")\n即加载地图根目录的 main.lua；子目录用点号，如 lua.main。\n完整示例：菜单“新建 Lua 教程地图”。"), directoryPane);
    entry->setObjectName("luaEntryHint"); entry->setWordWrap(true);
    entry->setTextInteractionFlags(Qt::TextSelectableByMouse); directoryLayout->addWidget(entry);
    auto* reveal = new QPushButton(tr("在资源中定位"), directoryPane); reveal->setObjectName("luaRevealAsset"); directoryLayout->addWidget(reveal);
    auto* codePane = new QWidget(splitter); auto* codeLayout = new QVBoxLayout(codePane); codeLayout->setContentsMargins(8, 0, 0, 0);
    sourceNote_ = new QLabel(tr("选择一个导入的 .lua 文件。"), codePane); sourceNote_->setObjectName("luaSourceNote"); sourceNote_->setWordWrap(true); codeLayout->addWidget(sourceNote_);
    editor_ = new UiKit::CodeView(codePane); editor_->setObjectName("luaCodeEditor"); editor_->SetLanguage(UiKit::CodeLanguage::Lua);
    editor_->SetDecoder(DecodeMapCodeForView); editor_->SetEditable(false); codeLayout->addWidget(editor_, 1);
    problems_ = new UiKit::CodeProblemsView(codePane); problems_->setObjectName("luaCodeProblems"); problems_->SetView(editor_); codeLayout->addWidget(problems_);
    auto* codeActions = new QHBoxLayout;
    apply_ = new QPushButton(tr("应用草稿"), codePane); apply_->setObjectName("luaApplyDraft"); codeActions->addWidget(apply_);
    discard_ = new QPushButton(tr("放弃草稿"), codePane); discard_->setObjectName("luaDiscardDraft"); codeActions->addWidget(discard_);
    auto* check = new QPushButton(tr("检查语法"), codePane); check->setObjectName("luaCheckSyntax"); codeActions->addWidget(check);
    auto* undo = new QPushButton(tr("撤销资源修改"), codePane); auto* redo = new QPushButton(tr("重做资源修改"), codePane);
    codeActions->addWidget(undo); codeActions->addWidget(redo); codeActions->addStretch(); codeLayout->addLayout(codeActions);
    status_ = new QLabel(this); status_->setObjectName("luaScriptStatus"); status_->setWordWrap(true);
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
        if (!selectedPath_.isEmpty() && QMessageBox::question(this, tr("删除 Lua 脚本"), tr("删除 %1？可通过撤销资源修改恢复。").arg(selectedPath_)) == QMessageBox::Yes) (void)DeleteScript(selectedPath_);
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
        if (publishing_ || selectedPath_.isEmpty()) return;
        if (HasPendingDraft()) { Status(tr("资源页面有修改；当前 Lua 草稿保留，应用时会重新核对文件身份。")); return; }
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
        Status(matches > 1 ? tr("资源改名后有多个同来源候选，请从目录中重新选择。") : tr("当前文件已从共同资源队列删除，可通过资源撤销恢复。")); UpdateDraftState();
    });
    connect(&project_, &QtProject::EditorProject::SourceStateChanged, this, [this] {
        if (publishing_ || !source_ || selectedPath_.isEmpty()) return;
        // 打开、恢复与工程另存的最终来源通知发生在世代和工程路径更新之后。
        // 只刷新干净视图；已有草稿保留旧身份，提交时仍必须拒绝来源替换。
        if (HasPendingDraft()) return;
        if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) return;
        (void)LoadScript(selectedPath_);
    });
    RefreshDirectory(); UpdateDraftState();
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
    if (!selected) sourceNote_->setText(tr("选择一个导入的 .lua 文件。"));
}
void TriggerLuaScripts::RefreshDirectory() {
    QSignalBlocker block(directory_); directory_->clear();
    for (const auto& entry : project_.AssetEntries()) {
        if (!entry.path.endsWith(".lua", Qt::CaseInsensitive)) continue;
        auto* item = new QTreeWidgetItem(directory_, {entry.path, entry.ambiguous ? tr("来源歧义") : entry.runtimeLuaDraft ? tr("工程草稿") : entry.Pending() ? tr("待导入") : tr("原内容")});
        item->setData(0, Qt::UserRole, entry.path); item->setToolTip(0, entry.path);
        if (QtProject::EditorProject::AssetKey(entry.path) == QtProject::EditorProject::AssetKey(selectedPath_)) directory_->setCurrentItem(item);
    }
    directory_->sortItems(0, Qt::AscendingOrder);
    if (project_.directorySnapshot_ && !project_.archiveSession_) Status(tr("目录列表来自打开时可读文件快照；扫描可能跳过无权限项，清单可能不完整。"));
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
    if (editableSource_) editor_->SetPrepareCheck([source = selectedPath_](const QString& text, std::uint64_t generation) {
            Services::CodeCheckContext context; context.complete = false;
            context.reason = "检查 Lua 5.3 语法（含 YDWE |xxxx| 四字码）；jass 模块是否可用由运行环境决定。";
            return PrepareTriggerCodeCheck(text, generation, source, UiKit::CodeLanguage::Lua, std::move(context));
        });
    else editor_->SetPrepareCheck({});
    sourceNote_->setText(editableSource_ ? tr("%1 · UTF-8；应用后在下次保存地图时写入。").arg(selectedPath_)
        : utf8 ? tr("%1 · 正文包含无法无损编辑的特殊空白字符，当前只读，原字节已保留。").arg(selectedPath_)
        : tr("%1 · 编码未声明，保留原字节只读查看；推测显示不授予转码保存。").arg(selectedPath_));
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
    if (project_.directorySnapshot_ && !project_.archiveSession_) { Status(tr("目录扫描完整性未确认，不能确认新路径没有冲突。")); return false; }
    const auto bytes = initialText.toUtf8();
    const auto problem = project_.RecordAssetEdit(tr("新建 Lua 脚本草稿"), [&](QtProject::AssetState& candidate) -> QString {
        const auto entries = project_.AssetEntries(candidate);
        if (std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return QtProject::EditorProject::AssetKey(entry.path) == QtProject::EditorProject::AssetKey(path); })) return tr("已有同名资源。");
        return QtProject::PutOwnedAsset(project_, candidate, path, Payload(bytes));
    });
    if (!problem.isEmpty()) { Status(problem); return false; }
    const bool loaded = LoadScript(path);
    RefreshDirectory();
    Status(tr("Lua 文件已加入导入队列，保存地图时写入。"));
    emit DraftEdited(); return loaded;
}
bool TriggerLuaScripts::RenameScript(const QString& path, const QString& newPath) {
    if (!CommitPendingDraft()) return false;
    if (!newPath.endsWith(".lua", Qt::CaseInsensitive)) { Status(tr("Lua 脚本必须保留 .lua 扩展名。")); return false; }
    publishing_ = true;
    const auto problem = project_.RecordAssetEdit(tr("重命名 Lua 脚本"), [&](QtProject::AssetState& candidate) { return project_.MoveAsset(candidate, path, newPath); });
    publishing_ = false;
    if (!problem.isEmpty()) { Status(problem); return false; }
    RefreshDirectory(); return LoadScript(newPath);
}
bool TriggerLuaScripts::DeleteScript(const QString& path) {
    if (!CommitPendingDraft()) return false;
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
    // 工程草稿保留未完成代码；后台语法诊断与地图写出门禁独立核对。
    const auto verified = QtProject::VerifyOwnedAsset(project_, *source_);
    if (!verified.Succeeded()) { Status(Problem(verified.diagnostics)); return false; }
    QString text = editor_->toPlainText();
    const auto& old = *source_->bytes; QByteArray oldBytes(reinterpret_cast<const char*>(old.data()), static_cast<qsizetype>(old.size()));
    const bool crlf = oldBytes.contains("\r\n") && !QByteArray(oldBytes).replace("\r\n", "").contains('\n');
    if (crlf) text.replace("\n", "\r\n");
    const auto bytes = Payload(text.toUtf8()); publishing_ = true;
    const auto problem = project_.RecordAssetEdit(tr("编辑 Lua 脚本草稿"), [&](QtProject::AssetState& candidate) { return QtProject::PutOwnedAsset(project_, candidate, selectedPath_, bytes); });
    publishing_ = false;
    if (!problem.isEmpty()) { Status(problem); return false; }
    const auto path = selectedPath_; RefreshDirectory(); (void)LoadScript(path);
    Status(tr("Lua 修改已应用，保存地图时写入；测试运行中可用“热重载”送进游戏。")); emit DraftEdited(); return true;
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
