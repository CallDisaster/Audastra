#include "ObjectModule.h"
#include "ObjectModuleShared.h"
#include "ObjectModels.h"
#include "EditorProject.h"
#include "Widgets/ObjectTreeBrowser.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QStackedWidget>
#include <QTableView>
#include <QTabWidget>
#include <QTreeView>

#include <algorithm>
#include <limits>

namespace War3Map::Modules {
namespace {
bool Number(const QJsonObject& state, const char* key, int maximum = (std::numeric_limits<int>::max)()) {
    const auto value = state.value(QLatin1String(key));
    const auto number = value.toDouble(-1.0);
    return value.isDouble() && number >= 0 && number <= maximum && number == static_cast<int>(number);
}
bool Text(const QJsonObject& state, const char* key) {
    const auto value = state.value(QLatin1String(key));
    return value.isString() && value.toString().size() <= 4096 && !value.toString().contains(QChar{});
}
bool Rawcode(const QString& value) {
    // MDX/物编四字码以原始 ASCII 字节标识；不能用 Unicode 显示名或模糊名称回退。
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](QChar c) {
        return c.unicode() >= 32 && c.unicode() <= 126;
    });
}
} // namespace

std::optional<QJsonObject> ObjectModule::CaptureNavigationBookmark() const {
    if (!page_ || currentObjectType_ < 0 || currentObjectType_ >= 7) return std::nullopt;
    QString rawcode;
    const bool map = EditingMapObjects();
    if (map) {
        const auto proxy = objectListView_->currentIndex();
        const auto* object = proxy.isValid() ? objectListModel_->ObjectAt(objectFilter_->mapToSource(proxy).row()) : nullptr;
        if (!object) return std::nullopt;
        rawcode = QString::fromStdString(object->key.rawcode);
    } else {
        const auto entry = standardObjectBrowser_->CurrentEntry();
        if (!entry || entry->kind != ObjectDetail::Kind(static_cast<ObjectData::ObjectType>(currentObjectType_))) return std::nullopt;
        rawcode = entry->id;
    }
    if (!Rawcode(rawcode)) return std::nullopt;
    const auto* search = objectTree_->findChild<QLineEdit*>("objectTreeSearch");
    const auto* fields = qobject_cast<QTableView*>(objectDetailPages_->currentWidget());
    if (!fields || !search) return std::nullopt;
    return QJsonObject{{"schema", QStringLiteral("audastra.navigation.objects.v1")},
        {"type", currentObjectType_}, {"source", map ? QStringLiteral("map") : QStringLiteral("standard")},
        {"rawcode", rawcode}, {"search", search->text()}, {"fieldSearch", objectFieldSearch_->text()},
        {"fieldCategory", objectFieldCategory_->currentData().toString()},
        {"rawFields", showRawFields_->isChecked()}, {"showIds", objectTree_->ShowIds()},
        {"treeScroll", objectTree_->View()->verticalScrollBar()->value()},
        {"treeHorizontal", objectTree_->View()->horizontalScrollBar()->value()},
        {"fieldScroll", fields->verticalScrollBar()->value()},
        {"fieldHorizontal", fields->horizontalScrollBar()->value()}};
}

Shell::NavigationRestoreResult ObjectModule::RestoreNavigationBookmark(const QJsonObject& state) {
    using Result = Shell::NavigationRestoreResult;
    if (state.value("schema").toString() != QStringLiteral("audastra.navigation.objects.v1")
        || !Number(state, "type", 6) || !Text(state, "rawcode") || !Rawcode(state.value("rawcode").toString())
        || !Text(state, "search") || !Text(state, "fieldSearch") || !Text(state, "fieldCategory")
        || !state.value("rawFields").isBool() || !state.value("showIds").isBool()) return Result::Invalid;
    for (const auto* key : {"treeScroll", "treeHorizontal", "fieldScroll", "fieldHorizontal"})
        if (!Number(state, key)) return Result::Invalid;
    const auto type = state.value("type").toInt();
    const auto source = state.value("source").toString();
    const auto rawcode = state.value("rawcode").toString();
    const auto objectType = static_cast<ObjectData::ObjectType>(type);
    if (source == QLatin1String("map")) {
        const auto* document = project_->objectDocuments_[static_cast<std::size_t>(type)].get();
        if (!document || !document->FindObject({objectType, rawcode.toStdString()})) return Result::Invalid;
    } else if (source == QLatin1String("standard")) {
        if (!project_->standardCatalog_ || !project_->standardCatalog_->Find(objectType, rawcode.toStdString())) return Result::Invalid;
    } else return Result::Invalid;
    // stable category+rawcode+source 都通过后才改变视图；禁止调用会回退来源的 Reveal。
    (void)Widget();
    objectCategories_->setCurrentRow(type);
    if (currentObjectType_ != type) ShowObjectType(type);
    showRawFields_->setChecked(state.value("rawFields").toBool());
    objectTree_->SetShowIds(state.value("showIds").toBool());
    SetSearchText({});
    if (source == QLatin1String("map")) SelectMapObject(type, rawcode);
    else {
        // 目录隐藏页仍可能保有先前的分类筛选；书签 anchor 先定位真实标准对象。
        for (const auto* name : {"objectCatalogRaces", "objectCatalogCategories"}) {
            if (auto* combo = standardObjectBrowser_->findChild<QComboBox*>(name)) combo->setCurrentIndex(0);
        }
        objectSourceTabs_->setCurrentIndex(0); standardObjectBrowser_->SetCurrentId(rawcode);
        const auto entry = standardObjectBrowser_->CurrentEntry();
        if (!entry || entry->id != rawcode || entry->kind != ObjectDetail::Kind(objectType)) return Result::Invalid;
        ShowStandardObject(*entry);
    }
    objectFieldSearch_->setText(state.value("fieldSearch").toString());
    const int category = objectFieldCategory_->findData(state.value("fieldCategory").toString());
    objectFieldCategory_->setCurrentIndex(category < 0 ? 0 : category);
    // 过滤值可能因对象重命名而不再匹配；保留作者视图过滤，不改文档或换选邻对象。
    objectTree_->SetSearchText(state.value("search").toString());
    objectTree_->View()->verticalScrollBar()->setValue(state.value("treeScroll").toInt());
    objectTree_->View()->horizontalScrollBar()->setValue(state.value("treeHorizontal").toInt());
    auto* fields = qobject_cast<QTableView*>(objectDetailPages_->currentWidget());
    if (!fields) return Result::Invalid;
    fields->verticalScrollBar()->setValue(state.value("fieldScroll").toInt());
    fields->horizontalScrollBar()->setValue(state.value("fieldHorizontal").toInt());
    return Result::Restored;
}
} // namespace War3Map::Modules
