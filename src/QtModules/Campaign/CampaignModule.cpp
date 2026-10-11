#include "CampaignModule.h"
#include "AssetsModule.h"
#include "ObjectModule.h"
#include "EditorProject.h"
#include "MapSavePreferences.h"
#include "Modules/ModuleHost.h"
#include "Icons/SvgIcons.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::Modules {
namespace {
QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QString Path(const std::filesystem::path& path) { return QString::fromStdWString(path.wstring()); }
}
class CampaignHistoryCommand final : public QUndoCommand {
public:
    explicit CampaignHistoryCommand(CampaignModule& module) : QUndoCommand(QObject::tr("编辑战役")), module_(module) {}
    void undo() override { if (!module_.rebuildingHistory_) module_.Replay(false); }
    void redo() override { if (!module_.rebuildingHistory_) module_.Replay(true); }
private:
    CampaignModule& module_;
};
class CampaignScopeHost final : public Shell::ModuleHost {
public:
    explicit CampaignScopeHost(CampaignModule& module) : module_(module) {}
    void Navigate(const Shell::NavigationTarget& target) override {
        ActivateModule(target.module);
        if (target.module == "objects") (void)module_.objects_->Reveal(target);
        else if (target.module == "assets") (void)module_.assets_->Reveal(target);
    }
    void ActivateModule(const Shell::ModuleId& id) override {
        if (!module_.tabs_) return;
        if (id == "objects") module_.tabs_->setCurrentIndex(2);
        else if (id == "assets") module_.tabs_->setCurrentIndex(3);
        else module_.Notice(QObject::tr("战役共享数据的跨模块引用定位尚未提供。"));
    }
    void ShowStatusMessage(const QString& text, int) override { module_.Notice(text); }
    void UndoStackChanged(const Shell::ModuleId&) override {
        if (module_.host_) module_.host_->UndoStackChanged("campaign");
    }
    std::uint64_t NavigationEpoch() const override { return module_.host_ ? module_.host_->NavigationEpoch() : 0; }
private:
    CampaignModule& module_;
};

CampaignModule::CampaignModule(QtProject::EditorProject& mainProject)
    : mainProject_(mainProject), shared_(std::make_unique<QtProject::EditorProject>()),
      scopeHost_(std::make_unique<CampaignScopeHost>(*this)),
      history_(new QUndoStack(this)) {
    for (auto& stack : shared_->objectUndoStacks_) stack = new QUndoStack(shared_.get());
    objects_ = std::make_unique<ObjectModule>(*shared_);
    assets_ = std::make_unique<AssetsModule>(*shared_);
    objects_->AttachHost(scopeHost_.get()); assets_->AttachHost(scopeHost_.get());
    const auto changed = [this]() { if (!refreshing_) { emit Changed(); if (host_) host_->UndoStackChanged("campaign"); } };
    connect(objects_.get(), &ObjectModule::DocumentEdited, this, changed);
    connect(objects_.get(), &ObjectModule::DocumentsChanged, this, changed);
    connect(shared_.get(), &QtProject::EditorProject::ImportsChanged, this, changed);
    for (auto* stack : shared_->objectUndoStacks_) connect(stack, &QUndoStack::indexChanged, this, changed);
    connect(shared_->AssetUndoStack(), &QUndoStack::indexChanged, this, changed);
}
CampaignModule::~CampaignModule() {
    // 子模块先收回其控件，避免外层页销毁后悬挂的物编模型再收到信号。
    objects_.reset(); assets_.reset(); delete page_.data();
}
Shell::ModuleInfo CampaignModule::Info() const { return {"campaign", tr("战役"), "lucide/flag", QKeySequence(Qt::ALT | Qt::Key_7)}; }
void CampaignModule::AttachHost(Shell::ModuleHost* host) { host_ = host; }
QWidget* CampaignModule::Widget() { if (!page_) BuildPage(); return page_; }
QtProject::EditorProject& CampaignModule::SharedProject() { return *shared_; }
bool CampaignModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "campaign") return false;
    (void)Widget();
    if (target.kind == "menu") tabs_->setCurrentIndex(1);
    else if (target.kind == "shared") tabs_->setCurrentIndex(2);
    else if (target.kind == "resources") tabs_->setCurrentIndex(3);
    else if (target.kind == "settings") tabs_->setCurrentIndex(4);
    else tabs_->setCurrentIndex(0);
    return true;
}
QJsonObject CampaignModule::SaveViewState() const { return {{"tab", tabs_ ? tabs_->currentIndex() : 0}}; }
void CampaignModule::RestoreViewState(const QJsonObject& state) {
    (void)Widget(); const auto tab = state["tab"].toInt(); if (tab >= 0 && tab < 5) tabs_->setCurrentIndex(tab);
}
void CampaignModule::Notice(const QString& text) { if (summary_) summary_->setText(text); if (host_) host_->ShowStatusMessage(text, 6000); }
bool CampaignModule::Failed(const std::vector<Diagnostic>& diagnostics) {
    QStringList lines;
    for (const auto& diagnostic : diagnostics) lines << Text(diagnostic.code) + ": " + Text(diagnostic.message);
    lastError_ = lines.join('\n'); Notice(lastError_); return false;
}

