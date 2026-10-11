#include "CampaignModule.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <bit>
#include <algorithm>
#include <cmath>
#include <limits>

namespace War3Map::Modules {
namespace {
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
}
QWidget* CampaignModule::BuildSettings() {
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true);
    auto* contents = new QWidget(scroll); auto* layout = new QFormLayout(contents);
    const QStringList labels{tr("战役名称"), tr("难度说明"), tr("作者"), tr("自定义背景路径"), tr("小地图路径"), tr("自定义环境声音路径"), tr("雾颜色（原始四字节，十六进制）")};
    for (int i = 0; i < labels.size(); ++i) {
        texts_[i] = new QLineEdit(contents); texts_[i]->setMaxLength(i == 6 ? 8 : 65536); texts_[i]->setObjectName("campaignSettingText" + QString::number(i));
        layout->addRow(labels[i], texts_[i]);
        connect(texts_[i], &QLineEdit::textEdited, this, [this] { settingsDirty_ = true; emit Changed(); });
    }
    description_ = new QPlainTextEdit(contents); description_->setObjectName("campaignDescription"); description_->setMaximumHeight(150);
    layout->addRow(tr("说明"), description_);
    connect(description_, &QPlainTextEdit::textChanged, this, [this] { if (!refreshing_) { settingsDirty_ = true; emit Changed(); } });
    const QStringList numbers{tr("背景编号（-1 为自定义）"), tr("环境声音编号（-1 为自定义）"), tr("雾样式编号"), tr("光标种族编号"), tr("编辑器版本")};
    for (int i = 0; i < numbers.size(); ++i) {
        numbers_[i] = new QSpinBox(contents); numbers_[i]->setRange((std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)());
        numbers_[i]->setObjectName("campaignSettingNumber" + QString::number(i)); layout->addRow(numbers[i], numbers_[i]);
        connect(numbers_[i], &QSpinBox::valueChanged, this, [this] { if (!refreshing_) { settingsDirty_ = true; emit Changed(); } });
    }
    const QStringList fogLabels{tr("雾起点"), tr("雾终点"), tr("雾密度")};
    for (int i = 0; i < 3; ++i) {
        fog_[i] = new QLineEdit(contents); fog_[i]->setObjectName("campaignFog" + QString::number(i)); layout->addRow(fogLabels[i], fog_[i]);
        connect(fog_[i], &QLineEdit::textEdited, this, [this] { settingsDirty_ = true; emit Changed(); });
    }
    variableDifficulty_ = new QCheckBox(tr("允许选择难度"), contents); expansion_ = new QCheckBox(tr("包含资料片地图"), contents);
    layout->addRow(variableDifficulty_); layout->addRow(expansion_);
    for (auto* control : {variableDifficulty_.data(), expansion_.data()}) connect(control, &QCheckBox::toggled, this, [this] { if (!refreshing_) { settingsDirty_ = true; emit Changed(); } });
    scroll->setWidget(contents); return scroll;
}
void CampaignModule::RefreshSettings() {
    if (!texts_[0]) return;
    const bool previous = refreshing_; refreshing_ = true;
    const auto info = session_ ? session_->Info() : Campaign::CampaignInfo{};
    const QStringList values{Text(info.name), Text(info.difficulty), Text(info.author), Text(info.backgroundPath), Text(info.minimapPath), Text(info.ambientSoundPath),
        QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(info.fogColor.data()), 4).toHex())};
    for (int i = 0; i < values.size(); ++i) texts_[i]->setText(values[i]);
    description_->setPlainText(Text(info.description));
    const std::array<int, 5> numbers{info.background, info.ambientSound, info.fogStyle, info.cursor, info.editorVersion};
    for (int i = 0; i < 5; ++i) numbers_[i]->setValue(numbers[i]);
    for (int i = 0; i < 3; ++i) fog_[i]->setText(QString::number(std::bit_cast<float>(info.fogBits[i]), 'g', 9));
    variableDifficulty_->setChecked((info.flags & 1u) != 0); expansion_->setChecked((info.flags & 2u) != 0);
    settingsDirty_ = false; refreshing_ = previous;
}
bool CampaignModule::CommitSettings() {
    if (!settingsDirty_ || !session_) return true;
    Services::CampaignCommand command; command.operation = Services::CampaignOperation::SetInfo; command.info = session_->Info();
    auto& info = command.info;
    info.name = texts_[0]->text().toUtf8().toStdString(); info.difficulty = texts_[1]->text().toUtf8().toStdString(); info.author = texts_[2]->text().toUtf8().toStdString();
    info.backgroundPath = texts_[3]->text().toUtf8().toStdString(); info.minimapPath = texts_[4]->text().toUtf8().toStdString(); info.ambientSoundPath = texts_[5]->text().toUtf8().toStdString();
    info.description = description_->toPlainText().toUtf8().toStdString();
    info.background = numbers_[0]->value(); info.ambientSound = numbers_[1]->value(); info.fogStyle = numbers_[2]->value(); info.cursor = numbers_[3]->value(); info.editorVersion = numbers_[4]->value();
    const auto raw = texts_[6]->text().toLatin1();
    if (raw.size() != 8 || std::any_of(raw.begin(), raw.end(), [](char c) { return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')); })) { Notice(tr("雾颜色需要 8 个十六进制字符。")); return false; }
    const auto color = QByteArray::fromHex(raw);
    for (int i = 0; i < 4; ++i) info.fogColor[i] = static_cast<std::uint8_t>(color[i]);
    for (int i = 0; i < 3; ++i) {
        const auto old = QString::number(std::bit_cast<float>(info.fogBits[i]), 'g', 9);
        if (fog_[i]->text() == old) continue;
        bool ok = false; const auto value = fog_[i]->text().toFloat(&ok);
        if (!ok || !std::isfinite(value)) { Notice(tr("修改后的雾数值必须是有限浮点数。")); return false; }
        info.fogBits[i] = std::bit_cast<std::uint32_t>(value);
    }
    info.flags = (info.flags & ~3u) | (variableDifficulty_->isChecked() ? 1u : 0u) | (expansion_->isChecked() ? 2u : 0u);
    const auto result = session_->Apply(displayedRevision_, command);
    if (!result.Succeeded()) return Failed(result.diagnostics);
    displayedRevision_ = session_->Revision(); settingsDirty_ = false; RefreshTables(); RebuildHistory(); emit Changed(); return true;
}
} // namespace War3Map::Modules
