#include "ManagerModule.h"
#include "../World/WorldNavigationBookmark.h"
#include <QScrollBar>
#include <QSignalBlocker>
#include <QCheckBox>

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "Widgets/ColumnLayout.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <limits>
#include <set>

namespace War3Map::Modules {
namespace {

using Manager::Page;

constexpr int kRowRole = Qt::UserRole + 1;
constexpr int kNumberRole = Qt::UserRole + 2;
constexpr int kTargetRole = Qt::UserRole + 3;
// 单个分组最多渲染的引用条数；函数可能被调用上万次，逐条生成自然语言会拖慢界面。
constexpr std::size_t kMaxRenderedUsages = 500;
constexpr std::size_t kNavigationRows = 4096;
constexpr std::size_t kNavigationTextBytes = 1024 * 1024;

bool NavigationText(const std::string& text, std::size_t& bytes) {
    if (text.size() > 64 * 1024 || text.size() > kNavigationTextBytes - bytes
        || text.find('\0') != std::string::npos) return false;
    bytes += text.size();
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())).toUtf8().toStdString() == text;
}

bool SourceSha(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    });
}

// 数值列按数值排序，其余按文本。
class RowItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override {
        const int column = treeWidget() != nullptr ? treeWidget()->sortColumn() : 0;
        const QVariant left = data(column, kNumberRole);
        const QVariant right = other.data(column, kNumberRole);
        if (left.isValid() && right.isValid()) return left.toLongLong() < right.toLongLong();
        return text(column).localeAwareCompare(other.text(column)) < 0;
    }
};

std::optional<Page> PageOfKind(const QString& kind) {
    static const std::map<QString, Page> kPages{{"unit", Page::Unit}, {"item", Page::Item},
        {"destructable", Page::Destructable}, {"doodad", Page::Doodad}, {"ability", Page::Ability},
        {"buff", Page::Buff}, {"upgrade", Page::Upgrade}};
    const auto found = kPages.find(kind.toLower());
    return found == kPages.end() ? std::nullopt : std::optional<Page>(found->second);
}

} // namespace

ManagerModule::ManagerModule(QtProject::EditorProject& project) : project_(project) {
    using Signal = void (QtProject::EditorProject::*)();
    for (const Signal signal : {&QtProject::EditorProject::TriggerDocumentChanged,
             &QtProject::EditorProject::MapDocumentChanged, &QtProject::EditorProject::ImportsChanged,
             &QtProject::EditorProject::ObjectDocumentsReplaced, &QtProject::EditorProject::WorldEditStringsChanged}) {
        connect(&project_, signal, this, [this]() {
            rowsGeneration_ = std::numeric_limits<std::uint64_t>::max();
            Refresh();
        });
    }
    const auto refreshNames = [this] {
        rowsGeneration_ = std::numeric_limits<std::uint64_t>::max();
        Refresh();
    };
    connect(&project_, &QtProject::EditorProject::UiLibrariesChanged, this, refreshNames);
    for (auto* stack : project_.objectUndoStacks_) if (stack) connect(stack, &QUndoStack::indexChanged, this, refreshNames);
    if (project_.triggerUndoStack_) connect(project_.triggerUndoStack_, &QUndoStack::indexChanged, this, refreshNames);
}

ManagerModule::~ManagerModule() {
    delete page_.data();
}

Shell::ModuleInfo ManagerModule::Info() const {
    return {"manager", tr("物体管理"), "lucide/list-tree", QKeySequence(Qt::ALT | Qt::Key_6)};
}

void ManagerModule::AttachHost(Shell::ModuleHost* host) {
    host_ = host;
}

QWidget* ManagerModule::Widget() {
    if (!page_) {
        BuildPage();
        ShowPage(current_);
    }
    return page_;
}

