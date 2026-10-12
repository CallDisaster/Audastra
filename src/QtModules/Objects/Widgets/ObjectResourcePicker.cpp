#include "Widgets/ObjectResourcePicker.h"
#include "Widgets/RoundedPopups.h"
#include "War3MapCore/ObjectData/ProfileList.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <exception>

namespace War3Map::Modules {
namespace {
QString U8(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QString Notes(const std::vector<Diagnostic>& notes) {
    QStringList values;
    for (const auto& note : notes) { if (values.size() >= 4) break; values.push_back(U8(note.message)); }
    return values.join(QLatin1Char('\n'));
}
}

ObjectResourcePicker::ObjectResourcePicker(Services::ObjectResourceField field, QString original,
    ObjectResourcePickerServices services, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint), field_(field), original_(std::move(original)),
      services_(std::move(services)), alive_(std::make_shared<std::atomic_bool>(true)) {
    setObjectName(QStringLiteral("objectResourcePicker"));
    setAttribute(Qt::WA_DeleteOnClose); setWindowModality(Qt::NonModal);
    UiKit::ConfigureRoundedPopup(*this);
    auto* column = new QVBoxLayout(this); column->setSpacing(8);
    // 三种来源只是切换，不需要各自的说明页；标签栏只占一行，筛选条按来源显示需要的控件。
    tabs_ = new QTabWidget(this); tabs_->setObjectName(QStringLiteral("resourceSourceTabs")); tabs_->setDocumentMode(true);
    for (const auto& label : {tr("选择"), tr("导入"), tr("自定义")}) tabs_->addTab(new QWidget(tabs_), label);
    tabs_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    tabs_->setFixedHeight(tabs_->tabBar()->sizeHint().height());
    column->addWidget(tabs_);
    filterBar_ = new QWidget(this); filterBar_->setObjectName(QStringLiteral("resourceFilterBar"));
    auto* filter = new QHBoxLayout(filterBar_); filter->setContentsMargins(0, 0, 0, 0); filter->setSpacing(6);
    browse_ = new QComboBox(filterBar_); browse_->setObjectName(QStringLiteral("resourceBrowseMode"));
    browse_->addItems({tr("物体使用的资源"), tr("游戏文件")}); filter->addWidget(browse_);
    category_ = new QComboBox(filterBar_); category_->setObjectName(QStringLiteral("resourceObjectCategory"));
    category_->addItem(tr("全部类别"), -1);
    const QStringList categories{tr("单位"), tr("技能"), tr("物品"), tr("魔法效果"), tr("科技"), tr("装饰物"), tr("可破坏物")};
    for (int i = 0; i < categories.size(); ++i) category_->addItem(categories[i], i);
    filter->addWidget(category_);
    up_ = new QPushButton(tr("上级"), filterBar_); up_->setObjectName(QStringLiteral("resourceDirectoryUp"));
    up_->setToolTip(tr("返回上级目录")); filter->addWidget(up_);
    location_ = new QLabel(filterBar_); location_->setObjectName(QStringLiteral("resourceDirectory"));
    location_->setTextFormat(Qt::PlainText); location_->setMinimumWidth(40); filter->addWidget(location_);
    search_ = new QLineEdit(filterBar_); search_->setObjectName(QStringLiteral("resourceSearch"));
    search_->setPlaceholderText(tr("搜索名称、四字码或路径")); search_->setClearButtonEnabled(true);
    filter->addWidget(search_, 1); column->addWidget(filterBar_);
    auto* body = new QHBoxLayout; body->setSpacing(8);
    candidates_ = new QTreeWidget(this); candidates_->setObjectName(QStringLiteral("resourceCandidates"));
    candidates_->setColumnCount(3); candidates_->setHeaderLabels({tr("名称"), tr("资源"), tr("来源")});
    candidates_->setRootIsDecorated(false); candidates_->setMouseTracking(true); candidates_->setMinimumWidth(220);
    candidates_->header()->setSectionResizeMode(QHeaderView::Interactive);
    candidates_->header()->setStretchLastSection(true);
    candidates_->setColumnWidth(0, 130); candidates_->setColumnWidth(1, 250);
    body->addWidget(candidates_, 1);
    if (services_.preview) preview_ = services_.preview(this);
    if (preview_.widget) { preview_.widget->setFixedWidth(240); body->addWidget(preview_.widget); }
    column->addLayout(body, 1);
    auto* statusRow = new QHBoxLayout;
    status_ = new QLabel(this); status_->setObjectName(QStringLiteral("resourceStatus")); status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText); statusRow->addWidget(status_, 1);
    more_ = new QPushButton(tr("加载更多"), this); more_->setObjectName(QStringLiteral("resourceMore")); statusRow->addWidget(more_);
    column->addLayout(statusRow);
    if (field_.orderedList) {
        auto* listRow = new QHBoxLayout;
        selected_ = new QListWidget(this); selected_->setObjectName(QStringLiteral("resourceOrderedValues"));
        selected_->setMaximumHeight(115);
        if (!original_.isEmpty() && original_ != QStringLiteral("_")) {
            for (const auto& token : ObjectData::SlkText::SplitProfileList(original_.toStdString()))
                originalItems_.push_back(U8(ObjectData::SlkText::DecodeProfileItem(token)));
            selected_->addItems(originalItems_);
        }
        listRow->addWidget(selected_, 1);
        auto* commands = new QVBoxLayout;
        const auto addButton = [&](const QString& name, const QString& label, auto callback) {
            auto* button = new QPushButton(label, this); button->setObjectName(name); commands->addWidget(button);
            connect(button, &QPushButton::clicked, this, callback);
        };
        addButton(QStringLiteral("resourceReplace"), tr("替换选中"), [this] { replacing_ = true; AddOrReplace(); });
        addButton(QStringLiteral("resourceRemove"), tr("删除"), [this] { delete selected_->takeItem(selected_->currentRow()); });
        addButton(QStringLiteral("resourceMoveUp"), tr("上移"), [this] { MoveSelected(-1); });
        addButton(QStringLiteral("resourceMoveDown"), tr("下移"), [this] { MoveSelected(1); });
        listRow->addLayout(commands); column->addLayout(listRow);
    }
    auto* pathRow = new QHBoxLayout;
    path_ = new QLineEdit(this); path_->setObjectName(QStringLiteral("resourcePath"));
    path_->setText(field_.orderedList ? QString{} : original_); path_->setClearButtonEnabled(true);
    path_->setPlaceholderText(field_.storesPath ? tr("地图或游戏中的资源路径") : tr("原版声音标签或组名"));
    if (selected_) connect(selected_, &QListWidget::currentTextChanged, this, [this](const QString& value) { path_->setText(value); ShowPreview(value); });
    pathRow->addWidget(path_, 1);
    if (field_.orderedList) {
        auto* add = new QPushButton(tr("添加"), this); add->setObjectName(QStringLiteral("resourceAdd")); pathRow->addWidget(add);
        connect(add, &QPushButton::clicked, this, [this] { replacing_ = false; AddOrReplace(); });
    }
    column->addLayout(pathRow);
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this); column->addWidget(actions);
    connect(actions->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &ObjectResourcePicker::Apply);
    connect(actions, &QDialogButtonBox::rejected, this, &QWidget::close);
    connect(tabs_, &QTabWidget::currentChanged, this, &ObjectResourcePicker::SelectSource);
    connect(browse_, &QComboBox::currentIndexChanged, this, &ObjectResourcePicker::SelectSource);
    connect(category_, &QComboBox::currentIndexChanged, this, &ObjectResourcePicker::Requery);
    auto* searchDelay = new QTimer(this); searchDelay->setSingleShot(true); searchDelay->setInterval(100);
    connect(search_, &QLineEdit::textChanged, searchDelay, qOverload<>(&QTimer::start));
    connect(searchDelay, &QTimer::timeout, this, &ObjectResourcePicker::Requery);
    connect(more_, &QPushButton::clicked, this, &ObjectResourcePicker::AppendPage);
    connect(up_, &QPushButton::clicked, this, [this] {
        const auto slash = directory_.lastIndexOf('/'); directory_ = slash < 0 ? QString{} : directory_.left(slash); Requery();
    });
    connect(candidates_, &QTreeWidget::itemClicked, this, [this](auto* item) { Choose(item, false); });
    connect(candidates_, &QTreeWidget::itemActivated, this, [this](auto* item) { Choose(item, true); });
    auto* hoverDelay = new QTimer(this); hoverDelay->setSingleShot(true); hoverDelay->setInterval(140);
    connect(candidates_, &QTreeWidget::itemEntered, this, [hoverDelay](auto* item) {
        if (item->data(0, Qt::UserRole + 1).toBool()) return;
        hoverDelay->setProperty("resourceValue", item->data(0, Qt::UserRole)); hoverDelay->start();
    });
    connect(hoverDelay, &QTimer::timeout, this, [this, hoverDelay] { ShowPreview(hoverDelay->property("resourceValue").toString()); });
    connect(path_, &QLineEdit::textEdited, this, [this](const QString& value) { ShowPreview(value); });
    connect(path_, &QLineEdit::returnPressed, this, [this] { if (field_.orderedList) { replacing_ = false; AddOrReplace(); } else Apply(); });
    resize(820, field_.orderedList ? 600 : 475);
    QTimer::singleShot(0, this, [this] { SelectSource(); if (!field_.orderedList) ShowPreview(original_); });
}
ObjectResourcePicker::~ObjectResourcePicker() { alive_->store(false); ++request_; }
void ObjectResourcePicker::SetCommit(std::function<bool(const QString&)> commit) { commit_ = std::move(commit); }
QString ObjectResourcePicker::Value() const {
    if (!selected_) return path_->text();
    QStringList values;
    for (int i = 0; i < selected_->count(); ++i) values.push_back(selected_->item(i)->text());
    if (values == originalItems_) return original_;
    std::vector<std::string> tokens;
    for (const auto& value : values) tokens.push_back(ObjectData::SlkText::ProfileItemToken(value.toStdString()).value_or(value.toStdString()));
    return U8(ObjectData::SlkText::JoinProfileList(tokens));
}
void ObjectResourcePicker::SelectSource() {
    const bool custom = tabs_->currentIndex() == 2;
    candidates_->setVisible(!custom); filterBar_->setVisible(!custom); more_->hide();
    origin_ = tabs_->currentIndex() == 1 ? Services::ObjectResourceOrigin::Import
        : browse_->currentIndex() == 1 ? Services::ObjectResourceOrigin::Game : Services::ObjectResourceOrigin::Object;
    browse_->setVisible(tabs_->currentIndex() == 0);
    category_->setVisible(tabs_->currentIndex() == 0 && origin_ == Services::ObjectResourceOrigin::Object);
    const bool directories = origin_ != Services::ObjectResourceOrigin::Object && field_.storesPath;
    up_->setVisible(directories); location_->setVisible(directories);
    if (preview_.widget) {
        // 自定义页没有候选列表，预览占满主体；其他页预览固定在右侧。
        if (custom) { preview_.widget->setMinimumWidth(240); preview_.widget->setMaximumWidth(QWIDGETSIZE_MAX); }
        else preview_.widget->setFixedWidth(240);
    }
    directory_.clear(); catalog_.reset(); candidates_->clear();
    if (custom) { status_->clear(); path_->setFocus(); return; }
    if (origin_ == Services::ObjectResourceOrigin::Import && !field_.storesPath) {
        status_->setText(tr("原版字段保存声音标签或组名，不接受文件路径。可在“选择”中选已有标签。")); return;
    }
    if (const auto cached = catalogs_[static_cast<int>(origin_)]) { catalog_ = cached; Requery(); return; }
    StartLoad(origin_);
}
void ObjectResourcePicker::StartLoad(Services::ObjectResourceOrigin origin) {
    if (worker_) return;
    if (!services_.load || (services_.isCurrent && !services_.isCurrent())) { status_->setText(tr("来源已变化，请重新打开资源选择器。")); return; }
    status_->setText(tr("正在建立资源索引…"));
    auto result = std::make_shared<Result<std::shared_ptr<const Services::ObjectResourceCatalog>>>();
    const auto serial = ++request_;
    const auto examined = std::make_shared<std::atomic_size_t>(0);
    auto* progressTimer = new QTimer(this); progressTimer->setInterval(100);
    connect(progressTimer, &QTimer::timeout, this, [this, examined] {
        if (tabs_->currentIndex() != 2)
            status_->setText(tr("正在建立资源索引… 已检查 %1 项").arg(examined->load()));
    });
    auto* worker = QThread::create([loader = services_.load, origin, result, alive = alive_, examined] {
        try {
            *result = loader(origin, [alive, examined](std::size_t count) {
                if (!alive->load()) return false;
                examined->store(count);
                return true;
            });
        } catch (const std::exception& error) {
            *result = Result<std::shared_ptr<const Services::ObjectResourceCatalog>>::Failure("Object.Resource", "OBJECT_RESOURCE_INDEX_EXCEPTION", error.what());
        }
    });
    worker_ = worker;
    connect(worker, &QThread::finished, this, [this, result, origin, serial, progressTimer] {
        progressTimer->stop();
        worker_ = nullptr;
        if (serial != request_ || (services_.isCurrent && !services_.isCurrent())) { status_->setText(tr("来源已变化，请重新打开资源选择器。")); return; }
        if (result->Succeeded()) catalogs_[static_cast<int>(origin)] = *result->value;
        if (tabs_->currentIndex() == 2) return;
        if (origin != origin_) { SelectSource(); return; }
        if (!result->Succeeded()) { status_->setText(Notes(result->diagnostics)); return; }
        catalog_ = *result->value; Requery();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, progressTimer, &QObject::deleteLater);
    progressTimer->start();
    worker->start(QThread::LowPriority);
}
void ObjectResourcePicker::Requery() {
    candidates_->clear(); cursor_.reset(); location_->setText(directory_.isEmpty() ? tr("根目录") : directory_);
    up_->setEnabled(!directory_.isEmpty());
    if (catalog_) AppendPage();
}
void ObjectResourcePicker::AppendPage() {
    if (!catalog_) return;
    if (services_.isCurrent && !services_.isCurrent()) { status_->setText(tr("来源已变化，请重新打开资源选择器。")); more_->hide(); return; }
    Services::ObjectResourceQuery query;
    query.expectedRevision = services_.revision; query.kind = field_.kind; query.search = search_->text().toStdString();
    if (origin_ == Services::ObjectResourceOrigin::Object && category_->currentData().toInt() >= 0)
        query.objectType = static_cast<ObjectData::ObjectType>(category_->currentData().toInt());
    if (origin_ != Services::ObjectResourceOrigin::Object && search_->text().isEmpty()
        && field_.storesPath) query.directory = directory_.toStdString();
    auto page = Services::QueryObjectResources(*catalog_, query, cursor_);
    if (!page.Succeeded()) { status_->setText(Notes(page.diagnostics)); more_->hide(); return; }
    for (const auto& row : page.value->rows) {
        auto* item = new QTreeWidgetItem(candidates_);
        item->setText(0, row.directory ? tr("目录 · %1").arg(U8(row.title)) : U8(row.title));
        item->setText(1, U8(row.value));
        // 正常行只写来源；缺失、歧义等需要作者注意的状态才追加。
        const auto state = row.state == "missing" ? tr("缺失") : row.state == "unresolved" ? tr("来源歧义")
            : row.state == "oversized" ? tr("文件过大，未核对") : QString{};
        const auto source = ObjectResourceSourceText(row.sourceId);
        item->setText(2, state.isEmpty() ? source : source + QStringLiteral(" · ") + state);
        item->setData(0, Qt::UserRole, U8(row.value)); item->setData(0, Qt::UserRole + 1, row.directory);
        const auto tip = U8(row.title) + QStringLiteral("\n") + U8(row.value) + QStringLiteral("\n") + item->text(2);
        for (int column = 0; column < 3; ++column) item->setToolTip(column, tip);
    }
    cursor_ = page.value->next; more_->setVisible(cursor_.has_value());
    status_->setText(tr("已显示 %1 项%2").arg(candidates_->topLevelItemCount())
        .arg(page.value->complete ? QString{} : tr(" · 目录或结果尚未全部列出")));
    status_->setToolTip(Notes(page.value->diagnostics));
    if (!page.value->diagnostics.empty()) status_->setText(status_->text() + QStringLiteral("\n") + U8(page.value->diagnostics.front().message));
}
void ObjectResourcePicker::Choose(QTreeWidgetItem* item, bool activate) {
    if (!item) return;
    const auto value = item->data(0, Qt::UserRole).toString();
    if (item->data(0, Qt::UserRole + 1).toBool()) {
        if (activate) { directory_ = value; Requery(); }
        return;
    }
    path_->setText(value); ShowPreview(value);
    if (activate && field_.orderedList) { replacing_ = false; AddOrReplace(); }
}
void ObjectResourcePicker::ShowPreview(const QString& value) {
    if (preview_.show && (!services_.isCurrent || services_.isCurrent())) preview_.show(value, field_.kind);
}
void ObjectResourcePicker::AddOrReplace() {
    if (!selected_ || path_->text().isEmpty()) return;
    if (!ObjectData::SlkText::ProfileItemToken(path_->text().toStdString())) {
        status_->setText(tr("列表项包含无法安全写入的引号、换行或首尾空白。")); return;
    }
    if (replacing_ && selected_->currentItem()) selected_->currentItem()->setText(path_->text());
    else { selected_->addItem(path_->text()); selected_->setCurrentRow(selected_->count() - 1); }
    replacing_ = false;
}
void ObjectResourcePicker::MoveSelected(int delta) {
    const auto row = selected_->currentRow(), next = row + delta;
    if (row < 0 || next < 0 || next >= selected_->count()) return;
    auto* item = selected_->takeItem(row); selected_->insertItem(next, item); selected_->setCurrentRow(next);
}
void ObjectResourcePicker::Apply() {
    const auto value = Value();
    if (value == original_) { close(); return; }
    if (services_.isCurrent && !services_.isCurrent()) { status_->setText(tr("工程已变化，草稿未写入。请重新打开资源选择器。")); return; }
    const QPointer<ObjectResourcePicker> self(this);
    const auto commit = commit_;
    const bool accepted = commit && commit(value);
    if (!self) return;
    if (accepted) { emit ValueCommitted(value); if (self) close(); }
    else status_->setText(tr("字段未写入；当前值或字段结构无法安全编辑。"));
}
QString ObjectResourceSourceText(std::string_view sourceId) {
    const auto starts = [sourceId](std::string_view prefix) { return sourceId.substr(0, prefix.size()) == prefix; };
    if (starts("world-map-") || sourceId == "map-archive") return QObject::tr("当前地图");
    if (sourceId == "map-directory") return QObject::tr("地图目录");
    if (sourceId == "session-queue") return QObject::tr("待导入");
    if (sourceId == "map-object") return QObject::tr("地图物体");
    if (sourceId == "standard-object") return QObject::tr("原版物体");
    if (starts("world-game")) return QObject::tr("游戏资源");
    // 游戏文件行是所在 MPQ 名，声音标签行是“MPQ:表路径”；只显示 MPQ 名。
    const auto colon = sourceId.find(':');
    const auto archive = colon == std::string_view::npos ? sourceId : sourceId.substr(0, colon);
    return archive.empty() ? QObject::tr("未知来源") : QObject::tr("游戏 · %1").arg(U8(archive));
}
} // namespace War3Map::Modules
