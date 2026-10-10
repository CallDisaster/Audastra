#include "MainWindow.h"
#include "MapSaveJob.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "Widgets/ChangesPanel.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"

#include <QAction>
#include <QDockWidget>
#include <QPointer>
#include <QStandardItemModel>
#include <QThread>
#include <QTimer>

#include <algorithm>

namespace {
namespace Services = War3Map::Services;
namespace MapData = War3Map::MapData;
namespace Objects = War3Map::ObjectData;
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString Value(const Services::ProjectChangeRow& row, const std::optional<MapData::MapFieldValue>& value) {
    if (!value) return {};
    const auto* field = MapData::FindMapField(row.member, row.fieldId);
    if (field && MapData::IsMapFieldDefault(*field, *value)) return QObject::tr("默认");
    if (value->type == MapData::MapFieldType::Int32 || value->type == MapData::MapFieldType::UInt32)
        return QString::number(value->integer);
    if (value->type == MapData::MapFieldType::Bytes) {
        const auto text = Text(value->bytes);
        if (text.toStdString() == value->bytes) return text;
    } else if (const auto number = MapData::MapFieldNumber(*value)) {
        return QString::number(*number, 'g', 9);
    }
    return Text(MapData::FormatMapFieldRaw(*value));
}
QStringList Diagnostics(const std::vector<War3Map::Diagnostic>& diagnostics) {
    QStringList result;
    for (const auto& item : diagnostics) {
        if (result.size() >= 128) { result.append(QObject::tr("还有诊断未列出，请查看诊断面板。")); break; }
        result.append((Text(item.subsystem) + " / " + Text(item.code) + " · " + Text(item.message)).left(4096));
    }
    return result;
}
QString ObjectTypeLabel(Objects::ObjectType type) {
    const QStringList names{QObject::tr("单位"), QObject::tr("技能"), QObject::tr("物品"), QObject::tr("魔法效果"),
        QObject::tr("科技"), QObject::tr("装饰物"), QObject::tr("可破坏物")};
    return names.value(static_cast<int>(type), QObject::tr("物体"));
}
QString ContentValue(const std::optional<Objects::Value>& value) {
    return value ? Text(Objects::ValueToDisplayString(*value)) : QObject::tr("无地图覆盖（继承）");
}
} // namespace

void MainWindow::SetupChangesPanel() {
    changesDock_ = new QDockWidget(tr("修改"), this);
    changesDock_->setObjectName("projectChangesDock");
    changesDock_->setAllowedAreas(Qt::RightDockWidgetArea);
    changesDock_->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    changesPanel_ = new ChangesPanel(changesDock_);
    changesDock_->setWidget(changesPanel_);
    addDockWidget(Qt::RightDockWidgetArea, changesDock_);
    resizeDocks({changesDock_}, {420}, Qt::Horizontal);
    changesDock_->hide();
    connect(changesPanel_, &ChangesPanel::SaveRequested, this, [this]() { ExecuteCommand("file.save.map"); });
    connect(changesPanel_, &ChangesPanel::CheckSaveRequested, this, &MainWindow::CheckChangesSave);
    connect(changesDock_, &QDockWidget::visibilityChanged, this, [this](bool shown) {
        changesAction_->setChecked(shown);
        if (shown) RefreshChangesPanel();
    });
    RefreshChangesPanel();
}

void MainWindow::ScheduleChangesRefresh() {
    if (!changesPanel_ || changesRefreshQueued_) return;
    // 修改明细可能比较整图实例与触发器。隐藏面板不占用摆放/播放的事件循环；
    // 标记数量待计算，展开仍通过同一完整查询得到精确结果，不复用旧行的撤销身份。
    if (!changesDock_->isVisible()) {
        changesAction_->setText(tr("修改"));
        changesAction_->setToolTip(tr("展开修改面板后计算当前明细；保存仍执行完整内容核验。"));
        return;
    }
    changesRefreshQueued_ = true;
    QTimer::singleShot(0, this, [this]() {
        changesRefreshQueued_ = false;
        if (changesDock_->isVisible()) RefreshChangesPanel();
    });
}