void ManagerModule::BuildPage() {
    page_ = new QWidget;
    page_->setObjectName("objectManager");
    page_->installEventFilter(this);
    auto* layout = new QHBoxLayout(page_);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    pages_ = new QListWidget(page_);
    pages_->setObjectName("managerPages");
    pages_->setFixedWidth(128);
    for (int index = 0; index < static_cast<int>(Page::Count); ++index) pages_->addItem(Manager::PageLabel(static_cast<Page>(index)));
    layout->addWidget(pages_);

    auto* splitter = new QSplitter(page_);
    splitter->setChildrenCollapsible(false);
    auto* center = new QWidget(splitter);
    auto* centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(6);
    auto* toolbar = new QHBoxLayout;
    filter_ = new QComboBox(center);
    filter_->setObjectName("managerFilter");
    search_ = new QLineEdit(center);
    search_->setObjectName("managerSearch");
    search_->setPlaceholderText(tr("搜索名称、ID"));
    search_->setClearButtonEnabled(true);
    count_ = new QLabel(center);
    count_->setObjectName("paneHint");
    toolbar->addWidget(filter_);
    rawValues_ = new QCheckBox(tr("显示原始字段"), center);
    rawValues_->setObjectName("managerRawValues");
    toolbar->addWidget(rawValues_);
    connect(rawValues_, &QCheckBox::toggled, this, [this] { Refresh(true); });
    toolbar->addWidget(search_, 1);
    toolbar->addWidget(count_);
    centerLayout->addLayout(toolbar);
    center_ = new QStackedWidget(center);
    overview_ = new QListWidget(center_);
    overview_->setObjectName("managerOverview");
    table_ = new QTreeWidget(center_);
    table_->setObjectName("managerTable");
    table_->setRootIsDecorated(false);
    table_->setUniformRowHeights(true);
    table_->setSortingEnabled(true);
    table_->setAlternatingRowColors(true);
    UiKit::ConfigureResizableColumns(table_->header());
    center_->addWidget(overview_);
    center_->addWidget(table_);
    centerLayout->addWidget(center_, 1);
    splitter->addWidget(center);

    auto* detail = new QWidget(splitter);
    auto* detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(8, 0, 0, 0);
    detailLayout->setSpacing(4);
    auto* heading = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    detailTitle_ = new QLabel(detail);
    detailTitle_->setObjectName("paneTitle");
    detailSubtitle_ = new QLabel(detail);
    detailSubtitle_->setObjectName("paneHint");
    titles->addWidget(detailTitle_);
    titles->addWidget(detailSubtitle_);
    heading->addLayout(titles, 1);
    openButton_ = new QPushButton(tr("打开"), detail);
    openButton_->setObjectName("managerOpen");
    heading->addWidget(openButton_);
    detailLayout->addLayout(heading);
    detailWarning_ = new QLabel(detail);
    detailWarning_->setObjectName("managerWarning");
    detailWarning_->setWordWrap(true);
    detailLayout->addWidget(detailWarning_);
    detail_ = new QTreeWidget(detail);
    detail_->setObjectName("managerDetail");
    detail_->setHeaderHidden(true);
    detail_->setUniformRowHeights(true);
    detailLayout->addWidget(detail_, 1);
    auto* note = new QLabel(tr("只统计工程数据中的直接引用；自定义脚本文本与动态拼接不在其中，不能作为安全删除依据。"), detail);
    note->setObjectName("paneHint");
    note->setWordWrap(true);
    detailLayout->addWidget(note);
    splitter->addWidget(detail);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    connect(pages_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!populating_ && row >= 0) ShowPage(static_cast<Page>(row));
    });
    connect(filter_, &QComboBox::currentIndexChanged, this, [this]() {
        if (populating_) return;
        filters_[static_cast<int>(current_)] = filter_->currentData().toString();
        Refresh(true);
    });
    connect(search_, &QLineEdit::textChanged, this, [this]() { PopulateTable(); });
    connect(table_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (populating_) return;
        const Manager::Row* row = item != nullptr ? &rows_[item->data(0, kRowRole).toULongLong()] : nullptr;
        selection_[static_cast<int>(current_)] = row != nullptr ? row->key : QString{};
        ShowDetail();
    });
    connect(table_, &QTreeWidget::itemActivated, this, [this]() {
        if (const auto* row = CurrentRow(); row != nullptr && row->open && host_ != nullptr) host_->Navigate(*row->open);
    });
    connect(openButton_, &QPushButton::clicked, this, [this]() {
        if (const auto* row = CurrentRow(); row != nullptr && row->open && host_ != nullptr) host_->Navigate(*row->open);
    });
    connect(detail_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) { OpenItem(item); });
    connect(overview_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const int index = item->data(kRowRole).toInt();
        if (index < 0 || index >= static_cast<int>(overviewLines_.size()) || !overviewLines_[index].page) return;
        if (!overviewLines_[index].filter.isEmpty()) filters_[static_cast<int>(*overviewLines_[index].page)] = overviewLines_[index].filter;
        ShowPage(*overviewLines_[index].page);
    });
}

