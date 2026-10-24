#include "GameUiModule.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "ObjectViewEntries.h"
#include "War3MapCore/GameUi/UiPresets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QListWidget>
#include <QPushButton>

#include <algorithm>
#include <initializer_list>

// 绑定界面的系统窗口：“读条系统”选读条条界面及其数值与状态，“团队框架”选团队框架界面及其数组数值与点击事件，
// 生成各自的触发器。两者都可直接按模板新建一份界面再绑定。读条的技能登记在触发器编辑器里改。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

QString Labeled(const std::string& label, const std::string& id) {
    return label.empty() ? Text(id) : QStringLiteral("%1  %2").arg(Text(label), Text(id));
}

void Prefer(QComboBox* box, std::initializer_list<const char*> preferred) {
    for (const char* id : preferred) {
        if (const int index = box->findData(QString::fromLatin1(id)); index >= 0) {
            box->setCurrentIndex(index);
            return;
        }
    }
}

// 按类型与格数（0 为单值）列出数值；可空的一项以“无”开头。按常用编号预选。
void FillValues(QComboBox* box, const UiDescription* description, UiValueType type, std::uint32_t size, bool optional,
    std::initializer_list<const char*> preferred) {
    box->clear();
    if (optional) box->addItem(QObject::tr("无"), QString());
    if (!description) return;
    for (const auto& value : description->values) {
        if (value.type == type && value.size == size) box->addItem(Labeled(value.label, value.id), Text(value.id));
    }
    Prefer(box, preferred);
}

void FillStates(QComboBox* box, const UiDescription* description, std::initializer_list<const char*> preferred) {
    box->clear();
    box->addItem(QObject::tr("无"), QString());
    if (!description) return;
    for (const auto& state : description->states) box->addItem(Labeled(state.label, state.id), Text(state.id));
    Prefer(box, preferred);
}

// 列出地图界面描述，末尾加一项“按模板新建”（数据为 +模板编号）。
void FillDescriptions(QComboBox* box, const std::vector<GameUiDescriptionFile>& files, const QString& none, const QString& createLabel,
    std::string_view preset) {
    if (!none.isEmpty()) box->addItem(none, QString());
    for (const auto& file : files) box->addItem(Labeled(file.description.label, file.description.id), Text(file.description.id));
    box->addItem(createLabel, QStringLiteral("+") + Text(preset));
}

// 窗口的表单与按钮；accepted 前不改地图。
struct SystemDialog {
    QDialog dialog;
    QFormLayout* form;

    SystemDialog(QWidget* parent, const char* name, const QString& title) : dialog(parent), form(new QFormLayout(&dialog)) {
        dialog.setObjectName(QLatin1String(name));
        dialog.setWindowTitle(title);
    }

    QComboBox* Combo(const char* name, const QString& label) {
        auto* box = new QComboBox(&dialog);
        box->setObjectName(QLatin1String(name));
        box->setMinimumWidth(220);
        form->addRow(label, box);
        return box;
    }

    bool Run() {
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("生成"));
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        return dialog.exec() == QDialog::Accepted;
    }
};

// 所选节点所在的界面描述编号；没有时为空。
QString SelectedDescriptionId(const FrameTree* tree, std::uint32_t selected) {
    if (!tree || selected >= tree->Nodes().size() || !tree->Nodes()[selected].uiFrame) return {};
    return Text(tree->Nodes()[selected].uiFrame->description);
}

} // namespace

