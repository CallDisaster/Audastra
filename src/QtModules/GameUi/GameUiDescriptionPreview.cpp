#include "GameUiModule.h"

#include "GameUiInputs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <charconv>

// 界面工作区的界面描述预览：切换状态、拖动数值，看地图自己的界面在各种取值下的样子。只改预览，不改地图。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

const std::vector<GameUiDescriptionFile> kNoDescriptions;

const UiValue* FindValue(const UiDescription& description, std::string_view id) {
    for (const auto& value : description.values) {
        if (value.id == id) return &value;
    }
    return nullptr;
}

// 预览键：编号，或 编号[下标]。
struct PreviewKey {
    std::string id;
    std::optional<std::uint32_t> index;
};

std::optional<PreviewKey> ParseKey(std::string_view key) {
    const std::size_t open = key.find('[');
    if (open == std::string_view::npos) return PreviewKey{std::string(key), std::nullopt};
    if (!key.ends_with(']')) return std::nullopt;
    const std::string_view digits = key.substr(open + 1, key.size() - open - 2);
    std::uint32_t index = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    if (error != std::errc() || end != digits.data() + digits.size()) return std::nullopt;
    return PreviewKey{std::string(key.substr(0, open)), index};
}

QString TypeLabel(UiValueType type) {
    switch (type) {
    case UiValueType::Integer: return QObject::tr("整数");
    case UiValueType::Boolean: return QObject::tr("布尔");
    case UiValueType::String: return QObject::tr("文字");
    default: return QObject::tr("实数");
    }
}

} // namespace

const std::vector<GameUiDescriptionFile>& GameUiModule::Descriptions() const {
    return definitions_ ? definitions_->descriptions : kNoDescriptions;
}

const UiDescription* GameUiModule::Description(const QString& id) const {
    for (const auto& file : Descriptions()) {
        if (Text(file.description.id) == id) return &file.description;
    }
    return nullptr;
}

UiInstanceState GameUiModule::DescriptionPreview(const QString& id) const {
    const auto found = descriptionStates_.find(id.toStdString());
    return found == descriptionStates_.end() ? UiInstanceState{} : found->second;
}

bool GameUiModule::SetDescriptionState(const QString& id, const QString& state, QString* error) {
    const UiDescription* description = Description(id);
    if (!description) {
        if (error) *error = tr("没有界面描述 %1").arg(id);
        return false;
    }
    if (!state.isEmpty() && std::none_of(description->states.begin(), description->states.end(),
                                [&](const UiState& candidate) { return Text(candidate.id) == state; })) {
        if (error) *error = tr("界面 %1 没有状态 %2").arg(id, state);
        return false;
    }
    descriptionStates_[id.toStdString()].state = state.toStdString();
    RebuildScene();
    return true;
}

bool GameUiModule::SetDescriptionValues(const QString& id, const std::map<std::string, std::string>& values, QString* error) {
    const UiDescription* description = Description(id);
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!description) return fail(tr("没有界面描述 %1").arg(id));
    std::map<std::string, std::string> accepted;
    for (const auto& [key, text] : values) {
        const auto parsed = ParseKey(key);
        const UiValue* value = parsed ? FindValue(*description, parsed->id) : nullptr;
        if (!value) return fail(tr("界面 %1 没有数值 %2").arg(id, Text(key)));
        if (value->size == 0 && parsed->index) return fail(tr("数值 %1 不是数组，不能写下标").arg(Text(value->id)));
        if (value->size > 0 && (!parsed->index || *parsed->index >= value->size)) {
            return fail(tr("数值 %1 是 %2 格的数组，要写 %1[0] 到 %1[%3]").arg(Text(value->id)).arg(value->size).arg(value->size - 1));
        }
        const auto normalized = NormalizeUiValue(value->type, text);
        if (!normalized) return fail(tr("%1 的值 %2 不是%3").arg(Text(key), Text(text), TypeLabel(value->type)));
        accepted[key] = *normalized;
    }
    auto& state = descriptionStates_[id.toStdString()];
    for (auto& [key, text] : accepted) state.values[key] = std::move(text);
    RebuildScene();
    return true;
}

void GameUiModule::SetShowHidden(bool show) {
    showHidden_ = show;
    if (showHiddenButton_ && showHiddenButton_->isChecked() != show) {
        const QSignalBlocker block(showHiddenButton_);
        showHiddenButton_->setChecked(show);
    }
    RebuildDrawList();
    emit SceneChanged();
    StartAssetsJob();
}