void MainWindow::ShowChangesPanel() {
    if (changesDock_->isVisible()) {
        changesDock_->hide();
        return;
    }
    changesDock_->show(); changesDock_->raise(); RefreshChangesPanel();
}

void MainWindow::RefreshChangesPanel() {
    if (!changesPanel_) return;
    std::vector<ChangesPanel::Entry> entries;
    QStringList unsupported{tr("导入资源"), tr("地形与通行性"), tr("字符串表")};
    bool incomplete = false;
    std::size_t knownCount = 0;
    const auto error = RefreshCommandMapSnapshot();
    if (!error.isEmpty()) { unsupported.append(error); incomplete = true; }
    if (error.isEmpty() && commandMapSnapshot_ && commandMapDocument_ && project_->mapDocument_) {
        const auto binding = commandMapSnapshot_->Binding();
        const Services::ProjectChangesContext context{project_->mapDocument_.get(), commandMapDocument_.get(), binding};
        Services::ProjectChangesOptions options; options.limit = 32;
        const auto appendPage = [&](const Services::ProjectChangesPage& page, const QString& memberName) {
            knownCount += page.totalRows;
            incomplete = incomplete || page.next.has_value() || page.member.partial;
            if (page.member.partial) unsupported.append(memberName + "：" + Text(page.member.reason));
            for (const auto& row : page.rows) {
                if (entries.size() >= 256) { incomplete = true; break; }
                ChangesPanel::Entry entry;
                entry.id = Text(row.id); entry.member = memberName;
                entry.record = row.rowKey.empty() ? tr("成员内容") : Text(row.rowKey);
                entry.title = Text(row.label);
                if (row.domain == Services::ProjectChangeDomain::Objects) {
                    entry.before = row.textTruncated && !row.objectBefore ? tr("原值超过展示上限") : ContentValue(row.objectBefore);
                    entry.after = row.textTruncated && !row.objectAfter ? tr("当前值超过展示上限") : ContentValue(row.objectAfter);
                } else if (row.domain == Services::ProjectChangeDomain::Triggers || row.domain == Services::ProjectChangeDomain::Variables) {
                    entry.record = Text(row.label) + tr(" · 标识 %1").arg(row.entityId);
                    entry.title = row.domain == Services::ProjectChangeDomain::Triggers ? tr("%1 处变化").arg(row.changedCount) : tr("变量声明");
                    entry.before = Text(row.beforeText); entry.after = Text(row.afterText);
                } else {
                    entry.before = Value(row, row.before); entry.after = Value(row, row.after);
                }
                entry.reason = Text(row.detail + (row.detail.empty() ? "" : "\n") + row.revertReason);
                if (row.before && row.after && *row.before != *row.after && entry.before == entry.after)
                    entry.reason += tr("\n显示值相同，精确原值：%1 → %2")
                        .arg(Text(MapData::FormatMapFieldRaw(*row.before)), Text(MapData::FormatMapFieldRaw(*row.after)));
                entry.kind = row.kind == Services::ProjectChangeKind::Added ? ChangesPanel::Kind::Added
                    : row.kind == Services::ProjectChangeKind::Removed ? ChangesPanel::Kind::Removed
                    : row.kind == Services::ProjectChangeKind::Member ? ChangesPanel::Kind::Member : ChangesPanel::Kind::Modified;
                entry.canRevert = row.revertible && !project_->savingMap_ && !changesChecking_;
                if ((row.currentIndex || row.object || row.entityId) && row.kind != Services::ProjectChangeKind::Member)
                    entry.reveal = [this, row]() { RevealProjectChange(row); };
                if (entry.canRevert) entry.revert = [this, row]() { RevertProjectChange(row); };
                entries.push_back(std::move(entry));
            }
        };
        for (const auto kind : {War3Map::MapDataKind::Settings, War3Map::MapDataKind::Units,
            War3Map::MapDataKind::Doodads, War3Map::MapDataKind::Regions, War3Map::MapDataKind::Cameras,
            War3Map::MapDataKind::Sounds}) {
            const auto page = Services::QueryProjectChanges(context, binding, kind, std::nullopt, options);
            if (!page.Succeeded()) { unsupported.append(Diagnostics(page.diagnostics)); incomplete = true; continue; }
            appendPage(*page.value, Text(page.value->member.memberName));
        }
        const auto content = project_->CaptureContentChangesContext(binding);
        if (!content.Succeeded()) { unsupported.append(Diagnostics(content.diagnostics)); incomplete = true; }
        else {
            for (int i = 0; i < 7; ++i) {
                const auto type = static_cast<Objects::ObjectType>(i);
                const auto page = Services::QueryProjectContentChanges(*content.value, binding,
                    Services::ProjectChangeDomain::Objects, type, std::nullopt, options);
                if (!page.Succeeded()) { unsupported.append(Diagnostics(page.diagnostics)); incomplete = true; continue; }
                appendPage(*page.value, tr("物编 · ") + ObjectTypeLabel(type));
            }
            for (const auto domain : {Services::ProjectChangeDomain::Triggers, Services::ProjectChangeDomain::Variables}) {
                const auto page = Services::QueryProjectContentChanges(*content.value, binding, domain,
                    Objects::ObjectType::Unit, std::nullopt, options);
                if (!page.Succeeded()) { unsupported.append(Diagnostics(page.diagnostics)); incomplete = true; continue; }
                appendPage(*page.value, domain == Services::ProjectChangeDomain::Triggers ? tr("触发器") : tr("变量"));
            }
        }
    } else { unsupported.append(tr("当前地图成员尚未载入")); incomplete = true; }
    // 未接入范围有未保存修改时，不能把已列出的数量说成工程总数。
    const bool otherEdits = project_->HasPendingAssetEdits();
    const auto scopes = project_->editRevision_.Current();
    const bool otherMapEdits = project_->mapEdits_ && project_->mapEdits_->IsDirty()
        && (scopes.contains("map/war3map.w3e") || scopes.contains("map/war3map.wpm") || scopes.contains("map/war3map.wts"));
    incomplete = incomplete || otherEdits || otherMapEdits;
    const auto count = entries.size();
    changesAction_->setText(tr("修改"));
    changesAction_->setToolTip(tr("已确认 %1 项，列出 %2 项%3。未应用草稿不计入数量。")
        .arg(knownCount).arg(count).arg(incomplete ? tr("，另有未展开范围") : QString{}));
    changesPanel_->SetEntries(std::move(entries), unsupported, incomplete);
    changesPanel_->SetActionsAvailable(!changesChecking_ && !project_->savingMap_ && saveMapInPlaceAction_->isEnabled(),
        !changesChecking_ && !project_->savingMap_ && project_->archiveSession_ != nullptr,
        changesChecking_ ? tr("正在检查冻结的编辑内容…") : project_->archiveSession_ ?
            tr("检查不会写出地图；未应用草稿需先在所属模块确认。") : tr("当前没有可供地图保存预检的归档会话。"));
}

