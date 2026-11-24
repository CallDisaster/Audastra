#include "WorldEffectsSettingsPage.h"
#include "War3MapCore/Render/RenderAdapters.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

namespace War3Map::QtApp {

WorldEffectsSettingsPage::WorldEffectsSettingsPage(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("worldEffectsSettingsPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* group = new QGroupBox(tr("世界视图"), this);
    auto* form = new QFormLayout(group);
    form->setContentsMargins(16, 18, 16, 16);
    form->setSpacing(12);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    quality_ = new QComboBox(group);
    quality_->setObjectName(QStringLiteral("worldEffectQuality"));
    quality_->addItem(tr("完整"), QStringLiteral("complete"));
    quality_->addItem(tr("平衡"), QStringLiteral("balanced"));
    quality_->addItem(tr("省电"), QStringLiteral("power-saving"));
    const auto configured = QSettings{}.value(QStringLiteral("world/effectQuality"), QStringLiteral("balanced")).toString();
    const int index = quality_->findData(configured);
    quality_->setCurrentIndex(index >= 0 ? index : 1);
    form->addRow(tr("特效显示"), quality_);
    // 显卡：默认跟随系统；列出本机硬件显卡。已保存的卡不在本机时仍列出并标明不可用，视口会退回系统默认。
    adapter_ = new QComboBox(group);
    adapter_->setObjectName(QStringLiteral("worldRenderAdapter"));
    adapter_->addItem(tr("跟随系统"), QString());
    const QSettings settings;
    const auto savedKey = settings.value(QStringLiteral("world/renderAdapter")).toString();
    const auto savedName = settings.value(QStringLiteral("world/renderAdapterName")).toString();
    if (const auto adapters = War3Map::Render::EnumerateRenderAdapters(); adapters.Succeeded()) {
        for (const auto& adapter : *adapters.value) {
            const auto name = QString::fromStdString(adapter.name);
            const auto memory = adapter.dedicatedVideoMemory / (1024ull * 1024ull);
            adapter_->addItem(memory ? tr("%1（%2 MB）").arg(name).arg(memory) : name, QString::fromStdString(adapter.key));
            adapter_->setItemData(adapter_->count() - 1, name, Qt::UserRole + 1);
        }
    }
    int selected = adapter_->findData(savedKey);
    if (!savedKey.isEmpty() && selected < 0) {
        adapter_->addItem(tr("%1（不可用）").arg(savedName.isEmpty() ? savedKey : savedName), savedKey);
        adapter_->setItemData(adapter_->count() - 1, savedName, Qt::UserRole + 1);
        selected = adapter_->count() - 1;
    }
    adapter_->setCurrentIndex(selected >= 0 ? selected : 0);
    form->addRow(tr("显卡"), adapter_);
    layout->addWidget(group);
    layout->addStretch(1);
}

QString WorldEffectsSettingsPage::QualityName() const { return quality_->currentData().toString(); }
QString WorldEffectsSettingsPage::RenderAdapterKey() const { return adapter_->currentData().toString(); }
QString WorldEffectsSettingsPage::RenderAdapterName() const { return adapter_->currentData(Qt::UserRole + 1).toString(); }

} // namespace War3Map::QtApp
