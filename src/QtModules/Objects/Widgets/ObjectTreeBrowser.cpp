#include "ObjectTreeBrowser.h"

#include "Icons/IconProvider.h"
#include "Icons/SvgIcons.h"
#include "ObjectClassification.h"
#include "Widgets/SearchField.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <memory>

namespace War3Map::QtApp {
namespace {

constexpr int kKindRole = Qt::UserRole + 1;   // 0 根，1 文件夹，2 物体
constexpr int kIdRole = Qt::UserRole + 2;
constexpr int kCustomRole = Qt::UserRole + 3;
constexpr int kIconPathRole = Qt::UserRole + 4;
constexpr int kSearchRole = Qt::UserRole + 5;
constexpr int kKeyRole = Qt::UserRole + 6;    // 根 / 文件夹的路径键，用于记住展开状态
// 原版物体编辑器用品红色标出自定义物体与修改过的标准物体。
const QColor kChangedColor(0xd8, 0x6c, 0xd8);

// 物体图标按需读取：几千个物体只解码滚动到的那部分。
class LazyIconModel final : public QStandardItemModel {
public:
    LazyIconModel(IconProvider* icons, QObject* parent) : QStandardItemModel(parent), icons_(icons) {}
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override {
        if (role == Qt::DecorationRole && icons_ != nullptr && QStandardItemModel::data(index, kKindRole).toInt() == 2) {
            const QString path = QStandardItemModel::data(index, kIconPathRole).toString();
            if (!path.isEmpty()) {
                const QIcon icon = icons_->IconForTexture(path);
                if (!icon.isNull()) return icon;
            }
        }
        return QStandardItemModel::data(index, role);
    }

private:
    IconProvider* icons_ = nullptr;
};

struct Folder {
    QString label;
    QString key;
    int depth = 0;
    std::map<QString, std::unique_ptr<Folder>> folders;
    std::vector<const ObjectTreeBrowser::Object*> objects;
    int count = 0;
};

int Count(Folder& folder) {
    folder.count = static_cast<int>(folder.objects.size());
    for (auto& [label, child] : folder.folders) folder.count += Count(*child);
    return folder.count;
}

} // namespace

ObjectTreeBrowser::ObjectTreeBrowser(IconProvider* icons, QWidget* parent)
    : QWidget(parent),
      icons_(icons) {
    setObjectName("objectTreeBrowser");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    search_ = new QLineEdit(this);
    search_->setObjectName("objectTreeSearch");
    search_->setPlaceholderText(tr("搜索名称 / ID"));
    search_->setClearButtonEnabled(true);
    UiKit::InstallSearchIcon(search_);
    auto* searchRow = new QHBoxLayout;
    searchRow->addWidget(search_, 1);
    showIds_ = new QCheckBox(tr("显示 ID"), this);
    showIds_->setObjectName("objectTreeShowIds");
    showIds_->setToolTip(tr("在目录中按对象 ID 显示；搜索仍匹配名称和 ID"));
    searchRow->addWidget(showIds_);
    layout->addLayout(searchRow);
    view_ = new QTreeView(this);
    view_->setObjectName("objectTree");
    view_->setHeaderHidden(true);
    view_->setUniformRowHeights(true);
    view_->setIndentation(16);
    view_->setIconSize(QSize(24, 24));
    view_->setSelectionMode(QAbstractItemView::SingleSelection);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(view_, 1);
    model_ = new LazyIconModel(icons_, this);
    filter_ = new QSortFilterProxyModel(this);
    filter_->setSourceModel(model_);
    filter_->setRecursiveFilteringEnabled(true);
    filter_->setFilterRole(kSearchRole);
    filter_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    view_->setModel(filter_);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        silent_ = true;
        filter_->setFilterFixedString(text.trimmed());
        if (text.trimmed().isEmpty()) RestoreExpansion();
        else view_->expandAll();
        silent_ = false;
    });
    connect(showIds_, &QCheckBox::toggled, this, [this] {
        const auto current = Current();
        Rebuild();
        if (current) Select(current->first, current->second);
    });
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
        if (silent_ || current.data(kKindRole).toInt() != 2) return;
        emit ObjectSelected(current.data(kIdRole).toString(), current.data(kCustomRole).toBool());
    });
    connect(view_, &QTreeView::expanded, this, [this](const QModelIndex& index) {
        UpdateFolderIcon(index, true);
        if (!silent_ && search_->text().trimmed().isEmpty()) expanded_[typeKey_].insert(index.data(kKeyRole).toString());
    });
    connect(view_, &QTreeView::collapsed, this, [this](const QModelIndex& index) {
        UpdateFolderIcon(index, false);
        if (!silent_ && search_->text().trimmed().isEmpty()) expanded_[typeKey_].remove(index.data(kKeyRole).toString());
    });
    connect(view_, &QTreeView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex index = view_->indexAt(pos);
        if (index.isValid()) view_->setCurrentIndex(index);
        emit ContextMenuRequested(view_->viewport()->mapToGlobal(pos));
    });
}

