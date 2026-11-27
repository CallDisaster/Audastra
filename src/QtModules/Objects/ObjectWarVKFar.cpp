// 物编装饰物的 WarVK“远景”勾选。设置保存在地图设置 → WarVK（地图成员 audastra\warvk.ini），
// 勾选与地图设置页、Agent 命令走同一工程命令；已标 16 个时第 17 个拒绝并提示，不截断。
#include "ObjectModule.h"
#include "EditorProject.h"
#include "ObjectModels.h"
#include "War3MapCore/ProductFeatures.h"

#include <QCheckBox>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>

namespace War3Map::Modules {

QWidget* ObjectModule::BuildFarDoodadToggle() {
    farDoodad_ = new QCheckBox(tr("远景"));
    farDoodad_->setObjectName(QStringLiteral("objectWarVKFar"));
    farDoodad_->setToolTip(tr("WarVK 远景类型 · 地图设置 → WarVK"));
    farDoodad_->setVisible(false);
    connect(farDoodad_, &QCheckBox::clicked, this, [this](bool far) {
        const auto code = CurrentObjectRawcode();
        if (code.isEmpty()) { UpdateFarDoodadToggle(); return; }
        // 发布会同步通知导入变化；推迟到事件循环下一轮，不在勾选框的信号处理里重建界面。
        QTimer::singleShot(0, this, [this, code, far] {
            const auto status = project_->SetWarVKFarDoodadType(code.toStdString(), far);
            if (!status.Succeeded() && !status.diagnostics.empty())
                ShowStatus(QString::fromStdString(status.diagnostics.front().message), 8000);
            UpdateFarDoodadToggle();
        });
    });
    connect(project_, &QtProject::EditorProject::ImportsChanged, this, &ObjectModule::UpdateFarDoodadToggle);
    if (auto* stack = project_->mapSettingsUndoStack_)
        connect(stack, &QUndoStack::indexChanged, this, &ObjectModule::UpdateFarDoodadToggle);
    return farDoodad_;
}

QString ObjectModule::CurrentObjectRawcode() const {
    if (objectSourceTabs_ && objectSourceTabs_->currentIndex() == 1) {
        if (!objectListView_ || !objectFilter_ || !objectListModel_) return {};
        const auto index = objectListView_->currentIndex();
        if (!index.isValid()) return {};
        const auto* object = objectListModel_->ObjectAt(objectFilter_->mapToSource(index).row());
        return object ? QString::fromStdString(object->key.rawcode) : QString{};
    }
    const auto entry = standardObjectBrowser_ ? standardObjectBrowser_->CurrentEntry() : std::nullopt;
    return entry ? entry->id : QString{};
}

void ObjectModule::UpdateFarDoodadToggle() {
    if (!farDoodad_) return;
    const bool doodads = WarVKFeaturesShown()
        && currentObjectType_ == static_cast<int>(ObjectData::ObjectType::Doodad) && !project_->projectRoot_.empty();
    farDoodad_->setVisible(doodads);
    if (!doodads) return;
    const auto code = CurrentObjectRawcode();
    const QSignalBlocker blocker(farDoodad_);
    if (code.isEmpty()) {
        farDoodad_->setEnabled(false);
        farDoodad_->setChecked(false);
        return;
    }
    const auto settings = project_->QueryWarVKMapSettings();
    const bool ok = settings.Succeeded() && settings.value;
    farDoodad_->setEnabled(ok);
    farDoodad_->setChecked(ok && std::ranges::find(settings.value->farDoodadTypes, code.toStdString())
        != settings.value->farDoodadTypes.end());
    if (!ok && !settings.diagnostics.empty())
        farDoodad_->setToolTip(QString::fromStdString(settings.diagnostics.front().message));
    else farDoodad_->setToolTip(tr("WarVK 远景类型 · 地图设置 → WarVK"));
}

} // namespace War3Map::Modules