bool ManagerModule::eventFilter(QObject* watched, QEvent* event) {
    if (watched == page_ && event->type() == QEvent::Show) Refresh();
    return QObject::eventFilter(watched, event);
}

void ManagerModule::ShowPage(Page page) {
    current_ = page;
    populating_ = true;
    pages_->setCurrentRow(static_cast<int>(page));
    filter_->clear();
    const auto filters = Manager::PageFilters(page);
    for (const auto& filter : filters) filter_->addItem(filter.label, filter.id);
    QString& saved = filters_[static_cast<int>(page)];
    if (saved.isEmpty() && !filters.empty()) saved = filters.front().id;
    filter_->setCurrentIndex(std::max(0, filter_->findData(saved)));
    filter_->setVisible(!filters.empty());
    search_->setVisible(page != Page::Overview);
    center_->setCurrentWidget(page == Page::Overview ? static_cast<QWidget*>(overview_) : table_);
    populating_ = false;
    Refresh(true);
}

void ManagerModule::Refresh(bool force) {
    if (!page_ || (!force && !page_->isVisible())) return;
    const std::uint64_t generation = project_.ReferenceGeneration();
    const bool allFunctions = current_ == Page::Function && filters_[static_cast<int>(current_)] == QLatin1String("all");
    if (force || generation != rowsGeneration_ || current_ != rowsPage_ || allFunctions != rowsAllFunctions_) {
        if (current_ == Page::Overview) overviewLines_ = Manager::BuildOverview(project_);
        else rows_ = Manager::BuildRows(project_, current_, allFunctions, rawValues_->isChecked());
        rowsGeneration_ = generation;
        rowsPage_ = current_;
        rowsAllFunctions_ = allFunctions;
        RebuildNavigationIdentities();
    }
    if (current_ == Page::Overview) PopulateOverview();
    else PopulateTable();
}

void ManagerModule::PopulateOverview() {
    overview_->clear();
    QString section;
    for (std::size_t index = 0; index < overviewLines_.size(); ++index) {
        const auto& line = overviewLines_[index];
        if (line.section != section) {
            section = line.section;
            auto* header = new QListWidgetItem(section, overview_);
            header->setFlags(Qt::NoItemFlags);
            QFont font = header->font();
            font.setBold(true);
            header->setFont(font);
        }
        auto* item = new QListWidgetItem(QStringLiteral("%1　%2").arg(line.label, line.value), overview_);
        item->setData(kRowRole, static_cast<int>(index));
        if (!line.page) item->setFlags(Qt::ItemIsEnabled);
        else item->setToolTip(tr("双击查看"));
    }
    count_->clear();
    detailTitle_->setText(Manager::PageLabel(Page::Overview));
    detailSubtitle_->clear();
    detailWarning_->clear();
    openButton_->setEnabled(false);
    detail_->clear();
}

void ManagerModule::PopulateTable() {
    if (current_ == Page::Overview) return;
    populating_ = true;
    table_->setSortingEnabled(false);
    table_->clear();
    const auto columns = Manager::PageColumns(current_);
    QStringList labels;
    for (const auto& column : columns) labels << column.label;
    // 换页时列数变化：名称列给出够看的初始宽度，其余按内容；用户拖动过的宽度在同一页内保留。
    const bool pageChanged = table_->property("page").toInt() != static_cast<int>(current_) || table_->columnCount() != labels.size();
    table_->setHeaderLabels(labels);
    if (pageChanged) {
        table_->setProperty("page", static_cast<int>(current_));
        table_->header()->resizeSection(0, 260);
        for (int column = 1; column + 1 < labels.size(); ++column) table_->header()->resizeSection(column, 90);
    }
    const QString filter = filters_[static_cast<int>(current_)];
    const QString query = search_->text().trimmed();
    QTreeWidgetItem* selected = nullptr;
    int shown = 0;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        if (!Manager::RowPasses(current_, row, filter)) continue;
        if (!query.isEmpty() && !row.cells.join(' ').contains(query, Qt::CaseInsensitive)) continue;
        auto* item = new RowItem(table_, row.cells);
        item->setToolTip(0, row.tooltip);
        item->setData(0, kRowRole, static_cast<qulonglong>(index));
        for (int column = 0; column < static_cast<int>(columns.size()) && column < row.cells.size(); ++column) {
            if (columns[column].numeric) item->setData(column, kNumberRole, row.cells[column].toLongLong());
        }
        if (row.unused) item->setForeground(0, table_->palette().color(QPalette::PlaceholderText));
        if (row.key == selection_[static_cast<int>(current_)]) selected = item;
        ++shown;
    }
    table_->setSortingEnabled(true);
    count_->setText(tr("%1 项").arg(shown));
    if (selected == nullptr && table_->topLevelItemCount() > 0) selected = table_->topLevelItem(0);
    table_->setCurrentItem(selected);
    if (selected != nullptr) table_->scrollToItem(selected);
    populating_ = false;
    ShowDetail();
}

