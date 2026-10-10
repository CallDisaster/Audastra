#include "ObjectModels.h"
#include "ObjectModelsDetail.h"

using namespace War3Map::ObjectData;
using namespace ObjectModelsDetail;

// 物体列表模型与继承链解析。


ObjectListModel::ObjectListModel(QObject* parent)
    : QAbstractTableModel(parent) {
}

void ObjectListModel::SetDocument(const LniObjectDocument* document) {
    beginResetModel();
    document_ = document;
    newObjects_.clear();
    endResetModel();
}

void ObjectListModel::MarkNewObject(const std::string& rawcode) {
    newObjects_.insert(rawcode);
    const int row = rowCount();
    if (row > 0) {
        emit dataChanged(index(0, 0), index(row - 1, columnCount() - 1),
                         {Qt::ForegroundRole});
    }
}

void ObjectListModel::SetPresentationResolver(PresentationResolver resolver) {
    presentationResolver_ = std::move(resolver);
    RefreshPresentation();
}

void ObjectListModel::RefreshPresentation() {
    if (rowCount() > 0) emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
}

bool ObjectListModel::IsNewObject(const std::string& rawcode) const {
    return newObjects_.contains(rawcode);
}


const ObjectRecord* ObjectListModel::ObjectAt(int row) const {
    if (document_ == nullptr || row < 0 ||
        static_cast<std::size_t>(row) >= document_->Objects().size()) {
        return nullptr;
    }
    return &document_->Objects()[static_cast<std::size_t>(row)];
}


int ObjectListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() || document_ == nullptr
        ? 0
        : static_cast<int>(document_->Objects().size());
}


int ObjectListModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 4;
}


QVariant ObjectListModel::data(const QModelIndex& index, int role) const {
    const ObjectRecord* object = ObjectAt(index.row());
    if (object == nullptr) {
        return {};
    }
    if (role == Qt::UserRole) {
        return ToQString(object->key.rawcode);
    }
    if (role == Qt::DecorationRole && index.column() == 1 && presentationResolver_) {
        return presentationResolver_(*object).second;
    }
    if (role == Qt::ForegroundRole) {
        if (newObjects_.contains(object->key.rawcode)) {
            return QBrush(QColor(QStringLiteral("#4ec96f")));  // 新增物体标绿
        }
        return {};
    }
    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
        return {};
    }
    switch (index.column()) {
    case 0: return ToQString(object->key.rawcode);
    // 无宿主解析器的独立模型只展示原始身份；完整名称由 N1 工程适配器提供。
    case 1: return presentationResolver_ ? presentationResolver_(*object).first : QString::fromUtf8(object->key.rawcode);
    case 2:
        return object->parentRawcode
            ? ToQString(*object->parentRawcode)
            : QStringLiteral("—");
    case 3: return static_cast<qulonglong>(object->modifications.size());
    default: return {};
    }
}


QVariant ObjectListModel::headerData(
    int section,
    Qt::Orientation orientation,
    int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }
    static const QStringList headers{
        tr("Rawcode"), tr("名称"), tr("父对象"), tr("字段数")};
    return section >= 0 && section < headers.size() ? headers[section] : QVariant{};
}