void GameUiModule::BuildDescriptionPanel(QWidget* parent) {
    descriptionPanel_ = new QWidget(parent);
    descriptionPanel_->setObjectName(QStringLiteral("gameUiDescription"));
    auto* layout = new QVBoxLayout(descriptionPanel_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* header = new QHBoxLayout;
    descriptionTitle_ = new QLabel(descriptionPanel_);
    descriptionTitle_->setObjectName(QStringLiteral("gameUiDescriptionTitle"));
    header->addWidget(descriptionTitle_, 1);
    descriptionStateBox_ = new QComboBox(descriptionPanel_);
    descriptionStateBox_->setObjectName(QStringLiteral("gameUiDescriptionState"));
    header->addWidget(descriptionStateBox_);
    layout->addLayout(header);
    descriptionInputs_ = new QTableWidget(0, 2, descriptionPanel_);
    descriptionInputs_->setObjectName(QStringLiteral("gameUiDescriptionInputs"));
    descriptionInputs_->setHorizontalHeaderLabels({tr("数值"), tr("值")});
    descriptionInputs_->verticalHeader()->setVisible(false);
    descriptionInputs_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    descriptionInputs_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    descriptionInputs_->horizontalHeader()->resizeSection(0, 120);
    descriptionInputs_->setSelectionMode(QAbstractItemView::NoSelection);
    descriptionInputs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(descriptionInputs_, 1);
    connect(descriptionStateBox_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!descriptionPanelFor_.isEmpty()) (void)SetDescriptionState(descriptionPanelFor_, descriptionStateBox_->itemData(index).toString());
    });
    descriptionPanel_->setVisible(false);
}

void GameUiModule::UpdateDescriptionPanel() {
    if (!descriptionPanel_) return;
    QString id;
    if (tree_ && selected_ < tree_->Nodes().size() && tree_->Nodes()[selected_].uiFrame) {
        id = Text(tree_->Nodes()[selected_].uiFrame->description);
    }
    const UiDescription* description = id.isEmpty() ? nullptr : Description(id);
    descriptionPanel_->setVisible(description != nullptr);
    if (!description) {
        descriptionPanelFor_.clear();
        return;
    }
    const UiInstanceState preview = DescriptionPreview(id);
    const bool rebuild = descriptionPanelFor_ != id;
    {
        const QSignalBlocker block(descriptionStateBox_);
        if (rebuild) {
            descriptionStateBox_->clear();
            descriptionStateBox_->addItem(tr("基础状态"), QString());
            for (const auto& state : description->states) {
                descriptionStateBox_->addItem(state.label.empty() ? Text(state.id) : Text(state.label), Text(state.id));
            }
        }
        const int index = descriptionStateBox_->findData(Text(preview.state));
        descriptionStateBox_->setCurrentIndex(index < 0 ? 0 : index);
        descriptionStateBox_->setEnabled(!description->states.empty());
    }
    // 同一份描述：保留编辑器，拖动过程中不重建。
    if (!rebuild) return;
    descriptionPanelFor_ = id;
    descriptionTitle_->setText(description->label.empty() ? id : QStringLiteral("%1  %2").arg(Text(description->label), id));
    descriptionInputs_->setRowCount(0);
    for (const auto& value : description->values) {
        const std::uint32_t cells = value.size == 0 ? 1 : value.size;
        for (std::uint32_t cell = 0; cell < cells; ++cell) {
            const std::optional<std::uint32_t> index = value.size == 0 ? std::nullopt : std::optional<std::uint32_t>(cell);
            const std::string key = index ? value.id + "[" + std::to_string(cell) + "]" : value.id;
            QString label = value.label.empty() ? Text(value.id) : Text(value.label);
            if (index) label += QStringLiteral("[%1]").arg(cell);
            const int row = descriptionInputs_->rowCount();
            descriptionInputs_->insertRow(row);
            auto* item = new QTableWidgetItem(label);
            item->setToolTip(QStringLiteral("%1  %2").arg(Text(key), TypeLabel(value.type)));
            descriptionInputs_->setItem(row, 0, item);
            const QString current = Text(UiValueText(*description, preview, value.id, index).value_or(value.defaultValue));
            const auto changed = [this, id, key](const QString& text) { (void)SetDescriptionValues(id, {{key, text.toStdString()}}); };
            QWidget* editor = nullptr;
            if (value.type == UiValueType::Real || value.type == UiValueType::Integer) {
                editor = GameUiNumberEditor(current.toDouble(), value.type == UiValueType::Integer, value.min, value.max,
                    descriptionInputs_, changed);
            } else if (value.type == UiValueType::Boolean) {
                auto* box = new QCheckBox(descriptionInputs_);
                box->setChecked(current == QLatin1String("true"));
                connect(box, &QCheckBox::toggled, this, [changed](bool on) { changed(on ? QStringLiteral("true") : QStringLiteral("false")); });
                editor = box;
            } else {
                auto* line = new QLineEdit(current, descriptionInputs_);
                connect(line, &QLineEdit::textEdited, this, changed);
                editor = line;
            }
            descriptionInputs_->setCellWidget(row, 1, editor);
        }
    }
    descriptionInputs_->resizeRowsToContents();
}

} // namespace War3Map::Modules
