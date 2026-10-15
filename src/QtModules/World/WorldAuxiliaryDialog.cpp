#include "WorldAuxiliaryDialog.h"

#include <QByteArray>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <limits>

namespace War3Map::Modules {
namespace {
QString Display(std::string_view bytes) { return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size())); }
QString Number(float value) { return QString::number(value, 'g', std::numeric_limits<float>::max_digits10); }
std::string ReadText(const QLineEdit& edit, std::string_view original) {
    // 原生名称可能不是 UTF-8；未改的显示值必须保留来源字节。
    return edit.text() == edit.property("worldAuxOriginalDisplay").toString()
        ? std::string(original) : edit.text().toUtf8().toStdString();
}
std::string Weather(const MapData::RegionRecord& record) {
    if (std::all_of(record.weatherId.begin(), record.weatherId.end(), [](auto value) { return value == std::byte{}; })) return {};
    return {reinterpret_cast<const char*>(record.weatherId.data()), record.weatherId.size()};
}
QString WeatherDisplay(const MapData::RegionRecord& record) {
    const auto bytes = Weather(record);
    if (MapData::IsEditableRegionWeatherId(bytes)) return Display(bytes);
    return QStringLiteral("原值：0x") + QString::fromLatin1(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())).toHex());
}
}

WorldAuxiliaryDialog::WorldAuxiliaryDialog(WorldAuxiliaryRecord record, const MapDocument& source,
    bool creating, QWidget* parent) : QDialog(parent), original_(std::move(record)) {
    setObjectName(QStringLiteral("worldAuxiliaryDialog"));
    setWindowTitle(creating ? tr("新建区域或镜头") : tr("区域或镜头属性"));
    setMinimumWidth(400);
    auto* column = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    column->addLayout(form);
    const auto addText = [this, form](const char* id, const QString& label, const QString& value) {
        auto* field = new QLineEdit(value, this);
        field->setObjectName(QLatin1String(id)); field->setMaxLength(4096);
        field->setProperty("worldAuxOriginalDisplay", field->text());
        form->addRow(label, field); return field;
    };
    const auto addNumber = [&](std::size_t index, const char* id, const QString& label, float value) {
        numbers_[index] = addText(id, label, Number(value));
        numbers_[index]->setMaxLength(64);
    };
    if (const auto* region = std::get_if<MapData::RegionRecord>(&original_)) {
        setWindowTitle(creating ? tr("新建区域") : tr("区域属性"));
        name_ = addText("worldAuxName", tr("名称"), Display(region->name));
        addNumber(0, "worldAuxRegionLeft", tr("左边界"), region->left);
        addNumber(1, "worldAuxRegionBottom", tr("下边界"), region->bottom);
        addNumber(2, "worldAuxRegionRight", tr("右边界"), region->right);
        addNumber(3, "worldAuxRegionTop", tr("上边界"), region->top);
        form->addRow(tr("创建编号"), new QLabel(QString::number(region->creationNumber), this));
        weather_ = addText("worldAuxWeather", tr("天气 rawcode"), WeatherDisplay(*region));
        weather_->setToolTip(tr("留空表示无天气；新值须为四字节 rawcode。未修改的原值会保留。"));
        sound_ = new QComboBox(this); sound_->setObjectName(QStringLiteral("worldAuxSound"));
        sound_->addItem(tr("无环境音"), QByteArray{});
        if (source.Member(MapDataKind::Sounds).state == MapDataState::Ready && source.Sounds()) {
            for (const auto& sound : source.Sounds()->entries) {
                auto candidate = *region; candidate.ambientSound = sound.name;
                if (MapData::ValidateRegionAmbientSound(candidate, &*source.Sounds()).Succeeded())
                    sound_->addItem(Display(sound.name), QByteArray(sound.name.data(), static_cast<qsizetype>(sound.name.size())));
            }
        }
        const QByteArray originalSound(region->ambientSound.data(), static_cast<qsizetype>(region->ambientSound.size()));
        auto selected = sound_->findData(originalSound);
        if (selected < 0) {
            sound_->addItem(tr("原值：%1").arg(Display(region->ambientSound)), originalSound);
            selected = sound_->count() - 1;
        }
        sound_->setCurrentIndex(selected); form->addRow(tr("环境音"), sound_);
        auto* colors = new QWidget(this); auto* row = new QHBoxLayout(colors); row->setContentsMargins(0, 0, 0, 0);
        for (int component = 0; component < 3; ++component) {
            color_[component] = new QSpinBox(colors); color_[component]->setRange(0, 255);
            color_[component]->setObjectName(QStringLiteral("worldAuxColor%1").arg(component));
            color_[component]->setPrefix(component == 0 ? QStringLiteral("B ") : component == 1 ? QStringLiteral("G ") : QStringLiteral("R "));
            color_[component]->setValue(region->colorBgra[component]); row->addWidget(color_[component]);
        }
        form->addRow(tr("颜色"), colors);
    } else {
        const auto& camera = std::get<MapData::CameraRecord>(original_);
        setWindowTitle(creating ? tr("新建镜头") : tr("镜头属性"));
        name_ = addText("worldAuxName", tr("名称"), Display(camera.name));
        addNumber(0, "worldAuxCameraX", tr("目标 X"), camera.targetX);
        addNumber(1, "worldAuxCameraY", tr("目标 Y"), camera.targetY);
        addNumber(2, "worldAuxCameraZ", tr("Z 偏移"), camera.zOffset);
        addNumber(3, "worldAuxCameraRotation", tr("旋转（度）"), camera.rotation);
        addNumber(4, "worldAuxCameraAttack", tr("攻角（度）"), camera.angleOfAttack);
        addNumber(5, "worldAuxCameraDistance", tr("距离"), camera.targetDistance);
        addNumber(6, "worldAuxCameraRoll", tr("滚转（度）"), camera.roll);
        addNumber(7, "worldAuxCameraFov", tr("视野（度）"), camera.fieldOfView);
        addNumber(8, "worldAuxCameraFar", tr("远裁剪"), camera.farClippingPlane);
        auto* preserved = new QLabel(Number(camera.nearClippingPlane), this);
        preserved->setToolTip(tr("经典格式的第十个数值槽用途未证实，保存时逐位保留。"));
        form->addRow(tr("第十数值槽（只读）"), preserved);
    }
    error_ = new QLabel(this); error_->setObjectName(QStringLiteral("worldAuxError"));
    error_->setWordWrap(true); error_->hide(); column->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &WorldAuxiliaryDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); column->addWidget(buttons);
}