const Manager::Row* ManagerModule::CurrentRow() const {
    const auto* item = table_ != nullptr ? table_->currentItem() : nullptr;
    if (item == nullptr || current_ == Page::Overview) return nullptr;
    const auto index = item->data(0, kRowRole).toULongLong();
    return index < rows_.size() ? &rows_[index] : nullptr;
}

std::optional<QJsonObject> ManagerModule::FunctionNavigationIdentity(const QString& key) const {
    const auto* catalog = project_.uiCatalog_.get();
    if (!catalog || !SourceSha(catalog->Sha256()) || key.isEmpty() || key.size() > 4096) return std::nullopt;
    const auto name = key.toUtf8().toStdString();
    const Trigger::UiDefinition* selected = nullptr;
    for (const auto kind : {Trigger::UiDefinitionKind::Event, Trigger::UiDefinitionKind::Condition,
             Trigger::UiDefinitionKind::Action, Trigger::UiDefinitionKind::Call}) {
        if (const auto* definition = catalog->Find(kind, name)) {
            if (selected) return std::nullopt; // 行投影按名字去重，不能猜属于哪一种定义。
            selected = definition;
        }
    }
    if (!selected || selected->revisions.empty()) return std::nullopt;
    const auto& active = selected->Active();
    std::size_t bytes = 0;
    if (active.kind != selected->kind || active.name != name || active.group != selected->group
        || active.group.size() > 2048 || active.layer.size() > 2048
        || !SourceSha(active.sourceSha256) || active.source.empty()
        || !NavigationText(active.name, bytes) || !NavigationText(active.group, bytes)
        || !NavigationText(active.layer, bytes)) return std::nullopt;
    const auto source = QString::fromStdWString(active.source.generic_wstring());
    if (source.isEmpty() || source.size() > 4096 || source.contains(QChar(u'\0'))) return std::nullopt;
    return QJsonObject{{"type", "function"}, {"kind", static_cast<int>(active.kind)},
        {"name", key}, {"group", QString::fromUtf8(active.group.data(), static_cast<qsizetype>(active.group.size()))},
        {"layer", QString::fromUtf8(active.layer.data(), static_cast<qsizetype>(active.layer.size()))}, {"source", source},
        {"sourceSha256", QString::fromStdString(active.sourceSha256)}, {"catalogSha256", QString::fromStdString(catalog->Sha256())},
        {"referenceGeneration", QString::number(project_.ReferenceGeneration())}};
}

std::optional<QJsonObject> ManagerModule::CurrentNavigationIdentity(Page page, const QString& key) const {
    if (key.isEmpty() || (page != Page::Variable && page != Page::Function)) return QJsonObject{};
    if (page == Page::Function) return FunctionNavigationIdentity(key);
    const auto* document = project_.triggerDocument_.get();
    if (!document || document->variables.size() > kNavigationRows) return std::nullopt;
    std::size_t bytes = 0;
    std::set<StableId> ids;
    const Trigger::VariableRecord* selected = nullptr;
    const auto name = key.toUtf8().toStdString();
    for (const auto& variable : document->variables) {
        if (!NavigationText(variable.name.text, bytes)) return std::nullopt;
        if (variable.name.text == name) {
            if (selected) return std::nullopt;
            selected = &variable;
        }
        if (!variable.id || !ids.insert(variable.id).second) return std::nullopt;
    }
    if (!selected) return std::nullopt;
    return QJsonObject{{"type", "variable"}, {"stableId", QString::number(selected->id)}};
}

