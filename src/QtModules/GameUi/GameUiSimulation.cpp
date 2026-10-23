#include "GameUiModule.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

// 界面工作区的预览模拟：选一个运行时改变脚本框架的函数，按输入值执行其中的界面语句。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr int kSliderSteps = 1000;

// 数值输入：滑块（拖动）与数字框（精确）同步。滑块范围按初值定，数字框不限。
QWidget* NumberEditor(const SimulationInput& input, QWidget* parent, std::function<void(const QString&)> changed) {
    auto* editor = new QWidget(parent);
    auto* layout = new QHBoxLayout(editor);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(4);
    auto* slider = new QSlider(Qt::Horizontal, editor);
    auto* spin = new QDoubleSpinBox(editor);
    spin->setRange(-1.0e6, 1.0e6);
    spin->setDecimals(input.integer ? 0 : 3);
    spin->setSingleStep(input.integer ? 1.0 : 0.01);
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setFixedWidth(spin->fontMetrics().horizontalAdvance(QStringLiteral("-000000.000")) + 12);
    const double initial = QString::fromStdString(input.value).toDouble();
    spin->setValue(initial);
    const double high = std::max(input.integer ? 10.0 : 1.0, std::ceil(std::fabs(initial) * 2.0));
    const double low = initial < 0.0 ? -high : 0.0;
    slider->setRange(0, kSliderSteps);
    const auto toSlider = [low, high](double value) {
        return static_cast<int>(std::lround((std::clamp(value, low, high) - low) / (high - low) * kSliderSteps));
    };
    slider->setValue(toSlider(initial));
    QObject::connect(slider, &QSlider::valueChanged, spin, [spin, low, high, integer = input.integer](int position) {
        double value = low + (high - low) * position / kSliderSteps;
        if (integer) value = std::round(value);
        spin->setValue(value);
    });
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, editor, [slider, toSlider, changed, integer = input.integer](double value) {
        const QSignalBlocker block(slider);
        slider->setValue(toSlider(value));
        changed(integer ? QString::number(static_cast<long long>(std::llround(value))) : QString::number(value, 'g', 9));
    });
    layout->addWidget(slider, 1);
    layout->addWidget(spin);
    return editor;
}

} // namespace

QStringList GameUiModule::SimulationFunctions() const {
    QStringList names;
    if (!definitions_) return names;
    for (const auto& name : GameUi::SimulationFunctions(definitions_->scriptFrames)) names.push_back(Text(name));
    return names;
}

QString GameUiModule::SimulationFunction() const {
    return simulator_ ? Text(simulator_->Function()) : QString();
}

void GameUiModule::ResetSimulation() {
    simulator_.reset();
    simulation_.reset();
    simulatedPlan_.reset();
    simulationValues_.clear();
    simulationDiagnostics_.clear();
}

bool GameUiModule::SetSimulation(const QString& function, QString* error) {
    ResetSimulation();
    bool ok = true;
    if (!function.isEmpty()) {
        if (!definitions_) {
            if (error) *error = tr("界面定义仍在读取");
            ok = false;
        } else {
            auto prepared = ScriptSimulator::Prepare(definitions_->scripts, definitions_->scriptFrames, function.toStdString());
            simulationDiagnostics_ = prepared.diagnostics;
            if (prepared.value) {
                simulator_.emplace(std::move(*prepared.value));
            } else {
                if (error) *error = prepared.diagnostics.empty() ? tr("无法模拟") : Text(prepared.diagnostics.front().message);
                ok = false;
            }
        }
    }
    if (simulationBox_) {
        const QSignalBlocker block(simulationBox_);
        const int index = simulationBox_->findData(SimulationFunction());
        simulationBox_->setCurrentIndex(index < 0 ? 0 : index);
    }
    RunSimulation();
    return ok;
}

void GameUiModule::SetSimulationValues(const std::map<std::string, std::string>& values) {
    for (const auto& [key, value] : values) simulationValues_[key] = value;
    RunSimulation();
}

const GameUi::SimulationResult* GameUiModule::Simulation() const {
    return simulation_ ? &*simulation_ : nullptr;
}

