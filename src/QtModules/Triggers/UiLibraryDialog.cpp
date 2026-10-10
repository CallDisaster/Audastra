#include "UiLibraryDialog.h"

#include "EditorProject.h"
#include "Widgets/ColumnLayout.h"

#include "War3MapCore/Trigger/UiCatalog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
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
    auto* hint = new QLabel(tr("启用与顺序保存在本机设置中，不修改数据根的 config；后加载的包覆盖同名定义。"
                               "地图用到但本机关闭的包可以仅对此地图启用。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

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