void ManagerModule::RebuildNavigationIdentities() {
    navigationIdentities_.clear();
    if (current_ != Page::Variable && current_ != Page::Function) return;
    if (rows_.size() > kNavigationRows) return;
    if (current_ == Page::Variable) {
        const auto* document = project_.triggerDocument_.get();
        if (!document || document->variables.size() > kNavigationRows) return;
        std::size_t bytes = 0;
        std::set<StableId> ids;
        for (const auto& variable : document->variables) {
            if (!NavigationText(variable.name.text, bytes) || !variable.id || !ids.insert(variable.id).second) {
                navigationIdentities_.clear(); return;
            }
            const auto key = QString::fromUtf8(variable.name.text.data(), static_cast<qsizetype>(variable.name.text.size()));
            if (navigationIdentities_.contains(key)) { navigationIdentities_.clear(); return; }
            navigationIdentities_.insert(key, {{"type", "variable"}, {"stableId", QString::number(variable.id)}});
        }
        return;
    }
    std::size_t bytes = 0;
    for (const auto& row : rows_) {
        if (!NavigationText(row.key.toUtf8().toStdString(), bytes)) { navigationIdentities_.clear(); return; }
        if (const auto identity = FunctionNavigationIdentity(row.key)) {
            const auto identityBytes = QJsonDocument(*identity).toJson(QJsonDocument::Compact).size();
            if (static_cast<std::size_t>(identityBytes) > kNavigationTextBytes - bytes) { navigationIdentities_.clear(); return; }
            bytes += static_cast<std::size_t>(identityBytes);
            navigationIdentities_.insert(row.key, *identity);
        }
    }
}

void ManagerModule::AddUsageSection(const QString& title, const std::vector<const Services::Usage*>& usages, const QString& empty) {
    auto* section = new QTreeWidgetItem(detail_, {QStringLiteral("%1（%2）").arg(title).arg(usages.size())});
    QFont bold = section->font(0);
    bold.setBold(true);
    section->setFont(0, bold);
    if (usages.empty()) {
        new QTreeWidgetItem(section, {empty});
        section->setExpanded(true);
        return;
    }
    // 按所在触发器 / 物体分组，组内保持语句顺序。
    std::vector<std::pair<std::string, std::vector<const Services::Usage*>>> groups;
    std::map<std::string, std::size_t> groupIndex;
    for (const auto* usage : usages) {
        const auto [found, inserted] = groupIndex.emplace(usage->ownerId, groups.size());
        if (inserted) groups.push_back({usage->ownerId, {}});
        groups[found->second].second.push_back(usage);
    }
    std::size_t rendered = 0;
    for (const auto& [ownerId, list] : groups) {
        auto* group = new QTreeWidgetItem(section, {QStringLiteral("%1（%2）")
            .arg(Manager::UsageOwnerLabel(project_, *list.front())).arg(list.size())});
        for (const auto* usage : list) {
            if (rendered++ >= kMaxRenderedUsages) break;
            auto* item = new QTreeWidgetItem(group, {Manager::UsageText(project_, *usage, rawValues_->isChecked())});
            if (const auto target = Manager::UsageTarget(project_, *usage)) {
                item->setData(0, kTargetRole, static_cast<int>(detailTargets_.size()));
                detailTargets_.push_back(*target);
            }
            if (usage->disabled) {
                QFont font = item->font(0);
                font.setStrikeOut(true);
                item->setFont(0, font);
                item->setForeground(0, detail_->palette().color(QPalette::PlaceholderText));
            }
        }
        if (rendered > kMaxRenderedUsages) {
            new QTreeWidgetItem(section, {tr("…其余 %1 处未展开显示").arg(usages.size() - kMaxRenderedUsages)});
            break;
        }
    }
    section->setExpanded(true);
    if (groups.size() <= 8) {
        for (int child = 0; child < section->childCount(); ++child) section->child(child)->setExpanded(true);
    }
}

