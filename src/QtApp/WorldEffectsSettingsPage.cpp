#include "WorldEffectsSettingsPage.h"

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
    layout->addWidget(group);
    layout->addStretch(1);
}

QString WorldEffectsSettingsPage::QualityName() const { return quality_->currentData().toString(); }

} // namespace War3Map::QtApp