void MainWindow::RevertProjectChange(const Services::ProjectChangeRow& row) {
    const auto error = RefreshCommandMapSnapshot();
    if (!error.isEmpty() || !commandMapSnapshot_ || row.binding != commandMapSnapshot_->Binding()) {
        ShowStatusMessage(tr("工程已变化，请刷新修改列表")); return;
    }
    if (changesChecking_ || settingsModule_->HasPendingDraft() || soundsModule_->HasPendingDraft()) {
        ShowStatusMessage(tr("请先完成保存检查或确认设置和声音草稿，再撤销这项")); return;
    }
    const auto status = project_->RevertProjectChange(row, commandMapSnapshot_->Binding());
    if (!status.Succeeded()) {
        project_->AddDiagnostics(status.diagnostics);
        ShowStatusMessage(Diagnostics(status.diagnostics).join("；"));
    } else {
        ScheduleRecovery(); UpdateEditDirtyFlags(); UpdateExportAvailability();
        ShowStatusMessage(tr("已撤销这项 · 可用普通撤销恢复"));
    }
    RefreshChangesPanel();
}

void MainWindow::RevealProjectChange(const Services::ProjectChangeRow& row) {
    if (!RefreshCommandMapSnapshot().isEmpty() || !commandMapSnapshot_ || row.binding != commandMapSnapshot_->Binding()) {
        ShowStatusMessage(tr("工程已变化，请刷新修改列表")); return;
    }
    if (row.domain != Services::ProjectChangeDomain::MapData && row.editorRevision != project_->editRevision_.Revision()) {
        ShowStatusMessage(tr("内容已变化，请刷新修改列表")); return;
    }
    if (row.domain == Services::ProjectChangeDomain::Objects && row.object) {
        Navigate({"objects", "object", Text(std::string(Objects::ToString(row.object->type)) + ":" + row.object->rawcode), {}}); return;
    }
    if (row.domain == Services::ProjectChangeDomain::Triggers || row.domain == Services::ProjectChangeDomain::Variables) {
        Navigate({row.domain == Services::ProjectChangeDomain::Triggers ? "triggers" : "variables",
            row.domain == Services::ProjectChangeDomain::Triggers ? "trigger" : "variable",
            QString::number(row.entityId), {{"stableIdentity", true}}}); return;
    }
    if (!row.currentIndex) return;
    if (row.member == War3Map::MapDataKind::Settings) ExecuteCommand("module.map-settings");
    else {
        const auto sourceIndex = QString::number(*row.currentIndex);
        if (row.member == War3Map::MapDataKind::Sounds) Navigate({"sounds", "sound", {}, {{"sourceIndex", sourceIndex}}});
        else {
            const QString kind = row.member == War3Map::MapDataKind::Units ? "unit" : row.member == War3Map::MapDataKind::Doodads
                ? "doodad" : row.member == War3Map::MapDataKind::Regions ? "region" : "camera";
            Navigate({"entities", kind, {}, {{"sourceIndex", sourceIndex}}});
        }
    }
}