void ObjectTreeBrowser::SetObjects(const QString& typeKey, const QString& standardRoot, const QString& customRoot,
    std::vector<Object> objects) {
    const auto current = typeKey == typeKey_ ? Current() : std::nullopt;
    typeKey_ = typeKey;
    standardRoot_ = standardRoot;
    customRoot_ = customRoot;
    objects_ = std::move(objects);
    Rebuild();
    if (current) Select(current->first, current->second);
}

void ObjectTreeBrowser::Rebuild() {
    silent_ = true;
    model_->clear();
    Folder roots[2];
    roots[0].label = standardRoot_;
    roots[0].key = QStringLiteral("s");
    roots[1].label = customRoot_;
    roots[1].key = QStringLiteral("c");
    for (const Object& object : objects_) {
        Folder* folder = &roots[object.custom ? 1 : 0];
        for (const QString& name : object.folders) {
            auto& child = folder->folders[name];
            if (!child) {
                child = std::make_unique<Folder>();
                child->label = name;
                child->key = folder->key + QLatin1Char('/') + name;
                child->depth = folder->depth + (folder == &roots[0] || folder == &roots[1] ? 0 : 1);
            }
            folder = child.get();
        }
        folder->objects.push_back(&object);
    }
    // 系统文件夹图标，在 24 像素的行里只画 16 像素（物体图标保持 24）。
    const QIcon folderIcon = UiKit::SystemFolderIcon(16, 24);
    // 文件夹按原版顺序在前，物体按名称在后。
    const std::function<void(QStandardItem*, Folder&, int)> fill = [&](QStandardItem* parent, Folder& folder, int depth) {
        std::vector<Folder*> children;
        for (auto& [label, child] : folder.folders) children.push_back(child.get());
        std::sort(children.begin(), children.end(), [depth](const Folder* left, const Folder* right) {
            const int a = Modules::ObjectDetail::FolderRank(left->label, depth);
            const int b = Modules::ObjectDetail::FolderRank(right->label, depth);
            return a != b ? a < b : left->label < right->label;
        });
        for (Folder* child : children) {
            auto* item = new QStandardItem(folderIcon, QStringLiteral("%1 (%2)").arg(child->label).arg(child->count));
            item->setData(1, kKindRole);
            item->setData(child->key, kKeyRole);
            parent->appendRow(item);
            fill(item, *child, depth + 1);
        }
        std::sort(folder.objects.begin(), folder.objects.end(), [](const Object* left, const Object* right) {
            return QString::localeAwareCompare(left->name, right->name) < 0;
        });
        for (const Object* object : folder.objects) {
            auto* item = new QStandardItem(showIds_->isChecked() ? object->id : object->name);
            item->setData(2, kKindRole);
            item->setData(object->id, kIdRole);
            item->setData(object->custom, kCustomRole);
            item->setData(object->iconPath, kIconPathRole);
            item->setData(object->name + QLatin1Char(' ') + object->id, kSearchRole);
            item->setToolTip(object->tooltip.isEmpty() ? QStringLiteral("%1 (%2)").arg(object->name, object->id)
                                                       : QStringLiteral("%1 (%2)\n%3").arg(object->name, object->id, object->tooltip));
            if (object->custom || object->modified) item->setForeground(kChangedColor);
            parent->appendRow(item);
        }
    };
    for (Folder& root : roots) {
        Count(root);
        auto* item = new QStandardItem(folderIcon, QStringLiteral("%1 (%2)").arg(root.label).arg(root.count));
        item->setData(0, kKindRole);
        item->setData(root.key, kKeyRole);
        model_->appendRow(item);
        fill(item, root, 0);
    }
    RestoreExpansion();
    silent_ = false;
}