void CampaignModule::BuildPage() {
    page_ = new QWidget; page_->setObjectName("campaignWorkspace");
    auto* layout = new QVBoxLayout(page_); layout->setContentsMargins(12, 12, 12, 12);
    auto* toolbar = new QHBoxLayout;
    const auto action = [&](const QString& text, const char* name, const auto& handler) {
        auto* button = new QPushButton(text, page_); button->setObjectName(name); toolbar->addWidget(button);
        connect(button, &QPushButton::clicked, this, handler); return button;
    };
    action(tr("新建战役"), "newCampaign", [this] { (void)New(); });
    action(tr("打开战役…"), "openCampaign", [this] {
        const auto path = QFileDialog::getOpenFileName(page_, tr("打开战役"), {}, tr("Warcraft III 战役 (*.w3n)"));
        if (!path.isEmpty()) (void)Open(std::filesystem::path(path.toStdWString()));
    });
    action(tr("保存"), "saveCampaign", [this] { (void)Save(); });
    action(tr("另存为…"), "saveCampaignAs", [this] { (void)Save(true); });
    toolbar->addStretch();
    action(tr("应用共享修改"), "applyCampaignShared", [this] { (void)FlushDraft(); });
    action(tr("放弃共享草稿"), "discardCampaignShared", [this] { ReloadShared(); RefreshSettings(); settingsDirty_ = false; emit Changed(); });
    layout->addLayout(toolbar);
    summary_ = new QLabel(tr("未打开战役"), page_); summary_->setWordWrap(true); summary_->setObjectName("campaignStatus"); layout->addWidget(summary_);
    tabs_ = new QTabWidget(page_); tabs_->setObjectName("campaignTabs"); layout->addWidget(tabs_, 1);
    for (bool menu : {false, true}) {
        auto* container = new QWidget(tabs_); auto* area = new QVBoxLayout(container); area->setContentsMargins(0, 8, 0, 0);
        auto* row = new QHBoxLayout;
        const auto add = [&](const QString& text, const char* name, const auto& handler) {
            auto* control = new QPushButton(text, container); control->setObjectName(name); row->addWidget(control);
            connect(control, &QPushButton::clicked, this, handler);
        };
        add(menu ? tr("新增菜单按钮") : tr("添加地图…"), menu ? "addCampaignButton" : "addCampaignMap", [this, menu] { if (menu) EditButton(true); else AddMap(false); });
        add(menu ? tr("编辑按钮") : tr("替换地图…"), menu ? "editCampaignButton" : "replaceCampaignMap", [this, menu] { if (menu) EditButton(false); else AddMap(true); });
        add(tr("移除"), menu ? "removeCampaignButton" : "removeCampaignMap", [this, menu] { RemoveSelection(menu); });
        add(tr("上移"), menu ? "upCampaignButton" : "upCampaignMap", [this, menu] { MoveSelection(menu, -1); });
        add(tr("下移"), menu ? "downCampaignButton" : "downCampaignMap", [this, menu] { MoveSelection(menu, 1); });
        if (!menu) {
            add(tr("打开章节"), "openCampaignChapter", [this] { (void)OpenChapter(SelectedId(false)); });
            add(tr("更新已打开章节"), "updateCampaignChapter", [this] { (void)UpdateOpenedChapter(); });
            add(tr("导出…"), "exportCampaignMap", [this] { ExportSelection(); });
        }
        row->addStretch(); area->addLayout(row);
        auto* table = new QTableWidget(container); table->setObjectName(menu ? "campaignButtons" : "campaignMaps");
        table->setColumnCount(menu ? 5 : 3);
        table->setHorizontalHeaderLabels(menu ? QStringList{tr("按钮身份"), tr("章节标题"), tr("关卡标题"), tr("地图路径"), tr("初始可见")}
            : QStringList{tr("地图身份"), tr("地图路径"), tr("菜单入口数")});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true); table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection); table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        area->addWidget(table); tabs_->addTab(container, menu ? tr("菜单") : tr("章节"));
        if (menu) buttons_ = table; else maps_ = table;
        connect(table, &QTableWidget::cellDoubleClicked, this, [this, menu](int, int) { if (menu) EditButton(false); else (void)OpenChapter(SelectedId(false)); });
    }
    const auto scopedPage = [&](QWidget* contents) {
        auto* container = new QWidget(tabs_); auto* area = new QVBoxLayout(container);
        auto* label = new QLabel(tr("作用域：战役共享；修改写入当前战役，不写入章节地图。"), container);
        label->setObjectName("campaignScopeLabel"); area->addWidget(label); area->addWidget(contents); return container;
    };
    tabs_->addTab(scopedPage(objects_->Widget()), tr("共享数据"));
    tabs_->addTab(scopedPage(assets_->Widget()), tr("资源"));
    tabs_->addTab(BuildSettings(), tr("设置"));
    connect(tabs_, &QTabWidget::currentChanged, this, [this] { if (host_) host_->UndoStackChanged("campaign"); });
    RefreshFromService();
}