void GameUiModule::ShowCastSystemDialog() {
    SystemDialog ui(page_, "gameUiCastDialog", tr("读条系统"));
    auto* bar = ui.Combo("gameUiCastBar", tr("读条条"));
    auto* visible = ui.Combo("gameUiCastVisible", tr("显示"));
    auto* progress = ui.Combo("gameUiCastProgress", tr("进度"));
    auto* text = ui.Combo("gameUiCastText", tr("技能名"));
    auto* remaining = ui.Combo("gameUiCastRemaining", tr("剩余秒数"));
    auto* channel = ui.Combo("gameUiCastChannel", tr("引导状态"));
    auto* interrupted = ui.Combo("gameUiCastInterrupted", tr("打断状态"));

    FillDescriptions(bar, Descriptions(), tr("不绑定"), tr("新建读条条"), "castbar");
    const auto refill = [&, this] {
        const UiDescription* description = Description(bar->currentData().toString());
        FillValues(visible, description, UiValueType::Boolean, 0, false, {"shown", "visible", "show"});
        FillValues(progress, description, UiValueType::Real, 0, false, {"progress", "ratio", "value"});
        FillValues(text, description, UiValueType::String, 0, true, {"text", "label", "name", "spell"});
        FillValues(remaining, description, UiValueType::Real, 0, true, {"remaining", "left", "time"});
        FillStates(channel, description, {"channel", "channeling"});
        FillStates(interrupted, description, {"interrupt", "interrupted"});
        for (QComboBox* box : {visible, progress, text, remaining, channel, interrupted}) box->setEnabled(description != nullptr);
    };
    connect(bar, &QComboBox::currentIndexChanged, &ui.dialog, refill);
    if (const int index = bar->findData(SelectedDescriptionId(tree_.get(), selected_)); index >= 0) bar->setCurrentIndex(index);
    refill();
    if (!ui.Run()) return;

    CastSystemRequest request;
    request.bar = bar->currentData().toString();
    QString error;
    if (request.bar.startsWith(QLatin1Char('+'))) {
        const auto created = CreateFromPreset(QStringLiteral("castbar"), &error);
        if (!created) {
            ReportAuthoring(error);
            return;
        }
        using namespace CastBarPreset;
        request.bar = *created;
        request.visible = Text(kVisible);
        request.progress = Text(kProgress);
        request.text = Text(kText);
        request.remaining = Text(kRemaining);
        request.channel = Text(kChannel);
        request.interrupted = Text(kInterrupted);
    } else if (!request.bar.isEmpty()) {
        request.visible = visible->currentData().toString();
        request.progress = progress->currentData().toString();
        request.text = text->currentData().toString();
        request.remaining = remaining->currentData().toString();
        request.channel = channel->currentData().toString();
        request.interrupted = interrupted->currentData().toString();
    }
    const auto generated = GenerateCastSystem(request, false, &error);
    if (!generated) {
        ReportAuthoring(error);
        return;
    }
    ReportAuthoring({});
    if (host_) host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(kCastTriggerCategory)), 8000);
}