void GameUiModule::RunSimulation() {
    if (simulator_ && definitions_) {
        simulation_ = simulator_->Run(simulationValues_);
        simulatedPlan_ = definitions_->scriptFrames;
        simulatedPlan_->frames = simulation_->frames;
    } else {
        simulation_.reset();
        simulatedPlan_.reset();
    }
    RebuildScene();
    UpdateSimulationPanel();
}

void GameUiModule::BuildSimulationPanel(QWidget* parent) {
    simulationPanel_ = new QWidget(parent);
    simulationPanel_->setObjectName(QStringLiteral("gameUiSimulation"));
    auto* layout = new QVBoxLayout(simulationPanel_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* header = new QHBoxLayout;
    header->addWidget(new QLabel(tr("模拟"), simulationPanel_));
    simulationBox_ = new QComboBox(simulationPanel_);
    simulationBox_->setObjectName(QStringLiteral("gameUiSimulationFunction"));
    header->addWidget(simulationBox_, 1);
    layout->addLayout(header);
    simulationInputs_ = new QTableWidget(0, 2, simulationPanel_);
    simulationInputs_->setObjectName(QStringLiteral("gameUiSimulationInputs"));
    simulationInputs_->setHorizontalHeaderLabels({tr("输入"), tr("值")});
    simulationInputs_->verticalHeader()->setVisible(false);
    simulationInputs_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    simulationInputs_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    simulationInputs_->horizontalHeader()->resizeSection(0, 150);
    simulationInputs_->setSelectionMode(QAbstractItemView::NoSelection);
    simulationInputs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(simulationInputs_, 1);
    connect(simulationBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        (void)SetSimulation(simulationBox_->itemData(index).toString());
    });
    UpdateSimulationPanel();
}

void GameUiModule::UpdateSimulationPanel() {
    if (!simulationPanel_) return;
    const QStringList functions = SimulationFunctions();
    simulationPanel_->setVisible(!functions.isEmpty());
    {
        const QSignalBlocker block(simulationBox_);
        QStringList current;
        for (int i = 1; i < simulationBox_->count(); ++i) current.push_back(simulationBox_->itemData(i).toString());
        if (current != functions || simulationBox_->count() == 0) {
            simulationBox_->clear();
            simulationBox_->addItem(tr("不模拟"), QString());
            for (const QString& function : functions) simulationBox_->addItem(function, function);
        }
        const int index = simulationBox_->findData(SimulationFunction());
        simulationBox_->setCurrentIndex(index < 0 ? 0 : index);
    }
    QStringList keys;
    if (simulation_) {
        for (const auto& input : simulation_->inputs) keys.push_back(Text(input.key));
    }
    // 输入集合不变时保留编辑器，拖动过程中不重建。
    if (keys == simulationKeys_) return;
    simulationKeys_ = keys;
    simulationInputs_->setRowCount(0);
    if (!simulation_) return;
    simulationInputs_->setRowCount(static_cast<int>(simulation_->inputs.size()));
    for (int row = 0; row < static_cast<int>(simulation_->inputs.size()); ++row) {
        const SimulationInput& input = simulation_->inputs[static_cast<std::size_t>(row)];
        const QString label = input.condition ? tr("条件 %1").arg(Text(input.label)) : Text(input.label);
        auto* item = new QTableWidgetItem(label);
        item->setToolTip(tr("%1  第 %2 行").arg(Text(input.label)).arg(input.line));
        simulationInputs_->setItem(row, 0, item);
        const std::string key = input.key;
        const auto changed = [this, key](const QString& value) { SetSimulationValues({{key, value.toStdString()}}); };
        QWidget* editor = nullptr;
        if (input.type == SimulationValueType::Number) {
            editor = NumberEditor(input, simulationInputs_, changed);
        } else if (input.type == SimulationValueType::Boolean) {
            auto* box = new QCheckBox(simulationInputs_);
            box->setChecked(input.value == "true");
            connect(box, &QCheckBox::toggled, this, [changed](bool on) { changed(on ? QStringLiteral("true") : QStringLiteral("false")); });
            editor = box;
        } else {
            auto* line = new QLineEdit(Text(input.value), simulationInputs_);
            connect(line, &QLineEdit::textEdited, this, changed);
            editor = line;
        }
        simulationInputs_->setCellWidget(row, 1, editor);
    }
    simulationInputs_->resizeRowsToContents();
}

} // namespace War3Map::Modules
