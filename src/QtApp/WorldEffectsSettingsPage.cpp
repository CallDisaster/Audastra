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
    auto* note = new QLabel(tr("平衡按完成帧和动画处理耗时自动调整特效数量；省电减少数量和刷新频率。完整取消数量软限，仍遵守资源和内存限制。"), group);
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    form->addRow(note);
    auto* temporary = new QLabel(tr("世界视图状态栏会提示因预算省略的特效；点击可临时显示全部，切换地图后恢复所选设置。"), group);
    temporary->setTextFormat(Qt::PlainText);
    temporary->setWordWrap(true);
    form->addRow(temporary);
    layout->addWidget(group);
    layout->addStretch(1);
}

QString WorldEffectsSettingsPage::QualityName() const { return quality_->currentData().toString(); }

} // namespace War3Map::QtApp