void ManagerModule::ShowDetail() {
    detail_->clear();
    detailTargets_.clear();
    const Manager::Row* row = CurrentRow();
    openButton_->setEnabled(row != nullptr && row->open.has_value());
    if (row == nullptr) {
        detailTitle_->setText(Manager::PageLabel(current_));
        detailSubtitle_->clear();
        detailWarning_->clear();
        return;
    }
    detailTitle_->setText(row->title);
    detailSubtitle_->setText(row->title != row->key ? row->key + QStringLiteral(" · ") + row->subtitle : row->subtitle);
    const bool placeable = current_ == Page::Unit || current_ == Page::Item || current_ == Page::Destructable || current_ == Page::Doodad;
    QString warning = row->unused ? (placeable ? tr("未发现静态引用，也没有放置在地图上") : tr("未发现静态引用")) : QString{};
    if (row->tag == QLatin1String("custom")) {
        const std::map<Page, ObjectData::ObjectType> types{
            {Page::Unit, ObjectData::ObjectType::Unit},
            {Page::Item, ObjectData::ObjectType::Item},
            {Page::Destructable, ObjectData::ObjectType::Destructable},
            {Page::Doodad, ObjectData::ObjectType::Doodad},
            {Page::Ability, ObjectData::ObjectType::Ability},
            {Page::Buff, ObjectData::ObjectType::Buff},
            {Page::Upgrade, ObjectData::ObjectType::Upgrade}};
        if (const auto type = types.find(current_); type != types.end()) {
            const ObjectData::ObjectKey key{type->second, row->key.toStdString()};
            const auto audit = project_.AssessDeletion(key);
            QStringList reasons;
            for (const auto& reason : audit.reasons)
                reasons << QString::fromUtf8(reason.message);
            if (audit.scriptHintCount != 0) {
                reasons << tr("脚本文本有 %1 处候选（包括注释或字符串，尚未确认为引用）")
                    .arg(audit.scriptHintCount);
                for (const auto& hint : audit.scriptHints)
                    reasons << tr("候选位置：%1 第 %2 行")
                        .arg(QString::fromUtf8(hint.source)).arg(hint.line);
            }
            if (audit.dynamicValueCount != 0)
                reasons << tr("GUI 中有 %1 处变量、函数或数组取值需要分析")
                    .arg(audit.dynamicValueCount);
            if (!warning.isEmpty()) warning += QStringLiteral("\n");
            warning += tr("删除暂不可用：%1").arg(reasons.isEmpty() ? tr("引用审计尚未完成") : reasons.first());
            detailWarning_->setToolTip(reasons.join(QStringLiteral("\n")));
        }
    } else detailWarning_->setToolTip({});
    detailWarning_->setText(warning);
    if (current_ == Page::Function) {
        AddUsageSection(tr("调用位置"), row->usages, tr("地图中没有使用这个函数"));
        return;
    }
    AddUsageSection(tr("被引用"), row->usages, tr("没有其他内容引用它"));
    if (placeable) AddUsageSection(tr("放置在地图上"), row->placed, tr("没有放置在地图上"));
    if (!row->outgoingOwner.empty()) {
        std::vector<Manager::ResolvedToken> used;
        for (const auto& token : project_.References().Outgoing(row->outgoingOwner)) {
            if (auto resolved = Manager::ResolveToken(project_, token); resolved && resolved->target.id != row->open->id) {
                used.push_back(std::move(*resolved));
            }
        }
        auto* section = new QTreeWidgetItem(detail_, {tr("使用了（%1）").arg(used.size())});
        for (const auto& entry : used) {
            auto* item = new QTreeWidgetItem(section, {entry.type + QStringLiteral(" · ") + entry.label});
            item->setData(0, kTargetRole, static_cast<int>(detailTargets_.size()));
            detailTargets_.push_back(entry.target);
        }
        section->setExpanded(true);
    }
    if (current_ == Page::Trigger && !row->outgoingOwner.empty()) {
        const auto called = Manager::FunctionsCalledBy(project_, row->outgoingOwner);
        auto* section = new QTreeWidgetItem(detail_, {tr("调用的函数（%1）").arg(called.size())});
        for (const auto& [name, count] : called) {
            auto* item = new QTreeWidgetItem(section, {QStringLiteral("%1 × %2").arg(QString::fromStdString(name)).arg(count)});
            item->setData(0, kTargetRole, static_cast<int>(detailTargets_.size()));
            detailTargets_.push_back({"manager", "function", QString::fromStdString(name), {}});
        }
    }
}

void ManagerModule::OpenItem(QTreeWidgetItem* item) {
    const QVariant data = item != nullptr ? item->data(0, kTargetRole) : QVariant{};
    bool validIndex = false;
    const int index = data.toInt(&validIndex);
    if (!data.isValid() || !validIndex || index < 0 || index >= static_cast<int>(detailTargets_.size())) return;
    const auto target = detailTargets_[static_cast<std::size_t>(index)];
    if (host_ != nullptr) {
        // 同模块引用跳转也经过宿主，保存当前实体与滚动位置供后退/前进恢复。
        host_->Navigate(target);
    } else if (target.module == "manager") {
        (void)Reveal(target);
    }
}