void ObjectTreeBrowser::RestoreExpansion() {
    const bool previous = silent_;
    silent_ = true;
    view_->collapseAll();
    if (!expanded_.contains(typeKey_)) {
        expanded_[typeKey_] = {QStringLiteral("s"), QStringLiteral("c")}; // 默认只展开两个根
    }
    const QSet<QString>& keys = expanded_[typeKey_];
    const std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int row = 0; row < filter_->rowCount(parent); ++row) {
            const QModelIndex index = filter_->index(row, 0, parent);
            if (index.data(kKindRole).toInt() == 2) continue;
            if (keys.contains(index.data(kKeyRole).toString())) {
                view_->expand(index);
                walk(index);
            }
        }
    };
    walk({});
    silent_ = previous;
}

void ObjectTreeBrowser::UpdateFolderIcon(const QModelIndex& proxyIndex, bool expanded) {
    // 系统文件夹图标不区分展开状态（与资源管理器一致）。
    (void)proxyIndex;
    (void)expanded;
}

void ObjectTreeBrowser::Select(const QString& id, bool custom) {
    const bool previous = silent_;
    silent_ = true;
    QModelIndex found;
    const std::function<bool(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int row = 0; row < model_->rowCount(parent); ++row) {
            const QModelIndex index = model_->index(row, 0, parent);
            if (index.data(kKindRole).toInt() == 2) {
                if (index.data(kIdRole).toString() == id && index.data(kCustomRole).toBool() == custom) {
                    found = index;
                    return true;
                }
            } else if (walk(index)) {
                return true;
            }
        }
        return false;
    };
    walk({});
    const QModelIndex proxy = filter_->mapFromSource(found);
    if (proxy.isValid()) {
        view_->setCurrentIndex(proxy);
        view_->scrollTo(proxy); // 会展开所在文件夹
        for (QModelIndex parent = proxy.parent(); parent.isValid(); parent = parent.parent()) {
            expanded_[typeKey_].insert(parent.data(kKeyRole).toString());
        }
    } else {
        view_->selectionModel()->clearCurrentIndex();
        view_->clearSelection();
    }
    silent_ = previous;
}

std::optional<std::pair<QString, bool>> ObjectTreeBrowser::Current() const {
    const QModelIndex index = view_->currentIndex();
    if (!index.isValid() || index.data(kKindRole).toInt() != 2) return std::nullopt;
    return std::make_pair(index.data(kIdRole).toString(), index.data(kCustomRole).toBool());
}

void ObjectTreeBrowser::SetSearchText(const QString& text) {
    search_->setText(text);
}

void ObjectTreeBrowser::SetShowIds(bool enabled) {
    showIds_->setChecked(enabled);
}

bool ObjectTreeBrowser::ShowIds() const {
    return showIds_->isChecked();
}

QString ObjectTreeBrowser::FolderKey(const QStandardItem* item) const {
    return item == nullptr ? QString{} : item->data(kKeyRole).toString();
}

} // namespace War3Map::QtApp