void GameUiModule::ShowPartySystemDialog() {
    SystemDialog ui(page_, "gameUiPartyDialog", tr("团队框架"));
    auto* frame = ui.Combo("gameUiPartyFrame", tr("界面"));
    auto* shown = ui.Combo("gameUiPartyShown", tr("显示"));
    auto* health = ui.Combo("gameUiPartyHealth", tr("生命"));
    auto* mana = ui.Combo("gameUiPartyMana", tr("法力"));
    auto* name = ui.Combo("gameUiPartyName", tr("名字"));
    auto* dead = ui.Combo("gameUiPartyDead", tr("死亡"));
    auto* outOfRange = ui.Combo("gameUiPartyOutOfRange", tr("过远"));
    auto* select = ui.Combo("gameUiPartySelect", tr("点选"));
    auto* nameSource = ui.Combo("gameUiPartyNameSource", tr("名字来源"));
    nameSource->addItem(tr("玩家名"), static_cast<int>(PartyNameSource::Player));
    nameSource->addItem(tr("英雄名或单位名"), static_cast<int>(PartyNameSource::Unit));
    auto* range = new QDoubleSpinBox(&ui.dialog);
    range->setObjectName(QStringLiteral("gameUiPartyRange"));
    range->setRange(100.0, 100000.0);
    range->setDecimals(0);
    range->setSingleStep(100.0);
    range->setValue(1200.0);
    ui.form->addRow(tr("过远距离"), range);
    auto* autoHeroes = new QCheckBox(tr("自动加入玩家英雄"), &ui.dialog);
    autoHeroes->setObjectName(QStringLiteral("gameUiPartyAutoHeroes"));
    autoHeroes->setChecked(true);
    ui.form->addRow(QString(), autoHeroes);
    // 增减益：勾选的图标数值按列表顺序显示；要显示的增减益从物编目录选，有序。
    auto* buffIcons = new QListWidget(&ui.dialog);
    buffIcons->setObjectName(QStringLiteral("gameUiPartyBuffIcons"));
    buffIcons->setMaximumHeight(96);
    ui.form->addRow(tr("增减益图标"), buffIcons);
    auto* buffs = new QtApp::OrderedObjectListWidget(project_.iconProvider_.get(), &ui.dialog);
    buffs->setObjectName(QStringLiteral("gameUiPartyBuffs"));
    QVector<QtApp::ObjectViewEntry> buffEntries;
    for (auto& entry : QtApp::BuildObjectViewEntries(project_)) {
        if (entry.kind == QLatin1String("buff")) buffEntries.push_back(std::move(entry));
    }
    buffs->SetEntries(buffEntries);
    buffs->SetKindFilter(QStringLiteral("buff"));
    buffs->SetInlineCatalogEnabled(true);
    ui.form->addRow(tr("增减益"), buffs);

    FillDescriptions(frame, Descriptions(), {}, tr("新建团队框架"), "party");
    // 其余数值按“显示”数组的格数筛选。
    const auto refillCells = [&, this] {
        const UiDescription* description = Description(frame->currentData().toString());
        std::uint32_t size = 0;
        if (description) {
            for (const auto& value : description->values) {
                if (Text(value.id) == shown->currentData().toString()) size = value.size;
            }
        }
        const UiDescription* cells = size ? description : nullptr;
        FillValues(health, cells, UiValueType::Real, size, true, {"hp", "health", "life"});
        FillValues(mana, cells, UiValueType::Real, size, true, {"mp", "mana"});
        FillValues(name, cells, UiValueType::String, size, true, {"name", "label"});
        FillValues(dead, cells, UiValueType::Boolean, size, true, {"dead"});
        FillValues(outOfRange, cells, UiValueType::Boolean, size, true, {"far", "range", "outofrange"});
        buffIcons->clear();
        if (cells) {
            for (const auto& value : cells->values) {
                if (value.type != UiValueType::String || value.size != size) continue;
                auto* item = new QListWidgetItem(Labeled(value.label, value.id), buffIcons);
                item->setData(Qt::UserRole, Text(value.id));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                const bool icon = value.id.starts_with("buff") || value.id.starts_with("icon");
                item->setCheckState(icon ? Qt::Checked : Qt::Unchecked);
            }
        }
        select->clear();
        select->addItem(tr("无"), QString());
        if (cells) {
            for (const auto& event : cells->events) {
                if (event.kind == UiEventKind::Click) select->addItem(Labeled(event.frame, event.id), Text(event.id));
            }
            Prefer(select, {"select", "click", "pick"});
        }
    };
    const auto refill = [&, this] {
        const UiDescription* description = Description(frame->currentData().toString());
        shown->clear();
        if (description) {
            for (const auto& value : description->values) {
                if (value.type == UiValueType::Boolean && value.size > 0) shown->addItem(Labeled(value.label, value.id), Text(value.id));
            }
            Prefer(shown, {"shown", "visible", "show"});
        }
        refillCells();
        for (QWidget* box : std::initializer_list<QWidget*>{shown, health, mana, name, dead, outOfRange, select, buffIcons}) {
            box->setEnabled(description != nullptr);
        }
    };
    connect(frame, &QComboBox::currentIndexChanged, &ui.dialog, refill);
    connect(shown, &QComboBox::currentIndexChanged, &ui.dialog, refillCells);
    if (const int index = frame->findData(SelectedDescriptionId(tree_.get(), selected_)); index >= 0) frame->setCurrentIndex(index);
    refill();
    if (!ui.Run()) return;

    PartySystemRequest request;
    request.frame = frame->currentData().toString();
    request.nameSource = static_cast<PartyNameSource>(nameSource->currentData().toInt());
    request.farRange = range->value();
    request.autoHeroes = autoHeroes->isChecked();
    request.buffs = buffs->Ids();
    QString error;
    if (request.frame.startsWith(QLatin1Char('+'))) {
        const auto created = CreateFromPreset(QStringLiteral("party"), &error);
        if (!created) {
            ReportAuthoring(error);
            return;
        }
        using namespace PartyPreset;
        request.frame = *created;
        request.shown = Text(kShown);
        request.health = Text(kHealth);
        request.mana = Text(kMana);
        request.name = Text(kName);
        request.dead = Text(kDead);
        request.outOfRange = Text(kFar);
        request.select = Text(kSelect);
        for (const auto icon : kBuffs) request.buffIcons.append(Text(icon));
    } else {
        request.shown = shown->currentData().toString();
        request.health = health->currentData().toString();
        request.mana = mana->currentData().toString();
        request.name = name->currentData().toString();
        request.dead = dead->currentData().toString();
        request.outOfRange = outOfRange->currentData().toString();
        request.select = select->currentData().toString();
        for (int i = 0; i < buffIcons->count(); ++i) {
            if (buffIcons->item(i)->checkState() == Qt::Checked) request.buffIcons.append(buffIcons->item(i)->data(Qt::UserRole).toString());
        }
    }
    const auto generated = GeneratePartySystem(request, false, &error);
    if (!generated) {
        ReportAuthoring(error);
        return;
    }
    ReportAuthoring({});
    if (host_) host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(kPartyTriggerCategory)), 8000);
}

std::optional<QString> GameUiModule::CreateFromPreset(const QString& preset, QString* error) {
    QString id = preset;
    for (int n = 2; Description(id); ++n) id = preset + QString::number(n);
    if (!CreateDescription(id, {}, error, preset) || !GenerateDescriptionTriggers(id, false, error)) return std::nullopt;
    return id;
}

} // namespace War3Map::Modules