void CampaignModule::SetSession(std::shared_ptr<Services::CampaignSession> session) {
    session_ = std::move(session); openedChapterId_ = 0; openedChapterFile_.clear();
    chapterSavedPending_ = false; openedChapterSourceSha_.clear();
    // 已打开章节可能仍由主地图工程使用，不在切换战役时提前删除临时成员。
    shared_->campaignScope_ = session_; settingsDirty_ = false;
    RefreshFromService(); emit SessionChanged(); emit Changed();
}
bool CampaignModule::New() {
    if (!ConfirmClose(tr("新建战役"))) return false;
    SetSession(Services::CampaignSession::Create()); return true;
}
bool CampaignModule::Open(const std::filesystem::path& path) {
    auto opened = Services::CampaignSession::Open(path);
    if (!opened.Succeeded() || !opened.value) return Failed(opened.diagnostics);
    if (!ConfirmClose(tr("打开战役"))) return false;
    SetSession(*opened.value); return true;
}
void CampaignModule::RefreshTables() {
    if (!maps_ || !buttons_) return;
    maps_->setRowCount(0); buttons_->setRowCount(0);
    if (!session_) return;
    const auto& info = session_->Info();
    const auto resolved = [&](const std::string& text) { const auto value = session_->ResolveText(text); return Text(value.value.value_or(text)); };
    for (const auto& map : info.maps) {
        const int row = maps_->rowCount(); maps_->insertRow(row);
        const auto count = std::count_if(info.buttons.begin(), info.buttons.end(), [&](const auto& button) { return QtProject::EditorProject::AssetKey(Text(button.mapPath)) == QtProject::EditorProject::AssetKey(Text(map.path)); });
        maps_->setItem(row, 0, new QTableWidgetItem(QString::number(map.id))); maps_->setItem(row, 1, new QTableWidgetItem(Text(map.path)));
        maps_->setItem(row, 2, new QTableWidgetItem(QString::number(count)));
    }
    for (const auto& button : info.buttons) {
        const int row = buttons_->rowCount(); buttons_->insertRow(row);
        const QStringList columns{QString::number(button.id), resolved(button.chapterTitle), resolved(button.mapTitle), Text(button.mapPath), button.visible ? tr("是") : tr("否")};
        for (int column = 0; column < columns.size(); ++column) buttons_->setItem(row, column, new QTableWidgetItem(columns[column]));
    }
}
void CampaignModule::RefreshFromService() {
    if (session_) displayedRevision_ = session_->Revision();
    if (!page_) return;
    const QScopedValueRollback guard(refreshing_, true);
    tabs_->setEnabled(session_ != nullptr); RefreshTables(); RefreshSettings(); ReloadShared(); RebuildHistory();
    if (session_) Notice(tr("%1 · %2 张地图 · %3 个菜单按钮%4").arg(Text(session_->Info().name)).arg(session_->Info().maps.size())
        .arg(session_->Info().buttons.size()).arg(session_->Dirty() ? tr(" · 未保存") : QString{}));
}
StableId CampaignModule::SelectedId(bool menu) const {
    const auto table = menu ? buttons_ : maps_;
    if (!table || table->currentRow() < 0 || !table->item(table->currentRow(), 0)) return 0;
    return table->item(table->currentRow(), 0)->text().toULongLong();
}
bool CampaignModule::ApplyCommand(const Services::CampaignCommand& command) {
    if (!session_) { Notice(tr("请先新建或打开战役。")); return false; }
    if (!FlushDraft()) return false;
    const auto applied = session_->Apply(displayedRevision_, command);
    if (!applied.Succeeded()) return Failed(applied.diagnostics);
    RefreshFromService(); emit Changed(); return true;
}
void CampaignModule::RebuildHistory() {
    const QScopedValueRollback guard(rebuildingHistory_, true);
    history_->clear();
    if (session_) {
        for (std::size_t i = 0; i < session_->HistorySize(); ++i) history_->push(new CampaignHistoryCommand(*this));
        history_->setIndex(static_cast<int>(session_->HistoryPosition()));
    }
    if (host_) host_->UndoStackChanged("campaign");
}
QUndoStack* CampaignModule::UndoStack() {
    if (tabs_ && tabs_->currentIndex() == 2) {
        auto* stack = objects_->UndoStack(); if (stack && (stack->canUndo() || stack->canRedo())) return stack;
    }
    if (tabs_ && tabs_->currentIndex() == 3 && (shared_->AssetUndoStack()->canUndo() || shared_->AssetUndoStack()->canRedo())) return shared_->AssetUndoStack();
    return history_;
}
void CampaignModule::Replay(bool redo) {
    if (!session_ || HasDraft()) { Notice(tr("请先应用或放弃当前共享/设置草稿，再撤销战役事务。")); QTimer::singleShot(0, this, &CampaignModule::RebuildHistory); return; }
    const auto result = redo ? session_->Redo(displayedRevision_) : session_->Undo(displayedRevision_);
    if (!result.Succeeded()) { (void)Failed(result.diagnostics); QTimer::singleShot(0, this, &CampaignModule::RebuildHistory); return; }
    displayedRevision_ = session_->Revision(); RefreshTables(); RefreshSettings(); ReloadShared(); emit Changed();
}
bool CampaignModule::PrepareNavigationLeave() { return FlushDraft(); }
bool CampaignModule::Dirty() const { return (session_ && session_->Dirty()) || HasDraft() || HasUncommittedChapter(); }
bool CampaignModule::HasDraft() const {
    return settingsDirty_ || shared_->HasPendingAssetEdits() || std::any_of(shared_->objectDirty_.begin(), shared_->objectDirty_.end(), [](bool value) { return value; });
}
bool CampaignModule::FlushDraft() {
    if (!session_) return true;
    if (session_->Revision() != displayedRevision_ && HasDraft()) { Notice(tr("战役已被其他入口修改，当前草稿保留。请导出或放弃草稿后重新载入。")); return false; }
    if (!CommitSettings() || !CommitShared()) return false;
    return true;
}
bool CampaignModule::ConfirmClose(const QString& action, bool allowDiscard) {
    if (!Dirty()) return true;
    QMessageBox prompt(QMessageBox::Warning, tr("战役尚未保存"), HasUncommittedChapter()
        ? tr("章节副本尚未更新到战役。请先保存章节；选择保存战役会先把章节副本更新到战役，再%1。").arg(action)
        : tr("保存当前战役后%1？").arg(action), QMessageBox::NoButton, page_);
    auto* save = prompt.addButton(tr("保存战役"), QMessageBox::AcceptRole);
    auto* discard = allowDiscard ? prompt.addButton(tr("放弃战役修改"), QMessageBox::DestructiveRole) : nullptr;
    auto* cancel = prompt.addButton(tr("继续编辑"), QMessageBox::RejectRole); prompt.setDefaultButton(qobject_cast<QPushButton*>(cancel)); prompt.exec();
    if (prompt.clickedButton() == save) return (!HasUncommittedChapter() || UpdateOpenedChapter()) && Save();
    if (discard && prompt.clickedButton() == discard) return true;
    return false;
}
bool CampaignModule::Save(bool saveAs) {
    if (!session_) { Notice(tr("没有可保存的战役。")); return false; }
    if (saveAs || session_->SourcePath().empty()) {
        const auto target = QFileDialog::getSaveFileName(page_, tr("另存战役为"), Path(session_->SourcePath()), tr("Warcraft III 战役 (*.w3n)"));
        if (target.isEmpty()) return false;
        return SaveTo(std::filesystem::path(target.toStdWString()));
    }
    return SaveTo({}, true);
}
bool CampaignModule::SaveTo(const std::filesystem::path& target, bool inPlace) {
    if (HasUncommittedChapter()) { Notice(tr("章节副本的修改尚未更新到战役。请先保存章节，再点击“更新已打开章节”；本次未保存战役。")); return false; }
    if (!session_ || !FlushDraft()) return false;
    auto backup = Result<InPlaceSaveOptions>::Success({});
    if (inPlace) backup = QtProject::ReadMapInPlaceSaveOptions();
    if (!backup.Succeeded() || !backup.value) return Failed(backup.diagnostics);
    const auto saved = session_->Save(displayedRevision_, target, inPlace, *backup.value);
    if (!saved.Succeeded() || !saved.value) return Failed(saved.diagnostics);
    RefreshFromService(); emit Changed();
    Notice(tr("战役已保存：%1%2").arg(Path(saved.value->written.path), saved.value->backup.empty() ? QString{} : tr("；原文件备份：%1").arg(Path(saved.value->backup))));
    return true;
}
} // namespace War3Map::Modules