bool ManagerModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "manager") return false;
    std::optional<Page> page;
    QString key = target.id;
    if (target.kind == QLatin1String("object")) {
        const int colon = key.indexOf(':');
        if (colon <= 0 || colon + 1 >= key.size()) return false;
        page = PageOfKind(colon > 0 ? key.left(colon) : QString{});
        key = key.mid(colon + 1);
    } else if (target.kind == QLatin1String("trigger")) {
        page = Page::Trigger;
    } else if (target.kind == QLatin1String("function")) {
        page = Page::Function;
    } else if (target.kind == QLatin1String("variable")) {
        page = Page::Variable;
    }
    if (!page || key.isEmpty() || key.size() > 4096) return false;
    // 失败不能通过 ShowPage/PopulateTable 的首行回退改变当前视图。
    // 复用真实业务行查询；只验证候选，不发布行、筛选、选择或编辑事务。
    if (*page == Page::Variable) {
        const auto* document = project_.triggerDocument_.get();
        if (!document || document->variables.size() > kNavigationRows) return false;
        std::size_t bytes = 0;
        for (const auto& variable : document->variables) {
            if (!NavigationText(variable.name.text, bytes)) return false;
        }
    } else if (*page == Page::Trigger
        && (!project_.triggerDocument_ || project_.triggerDocument_->triggers.size() > kNavigationRows)) {
        return false;
    }
    const auto generation = project_.ReferenceGeneration();
    const auto candidates = Manager::BuildRows(project_, *page, *page == Page::Function,
        rawValues_ != nullptr && rawValues_->isChecked());
    if (candidates.size() > kNavigationRows) return false;
    std::size_t bytes = 0, matches = 0;
    for (const auto& row : candidates) {
        if (!NavigationText(row.key.toUtf8().toStdString(), bytes)) return false;
        if (row.key == key) ++matches;
    }
    if (matches != 1 || project_.ReferenceGeneration() != generation) return false;
    (void)Widget();
    selection_[static_cast<int>(*page)] = key;
    filters_[static_cast<int>(*page)] = QStringLiteral("all");
    search_->clear();
    ShowPage(*page);
    const auto* row = CurrentRow();
    return row != nullptr && row->key == key;
}

QJsonObject ManagerModule::SaveViewState() const {
    QJsonArray filters;
    QJsonArray selection;
    for (const auto& value : filters_) filters.push_back(value);
    for (const auto& value : selection_) selection.push_back(value);
    return {{"page", static_cast<int>(current_)}, {"filters", filters}, {"selection", selection}};
}

void ManagerModule::RestoreViewState(const QJsonObject& state) {
    const QJsonArray filters = state["filters"].toArray();
    const QJsonArray selection = state["selection"].toArray();
    for (int index = 0; index < static_cast<int>(Page::Count); ++index) {
        filters_[static_cast<std::size_t>(index)] = filters.at(index).toString();
        selection_[static_cast<std::size_t>(index)] = selection.at(index).toString();
    }
    const int page = state["page"].toInt();
    current_ = page >= 0 && page < static_cast<int>(Page::Count) ? static_cast<Page>(page) : Page::Overview;
    if (page_) ShowPage(current_);
}

std::optional<QJsonObject> ManagerModule::CaptureNavigationBookmark() const {
    if (!page_ || !table_ || !search_ || !filter_ || !rawValues_) return std::nullopt;
    const auto* row = CurrentRow();
    const auto key = current_ != Page::Overview && row ? row->key : QString{};
    const auto filter = filter_->currentData().toString();
    if (key.size() > 4096 || search_->text().size() > WorldNavigation::MaximumText || filter.size() > 128) return std::nullopt;
    const auto identity = CurrentNavigationIdentity(current_, key);
    if (!identity) return std::nullopt;
    if (!key.isEmpty() && (current_ == Page::Variable || current_ == Page::Function)) {
        if (rowsGeneration_ != project_.ReferenceGeneration()
            || navigationIdentities_.value(key) != *identity) return std::nullopt;
    }
    const bool overview = current_ == Page::Overview;
    QJsonObject state{{"schema", 1}, {"page", static_cast<int>(current_)}, {"key", key}, {"identity", *identity}, {"filter", filter},
        {"search", search_->text()}, {"rawValues", rawValues_->isChecked()},
        {"horizontal", overview ? overview_->horizontalScrollBar()->value() : table_->horizontalScrollBar()->value()},
        {"vertical", overview ? overview_->verticalScrollBar()->value() : table_->verticalScrollBar()->value()},
        {"detailHorizontal", detail_->horizontalScrollBar()->value()}, {"detailVertical", detail_->verticalScrollBar()->value()},
        {"pagesVertical", pages_->verticalScrollBar()->value()},
        {"sortColumn", overview ? 0 : std::max(0, table_->sortColumn())},
        {"sortOrder", static_cast<int>(table_->header()->sortIndicatorOrder())}};
    return WorldNavigation::Bounded(state) ? std::optional(state) : std::nullopt;
}

