#include "ObjectModule.h"
#include "ObjectModuleShared.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "ObjectClassification.h"
#include "ObjectModels.h"
#include "Widgets/ObjectTreeBrowser.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QHash>
#include <QCheckBox>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QTabWidget>

// 原版风格的物体目录树与原有状态控件（标准目录 / 地图物体列表，隐藏）之间的同步。
// 树只负责浏览与选择；字段显示、创建、复制和撤销仍走原来的流程。
namespace War3Map::Modules {

using namespace ObjectData;
using namespace QtApp;
using namespace ObjectDetail;

void ObjectModule::RebuildObjectTree() {
    if (objectTree_ == nullptr || currentObjectType_ < 0 || currentObjectType_ >= 7) {
        return;
    }
    const auto type = static_cast<ObjectType>(currentObjectType_);
    const QString kind = Kind(type);
    const auto* document = project_->objectDocuments_[static_cast<std::size_t>(currentObjectType_)].get();
    const auto* catalog = project_->standardCatalog_.get();
    // 地图物体的显示名与图标已在 objectViewEntries_ 中按父链解析好。
    QHash<QString, const ObjectViewEntry*> mapEntries;
    for (const auto& entry : objectViewEntries_) {
        if (entry.kind == kind) mapEntries.insert(entry.id, &entry);
    }
    std::vector<ObjectTreeBrowser::Object> objects;
    if (catalog != nullptr) {
        for (const auto& entry : catalog->Entries()) {
            if (entry.key.type != type) continue;
            ObjectTreeBrowser::Object object;
            object.id = U8(entry.key.rawcode);
            const auto* record = document != nullptr ? document->FindObject(entry.key) : nullptr;
            object.modified = record != nullptr && !record->parentRawcode;
            const auto* shown = object.modified ? mapEntries.value(object.id) : nullptr;
            object.name = showRawFields_->isChecked() ? object.id : shown != nullptr ? shown->name : U8(entry.displayName);
            object.iconPath = showRawFields_->isChecked() ? QString{} : shown != nullptr ? shown->iconPath : U8(entry.iconPath);
            object.folders = ClassifyStandard(entry);
            if (object.modified) object.tooltip = tr("已修改 %1 个字段").arg(record->modifications.size());
            objects.push_back(std::move(object));
        }
    }
    if (document != nullptr) {
        for (const auto& record : document->Objects()) {
            const bool standard = catalog != nullptr && catalog->Find(type, record.key.rawcode) != nullptr;
            if (!record.parentRawcode && standard) continue; // 已作为“已修改的标准对象”列出
            ObjectTreeBrowser::Object object;
            object.id = U8(record.key.rawcode);
            object.custom = true;
            const auto* shown = mapEntries.value(object.id);
            const auto presentation = shown ? ObjectPresentation{} : ResolveProjectObjectPresentation(*project_, record.key);
            object.name = showRawFields_->isChecked() ? object.id : shown != nullptr ? shown->name : presentation.name;
            object.iconPath = showRawFields_->isChecked() ? QString{} : shown != nullptr ? shown->iconPath : presentation.iconPath;
            const auto chain = ResolveObjectInheritance(*document, record.key, catalog);
            object.folders = ClassifyCustom(chain, type, record.key.rawcode);
            object.tooltip = record.parentRawcode ? tr("基于 %1").arg(U8(*record.parentRawcode)) : tr("找不到基础对象");
            if (shown) object.tooltip += QLatin1Char('\n') + shown->details.join(QLatin1Char('\n'));
            else if (!presentation.issue.isEmpty()) object.tooltip += QLatin1Char('\n') + presentation.issue;
            objects.push_back(std::move(object));
        }
    }
    objectTree_->SetObjects(kind, RootLabel(type, false), RootLabel(type, true), std::move(objects));
    SyncObjectTreeSelection();
}

void ObjectModule::OnObjectTreeSelected(const QString& id, bool) {
    const auto type = static_cast<ObjectType>(currentObjectType_);
    const auto* document = project_->objectDocuments_[static_cast<std::size_t>(currentObjectType_)].get();
    syncingObjectTree_ = true;
    if (document != nullptr && document->FindObject({type, id.toStdString()}) != nullptr) {
        // 自定义物体与修改过的标准物体：直接进入可编辑的字段表。
        SelectMapObject(currentObjectType_, id);
    } else {
        objectSourceTabs_->setCurrentIndex(0);
        standardObjectBrowser_->SetCurrentId(id);
        if (const auto entry = standardObjectBrowser_->CurrentEntry()) ShowStandardObject(*entry);
    }
    syncingObjectTree_ = false;
    UpdateObjectFieldControls();
}

void ObjectModule::SyncObjectTreeSelection() {
    if (objectTree_ == nullptr || syncingObjectTree_) {
        return;
    }
    if (objectSourceTabs_->currentIndex() == 1) {
        const QModelIndex proxy = objectListView_->currentIndex();
        const auto* record = proxy.isValid() ? objectListModel_->ObjectAt(objectFilter_->mapToSource(proxy).row()) : nullptr;
        if (record != nullptr) {
            const bool standard = project_->standardCatalog_ != nullptr &&
                project_->standardCatalog_->Find(record->key.type, record->key.rawcode) != nullptr;
            objectTree_->Select(U8(record->key.rawcode), record->parentRawcode.has_value() || !standard);
        }
        return;
    }
    if (const auto entry = standardObjectBrowser_->CurrentEntry()) {
        objectTree_->Select(entry->id, false);
    }
}

void ObjectModule::ShowObjectTreeMenu(const QPoint& globalPos) {
    const auto current = objectTree_->Current();
    const auto type = static_cast<ObjectType>(currentObjectType_);
    const auto* document = project_->objectDocuments_[static_cast<std::size_t>(currentObjectType_)].get();
    const auto* record = current && document != nullptr ? document->FindObject({type, current->first.toStdString()}) : nullptr;
    QMenu menu(page_);
    QAction* references = menu.addAction(tr("查看引用（物体管理）"));
    references->setEnabled(current.has_value() && host_ != nullptr);
    menu.addSeparator();
    QAction* create = menu.addAction(tr("新建自定义物体…"));
    QAction* clone = menu.addAction(tr("复制自定义物体"));
    clone->setEnabled(record != nullptr && record->parentRawcode.has_value());
    QAction* modify = nullptr;
    if (current && record == nullptr) modify = menu.addAction(tr("修改此对象"));
    menu.addSeparator();
    QAction* remove = menu.addAction(tr("删除"));
    remove->setEnabled(false);
    remove->setToolTip(tr("尚未接入完整引用审计，删除暂不可用。"));
    menu.setToolTipsVisible(true);
    const QAction* chosen = menu.exec(globalPos);
    if (chosen == nullptr) {
        return;
    }
    if (chosen == references) {
        host_->Navigate({"manager", "object", Kind(type) + ':' + current->first, {}});
    } else if (chosen == create) {
        CreateNewCustomObject();
    } else if (chosen == clone) {
        DuplicateSelectedCustomObject();
    } else if (chosen == modify) {
        EditSelectedStandardObject();
    }
}

} // namespace War3Map::Modules