Result<WorldAuxiliaryRecord> WorldAuxiliaryDialog::Record() const {
    auto result = original_;
    const auto read = [&](std::size_t index, float& value) {
        if (numbers_[index]->text() == numbers_[index]->property("worldAuxOriginalDisplay").toString()) return true;
        bool ok = false;
        const float parsed = numbers_[index]->text().trimmed().toFloat(&ok);
        if (!ok || !std::isfinite(parsed)) return false;
        value = parsed; return true;
    };
    bool valid = true;
    if (auto* region = std::get_if<MapData::RegionRecord>(&result)) {
        region->name = ReadText(*name_, region->name);
        valid = read(0, region->left) && read(1, region->bottom) && read(2, region->right) && read(3, region->top);
        if (weather_->text() != weather_->property("worldAuxOriginalDisplay").toString()) {
            const auto weather = weather_->text().toUtf8();
            if (!MapData::IsEditableRegionWeatherId(weather.toStdString()))
                return Result<WorldAuxiliaryRecord>::Failure("World", "REGION_WEATHER_INVALID", "天气需留空或使用四字节可打印 rawcode。");
            region->weatherId = {};
            if (!weather.isEmpty()) for (int i = 0; i < 4; ++i) region->weatherId[i] = static_cast<std::byte>(weather[i]);
        }
        region->ambientSound = sound_->currentData().toByteArray().toStdString();
        for (std::size_t i = 0; i < 3; ++i) region->colorBgra[i] = static_cast<std::uint8_t>(color_[i]->value());
    } else {
        auto& camera = std::get<MapData::CameraRecord>(result);
        camera.name = ReadText(*name_, camera.name);
        const std::array values{&camera.targetX, &camera.targetY, &camera.zOffset, &camera.rotation, &camera.angleOfAttack,
            &camera.targetDistance, &camera.roll, &camera.fieldOfView, &camera.farClippingPlane};
        for (std::size_t i = 0; i < values.size(); ++i) valid = read(i, *values[i]) && valid;
    }
    if (!valid) return Result<WorldAuxiliaryRecord>::Failure("World", "AUX_NUMBER_INVALID", "请输入有效的有限数值，小数点使用“.”。");
    return Result<WorldAuxiliaryRecord>::Success(std::move(result));
}

void WorldAuxiliaryDialog::accept() {
    const auto record = Record();
    if (!record.Succeeded()) {
        error_->setText(QString::fromUtf8(record.diagnostics.front().message)); error_->show(); return;
    }
    QDialog::accept();
}
} // namespace War3Map::Modules