Shell::NavigationRestoreResult ManagerModule::RestoreNavigationBookmark(const QJsonObject& state) {
    using Restore = Shell::NavigationRestoreResult;
    const auto schema = WorldNavigation::Integer(state.value("schema"), 1, 1);
    const auto page = WorldNavigation::Integer(state.value("page"), 0, static_cast<int>(Page::Count) - 1);
    if (!schema || !page || !WorldNavigation::Bounded(state) || !state.value("key").isString()
        || !state.value("filter").isString() || !state.value("search").isString() || !state.value("rawValues").isBool()) return Restore::Invalid;
    const auto candidatePage = static_cast<Page>(*page);
    const auto key = state.value("key").toString(), filter = state.value("filter").toString(), search = state.value("search").toString();
    if (key.size() > 4096 || filter.size() > 128 || search.size() > WorldNavigation::MaximumText) return Restore::Invalid;
    const auto identity = CurrentNavigationIdentity(candidatePage, key);
    if (!state.value("identity").isObject() || !identity || state.value("identity").toObject() != *identity) return Restore::Invalid;
    const auto filters = Manager::PageFilters(candidatePage);
    if (filters.empty() ? !filter.isEmpty() : std::none_of(filters.begin(), filters.end(), [&](const auto& item) { return item.id == filter; })) return Restore::Invalid;
    const auto columns = Manager::PageColumns(candidatePage);
    const auto sortColumn = WorldNavigation::Integer(state.value("sortColumn"), 0,
        candidatePage == Page::Overview ? 0 : static_cast<std::int64_t>(columns.size()) - 1);
    const auto sortOrder = WorldNavigation::Integer(state.value("sortOrder"), 0, 1);
    std::array<int, 5> scroll{};
    const std::array<const char*, 5> names{"horizontal", "vertical", "detailHorizontal", "detailVertical", "pagesVertical"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto value = WorldNavigation::Integer(state.value(QLatin1String(names[index])));
        if (!value) return Restore::Invalid;
        scroll[index] = static_cast<int>(*value);
    }
    if (!sortColumn || !sortOrder || (candidatePage == Page::Overview && !key.isEmpty())) return Restore::Invalid;
    if (!key.isEmpty()) {
        const auto candidates = Manager::BuildRows(project_, candidatePage,
            candidatePage == Page::Function && filter == QLatin1String("all"), state.value("rawValues").toBool());
        std::size_t matches = 0;
        for (const auto& row : candidates) if (row.key == key) {
            if (!Manager::RowPasses(candidatePage, row, filter)
                || (!search.trimmed().isEmpty() && !row.cells.join(' ').contains(search.trimmed(), Qt::CaseInsensitive))) return Restore::Invalid;
            ++matches;
        }
        if (matches != 1) return Restore::Invalid;
    }
    // rows/key/filter/search 全部验证完成后才更改视图；旧目标失效不接受 PopulateTable 的首行回退。
    (void)Widget();
    {
        const QSignalBlocker raw(rawValues_), searching(search_);
        rawValues_->setChecked(state.value("rawValues").toBool());
        search_->setText(search);
    }
    filters_[static_cast<int>(candidatePage)] = filter;
    selection_[static_cast<int>(candidatePage)] = key;
    ShowPage(candidatePage);
    if (candidatePage != Page::Overview) {
        table_->sortItems(static_cast<int>(*sortColumn), static_cast<Qt::SortOrder>(*sortOrder));
        if (key.isEmpty()) {
            const QSignalBlocker selecting(table_);
            table_->clearSelection(); table_->setCurrentItem(nullptr);
            selection_[static_cast<int>(candidatePage)].clear();
            ShowDetail();
        }
        table_->doItemsLayout();
        table_->horizontalScrollBar()->setValue(scroll[0]); table_->verticalScrollBar()->setValue(scroll[1]);
    } else {
        overview_->horizontalScrollBar()->setValue(scroll[0]); overview_->verticalScrollBar()->setValue(scroll[1]);
    }
    detail_->horizontalScrollBar()->setValue(scroll[2]); detail_->verticalScrollBar()->setValue(scroll[3]);
    pages_->verticalScrollBar()->setValue(scroll[4]);
    return Restore::Restored;
}

} // namespace War3Map::Modules
