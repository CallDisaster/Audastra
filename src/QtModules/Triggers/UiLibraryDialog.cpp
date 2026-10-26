#include "UiLibraryDialog.h"

#include "BundledUiLibrary.h"
#include "EditorProject.h"
#include "Widgets/ColumnLayout.h"

#include "War3MapCore/Trigger/UiCatalog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace War3Map::Modules {
namespace {

enum Column { kEnabled, kMapOnly, kPackage, kSource, kFunctions, kStatus, kFiles, kColumnCount };

QString Text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

bool Contains(const std::vector<std::string>& values, const std::string& id) {
    return std::find(values.begin(), values.end(), id) != values.end();
}

} // namespace

UiLibraryDialog::UiLibraryDialog(const QtProject::EditorProject& project, QWidget* parent)
    : QDialog(parent), project_(project), settings_(project.uiLibraryGlobal_), mapOnly_(project.uiLibraryMapOnly_) {
    setWindowTitle(tr("函数库"));
    resize(820, 520);
    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    auto* packagePage = new QWidget(tabs);
    auto* packageLayout = new QVBoxLayout(packagePage);
    packages_ = new QTableWidget(0, kColumnCount, packagePage);
    packages_->setObjectName("uiLibraryPackages");
    packages_->setHorizontalHeaderLabels({tr("启用"), tr("仅本图"), tr("函数库"), tr("来源"), tr("本图函数"), tr("本图状态"), tr("文件")});
    packages_->setSelectionBehavior(QAbstractItemView::SelectRows);
    packages_->setSelectionMode(QAbstractItemView::SingleSelection);
    packages_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    packages_->verticalHeader()->hide();
    UiKit::ConfigureResizableColumns(packages_->horizontalHeader(), {220});
    packageLayout->addWidget(packages_, 1);
    auto* orderRow = new QHBoxLayout;
    auto* up = new QPushButton(tr("上移"), packagePage);
    auto* down = new QPushButton(tr("下移"), packagePage);
    orderRow->addWidget(up);
    orderRow->addWidget(down);
    orderRow->addStretch();
    installBundled_ = new QPushButton(packagePage);
    installBundled_->setObjectName("uiLibraryInstallBundled");
    orderRow->addWidget(installBundled_);
    packageLayout->addLayout(orderRow);
    tabs->addTab(packagePage, tr("包"));

    auto* conflicts = new QListWidget(tabs);
    conflicts->setObjectName("uiLibraryConflicts");
    if (project.uiCatalog_) {
        for (const auto& conflict : UiLibrary::FindConflicts(*project.uiCatalog_)) {
            QStringList providers;
            for (const auto& layer : conflict.layers) providers << Text(layer);
            new QListWidgetItem(tr("%1 %2 · 生效：%3 · 提供者：%4")
                .arg(Text(Trigger::ToString(conflict.kind)), Text(conflict.name), Text(conflict.activeLayer),
                    providers.join(QStringLiteral(" → "))), conflicts);
        }
    }
    tabs->addTab(conflicts, tr("冲突 (%1)").arg(conflicts->count()));
    layout->addWidget(tabs, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("应用并重新载入触发器"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(up, &QPushButton::clicked, this, [this]() { MoveSelected(-1); });
    connect(down, &QPushButton::clicked, this, [this]() { MoveSelected(1); });
    connect(installBundled_, &QPushButton::clicked, this, [this]() { InstallBundled(); });
    connect(packages_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const int row = item->row();
        if (row < 0 || row >= static_cast<int>(settings_.packages.size())) return;
        const std::string& id = settings_.packages[static_cast<std::size_t>(row)].id;
        if (item->column() == kEnabled) {
            settings_.packages[static_cast<std::size_t>(row)].enabled = item->checkState() == Qt::Checked;
        } else if (item->column() == kMapOnly) {
            const bool on = item->checkState() == Qt::Checked;
            if (on && !Contains(mapOnly_, id)) mapOnly_.push_back(id);
            if (!on) mapOnly_.erase(std::remove(mapOnly_.begin(), mapOnly_.end(), id), mapOnly_.end());
        }
    });
    Populate();
    UpdateInstallButton();
}

void UiLibraryDialog::UpdateInstallButton() {
    const QString name = Text(QtProject::kBundledUiLibrary);
    const auto check = QtProject::CheckBundledUiLibrary(project_.ResolveUiDataRoot());
    installBundled_->setEnabled(check.error.isEmpty() && check.state != QtProject::BundledUiLibraryState::Same);
    if (!check.error.isEmpty()) installBundled_->setText(tr("安装 %1").arg(name));
    else if (check.state == QtProject::BundledUiLibraryState::Missing) installBundled_->setText(tr("安装 %1").arg(name));
    else if (check.state == QtProject::BundledUiLibraryState::Different) installBundled_->setText(tr("更新 %1").arg(name));
    else installBundled_->setText(tr("%1 已是最新").arg(name));
}

// 写进数据根后在本次设置里启用它；点“应用并重新载入触发器”时与其他包一起生效。
void UiLibraryDialog::InstallBundled() {
    const auto dataRoot = project_.ResolveUiDataRoot();
    const auto check = QtProject::CheckBundledUiLibrary(dataRoot);
    if (!check.error.isEmpty()) {
        QMessageBox::warning(this, tr("函数库"), check.error);
        return;
    }
    const QString target = QDir::toNativeSeparators(QString::fromStdWString(check.target.wstring()));
    QString question = tr("把编辑器自带的 %1 函数库写入\n%2\n不修改 config。").arg(Text(QtProject::kBundledUiLibrary), target);
    if (!check.injectionTarget.empty())
        question += tr("\n注入代码另写一份到\n%1").arg(QDir::toNativeSeparators(QString::fromStdWString(check.injectionTarget.wstring())));
    if (check.state == QtProject::BundledUiLibraryState::Different) question += tr("\n目录里同名的文件会被替换。");
    if (QMessageBox::question(this, tr("函数库"), question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (const QString error = QtProject::InstallBundledUiLibrary(dataRoot); !error.isEmpty()) {
        QMessageBox::warning(this, tr("函数库"), error);
        UpdateInstallButton();
        return;
    }
    const std::string id(QtProject::kBundledUiLibrary);
    const auto entry = std::find_if(settings_.packages.begin(), settings_.packages.end(), [&id](const auto& value) { return value.id == id; });
    if (entry == settings_.packages.end()) settings_.packages.push_back({id, true, {}});
    else entry->enabled = true;
    Populate();
    UpdateInstallButton();
}

void UiLibraryDialog::Populate() {
    const QSignalBlocker blocker(packages_);
    packages_->setRowCount(static_cast<int>(settings_.packages.size()));
    std::vector<UiLibrary::PackageCheck> checks;
    if (project_.uiLibraryUsage_) {
        checks = UiLibrary::CheckMapUsage(*project_.uiLibraryUsage_, settings_, project_.uiLibraryPackages_);
    }
    for (int row = 0; row < packages_->rowCount(); ++row) {
        const auto& entry = settings_.packages[static_cast<std::size_t>(row)];
        const auto package = std::find_if(project_.uiLibraryPackages_.begin(), project_.uiLibraryPackages_.end(),
            [&entry](const auto& value) { return value.id == entry.id; });
        const UiLibrary::MapUsageEntry* usage = nullptr;
        if (project_.uiLibraryUsage_) {
            for (const auto& value : project_.uiLibraryUsage_->packages) {
                if (value.id == entry.id) usage = &value;
            }
        }
        const bool used = usage != nullptr;
        const auto check = std::find_if(checks.begin(), checks.end(), [&entry](const auto& value) { return value.id == entry.id; });

        auto* enabled = new QTableWidgetItem;
        enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        enabled->setCheckState(entry.enabled ? Qt::Checked : Qt::Unchecked);
        packages_->setItem(row, kEnabled, enabled);
        auto* mapOnly = new QTableWidgetItem;
        mapOnly->setFlags(entry.enabled ? Qt::ItemIsSelectable : Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        mapOnly->setCheckState(!entry.enabled && Contains(mapOnly_, entry.id) ? Qt::Checked : Qt::Unchecked);
        packages_->setItem(row, kMapOnly, mapOnly);
        packages_->setItem(row, kPackage, new QTableWidgetItem(Text(entry.id)));
        packages_->setItem(row, kSource, new QTableWidgetItem(
            package != project_.uiLibraryPackages_.end() && package->listedInConfig ? tr("config") : tr("未列出")));
        packages_->setItem(row, kFunctions, new QTableWidgetItem(used ? QString::number(usage->functionCount) : QStringLiteral("-")));
        QString status = tr("未使用");
        if (check != checks.end()) {
            switch (check->status) {
            case UiLibrary::PackageStatus::Same: status = tr("一致"); break;
            case UiLibrary::PackageStatus::Changed: status = tr("内容已变化"); break;
            case UiLibrary::PackageStatus::Missing: status = tr("本机缺失"); break;
            case UiLibrary::PackageStatus::DisabledLocally:
                status = Contains(mapOnly_, entry.id) ? tr("仅对此地图启用") : tr("本机已关闭");
                break;
            }
        }
        packages_->setItem(row, kStatus, new QTableWidgetItem(status));
        packages_->setItem(row, kFiles, new QTableWidgetItem(
            package != project_.uiLibraryPackages_.end() ? QString::number(package->fileCount) : QStringLiteral("-")));
    }
    packages_->resizeColumnsToContents();
}

void UiLibraryDialog::MoveSelected(int delta) {
    const int row = packages_->currentRow();
    const int target = row + delta;
    if (row < 0 || target < 0 || target >= packages_->rowCount()) return;
    std::swap(settings_.packages[static_cast<std::size_t>(row)], settings_.packages[static_cast<std::size_t>(target)]);
    Populate();
    packages_->selectRow(target);
}

UiLibrary::GlobalSettings UiLibraryDialog::Settings() const {
    UiLibrary::GlobalSettings settings = settings_;
    // 记录本次确认时的内容指纹。
    for (auto& entry : settings.packages) {
        const auto package = std::find_if(project_.uiLibraryPackages_.begin(), project_.uiLibraryPackages_.end(),
            [&entry](const auto& value) { return value.id == entry.id; });
        if (package != project_.uiLibraryPackages_.end()) entry.fingerprint = package->fingerprint;
    }
    return settings;
}

std::vector<std::string> UiLibraryDialog::MapOnly() const {
    std::vector<std::string> result;
    for (const auto& entry : settings_.packages) {
        if (!entry.enabled && Contains(mapOnly_, entry.id)) result.push_back(entry.id);
    }
    return result;
}

} // namespace War3Map::Modules