void MainWindow::CheckChangesSave() {
    if (changesChecking_ || project_->savingMap_ || !project_->archiveSession_) return;
    const auto firstDiagnostic = project_->diagnosticsModel_->rowCount();
    auto prepared = project_->PrepareMapPreflight();
    if (!prepared) {
        QStringList details;
        for (int row = firstDiagnostic; row < project_->diagnosticsModel_->rowCount() && details.size() < 128; ++row)
            details.append(project_->diagnosticsModel_->index(row, 2).data().toString() + " · "
                + project_->diagnosticsModel_->index(row, 3).data().toString().left(4096));
        changesPanel_->SetPreflightResult(tr("保存准备被拒绝 · 未写出地图"), details); return;
    }
    const auto revision = CommandContextRevision();
    auto job = std::shared_ptr<War3Map::QtProject::MapSaveJob>(std::move(prepared));
    changesChecking_ = true; RefreshChangesPanel();
    changesPanel_->SetPreflightResult(tr("正在检查保存…"));
    const QPointer<MainWindow> guarded(this);
    auto* worker = QThread::create([job]() { (void)Services::MapSaveService::Preflight(*job); });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [guarded, job, revision]() {
        if (!guarded) return;
        guarded->changesChecking_ = false; guarded->RefreshChangesPanel();
        if (guarded->CommandContextRevision() != revision) {
            guarded->changesPanel_->SetPreflightResult(guarded->tr("预检结果已过期，请重新检查 · 未写出地图")); return;
        }
        guarded->project_->AddDiagnostics(job->diagnostics);
        const War3Map::Status status{job->diagnostics};
        auto details = Diagnostics(job->diagnostics);
        details.prepend(guarded->tr("仅检查已提交的编辑；不含未应用草稿。尚未验证输出权限、压缩后大小、回读或备份。"));
        guarded->changesPanel_->SetPreflightResult(status.Succeeded() ? guarded->tr("内容预检通过 · 尚未写出地图")
            : guarded->tr("保存预检被拒绝 · 未写出地图"), details);
    });
    worker->start(QThread::LowPriority);
}
