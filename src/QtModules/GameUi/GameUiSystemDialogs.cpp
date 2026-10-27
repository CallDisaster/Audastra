#include "GameUiModule.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "ObjectViewEntries.h"
#include "TriggerCommands.h"
#include "War3MapCore/GameUi/UiPresets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

#include <algorithm>
#include <initializer_list>

// 绑定界面的系统窗口：“读条系统”选读条条界面及其数值与状态，“团队框架”选团队框架界面及其数组数值与点击事件，
// “目标框架”选目标框架界面及其单值数值与点击事件，“天赋树”选按天赋树模板建的界面（按模板的编号绑定），生成各自的触发器。都可直接按模板新建一份界面再绑定。
// 读条的技能登记与各英雄的天赋登记在触发器编辑器里改。
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

// 格数相同的字符串数值列为可勾选的图标数值，编号以 buff、icon 开头的默认勾选。
void FillIconValues(QListWidget* list, const UiDescription* description, std::uint32_t size) {
    list->clear();
    if (!description) return;
    for (const auto& value : description->values) {
        if (value.type != UiValueType::String || value.size != size) continue;
        auto* item = new QListWidgetItem(Labeled(value.label, value.id), list);
        item->setData(Qt::UserRole, Text(value.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool icon = value.id.starts_with("buff") || value.id.starts_with("icon");
        item->setCheckState(icon ? Qt::Checked : Qt::Unchecked);
    }
}

QStringList CheckedValues(const QListWidget* list) {
    QStringList ids;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->checkState() == Qt::Checked) ids.append(list->item(i)->data(Qt::UserRole).toString());
    }
    return ids;
}

// 点击事件列表，前面一项“无”。
void FillClickEvents(QComboBox* box, const UiDescription* description) {
    box->clear();
    box->addItem(QObject::tr("无"), QString());
    if (!description) return;
    for (const auto& event : description->events) {
        if (event.kind == UiEventKind::Click) box->addItem(Labeled(event.frame, event.id), Text(event.id));
    }
    Prefer(box, {"select", "click", "pick"});
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

    // 物编目录里某一类对象的有序选择列表（增减益、头像单位）。
    QtApp::OrderedObjectListWidget* ObjectList(const QtProject::EditorProject& project, const char* name, const QString& label,
        const QString& kind) {
        auto* list = new QtApp::OrderedObjectListWidget(project.iconProvider_.get(), &dialog);
        list->setObjectName(QLatin1String(name));
        QVector<QtApp::ObjectViewEntry> entries;
        for (auto& entry : QtApp::BuildObjectViewEntries(project)) {
            if (entry.kind == kind) entries.push_back(std::move(entry));
        }
        list->SetEntries(entries);
        list->SetKindFilter(kind);
        list->SetInlineCatalogEnabled(true);
        form->addRow(label, list);
        return list;
    }

    // 可勾选的增减益图标数值，勾选的按列表顺序显示。
    QListWidget* IconValues(const char* name, const QString& label) {
        auto* list = new QListWidget(&dialog);
        list->setObjectName(QLatin1String(name));
        list->setMaximumHeight(96);
        form->addRow(label, list);
        return list;
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
    if (!request.bar.isEmpty()) OfferMapRuntime();
}

void GameUiModule::ShowPartySystemDialog() {
    SystemDialog ui(page_, "gameUiPartyDialog", tr("团队框架"));
    auto* frame = ui.Combo("gameUiPartyFrame", tr("界面"));
    auto* shown = ui.Combo("gameUiPartyShown", tr("显示"));
    auto* health = ui.Combo("gameUiPartyHealth", tr("生命"));
    auto* mana = ui.Combo("gameUiPartyMana", tr("法力"));
    auto* healthNow = ui.Combo("gameUiPartyHealthNow", tr("当前生命"));
    auto* healthMax = ui.Combo("gameUiPartyHealthMax", tr("最大生命"));
    auto* manaNow = ui.Combo("gameUiPartyManaNow", tr("当前法力"));
    auto* manaMax = ui.Combo("gameUiPartyManaMax", tr("最大法力"));
    auto* portrait = ui.Combo("gameUiPartyPortrait", tr("头像"));
    auto* name = ui.Combo("gameUiPartyName", tr("名字"));
    auto* dead = ui.Combo("gameUiPartyDead", tr("死亡"));
    auto* outOfRange = ui.Combo("gameUiPartyOutOfRange", tr("过远"));
    auto* select = ui.Combo("gameUiPartySelect", tr("点选"));
    auto* nameSource = ui.Combo("gameUiPartyNameSource", tr("名字来源"));
    nameSource->addItem(tr("玩家名"), static_cast<int>(UnitNameSource::Player));
    nameSource->addItem(tr("英雄名或单位名"), static_cast<int>(UnitNameSource::Unit));
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
    // 增减益与头像：要显示的增减益、登记头像的单位类型从物编目录选，有序；没登记的队员头像为空。
    auto* buffIcons = ui.IconValues("gameUiPartyBuffIcons", tr("增减益图标"));
    auto* buffs = ui.ObjectList(project_, "gameUiPartyBuffs", tr("增减益"), QStringLiteral("buff"));
    auto* portraits = ui.ObjectList(project_, "gameUiPartyPortraits", tr("头像单位"), QStringLiteral("unit"));

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
        FillValues(healthNow, cells, UiValueType::Real, size, true, {"hp_now", "health_now", "life_now"});
        FillValues(healthMax, cells, UiValueType::Real, size, true, {"hp_max", "health_max", "life_max"});
        FillValues(manaNow, cells, UiValueType::Real, size, true, {"mp_now", "mana_now"});
        FillValues(manaMax, cells, UiValueType::Real, size, true, {"mp_max", "mana_max"});
        FillValues(portrait, cells, UiValueType::String, size, true, {"portrait", "avatar"});
        FillValues(name, cells, UiValueType::String, size, true, {"name", "label"});
        FillValues(dead, cells, UiValueType::Boolean, size, true, {"dead"});
        FillValues(outOfRange, cells, UiValueType::Boolean, size, true, {"far", "range", "outofrange"});
        FillIconValues(buffIcons, cells, size);
        FillClickEvents(select, cells);
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
        for (QWidget* box : std::initializer_list<QWidget*>{shown, health, mana, healthNow, healthMax, manaNow, manaMax, portrait, name, dead,
                 outOfRange, select, buffIcons}) {
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
    request.nameSource = static_cast<UnitNameSource>(nameSource->currentData().toInt());
    request.farRange = range->value();
    request.autoHeroes = autoHeroes->isChecked();
    request.buffs = buffs->Ids();
    request.portraits = portraits->Ids();
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
        request.healthNow = Text(kHealthNow);
        request.healthMax = Text(kHealthMax);
        request.portrait = Text(kPortrait);
        for (const auto icon : kBuffs) request.buffIcons.append(Text(icon));
    } else {
        request.shown = shown->currentData().toString();
        request.health = health->currentData().toString();
        request.mana = mana->currentData().toString();
        request.name = name->currentData().toString();
        request.dead = dead->currentData().toString();
        request.outOfRange = outOfRange->currentData().toString();
        request.select = select->currentData().toString();
        request.healthNow = healthNow->currentData().toString();
        request.healthMax = healthMax->currentData().toString();
        request.manaNow = manaNow->currentData().toString();
        request.manaMax = manaMax->currentData().toString();
        request.portrait = portrait->currentData().toString();
        request.buffIcons = CheckedValues(buffIcons);
    }
    const auto generated = GeneratePartySystem(request, false, &error);
    if (!generated) {
        ReportAuthoring(error);
        return;
    }
    ReportAuthoring({});
    if (host_) host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(kPartyTriggerCategory)), 8000);
    OfferMapRuntime();
}

void GameUiModule::ShowTargetSystemDialog() {
    SystemDialog ui(page_, "gameUiTargetDialog", tr("目标框架"));
    auto* frame = ui.Combo("gameUiTargetFrame", tr("界面"));
    auto* source = ui.Combo("gameUiTargetSource", tr("来源"));
    source->addItem(tr("选中的单位"), static_cast<int>(TargetSource::Selection));
    source->addItem(tr("焦点（触发器设定）"), static_cast<int>(TargetSource::Focus));
    auto* systemName = new QLineEdit(tr("目标"), &ui.dialog);
    systemName->setObjectName(QStringLiteral("gameUiTargetSystemName"));
    ui.form->addRow(tr("系统名"), systemName);
    auto* key = new QLineEdit(QStringLiteral("Target"), &ui.dialog);
    key->setObjectName(QStringLiteral("gameUiTargetKey"));
    ui.form->addRow(tr("键"), key);
    // 换来源时，没改过的系统名与键跟着换成该来源的默认值。
    connect(source, &QComboBox::currentIndexChanged, &ui.dialog, [=] {
        const bool focus = source->currentData().toInt() == static_cast<int>(TargetSource::Focus);
        if (systemName->text() == tr("目标") || systemName->text() == tr("焦点")) systemName->setText(focus ? tr("焦点") : tr("目标"));
        if (key->text() == QLatin1String("Target") || key->text() == QLatin1String("Focus"))
            key->setText(focus ? QStringLiteral("Focus") : QStringLiteral("Target"));
    });
    auto* shown = ui.Combo("gameUiTargetShown", tr("显示"));
    auto* health = ui.Combo("gameUiTargetHealth", tr("生命"));
    auto* mana = ui.Combo("gameUiTargetMana", tr("法力"));
    auto* healthNow = ui.Combo("gameUiTargetHealthNow", tr("当前生命"));
    auto* healthMax = ui.Combo("gameUiTargetHealthMax", tr("最大生命"));
    auto* manaNow = ui.Combo("gameUiTargetManaNow", tr("当前法力"));
    auto* manaMax = ui.Combo("gameUiTargetManaMax", tr("最大法力"));
    auto* name = ui.Combo("gameUiTargetName", tr("名字"));
    auto* level = ui.Combo("gameUiTargetLevel", tr("等级"));
    auto* dead = ui.Combo("gameUiTargetDead", tr("死亡"));
    auto* portrait = ui.Combo("gameUiTargetPortrait", tr("头像"));
    auto* select = ui.Combo("gameUiTargetSelect", tr("点选"));
    auto* nameSource = ui.Combo("gameUiTargetNameSource", tr("名字来源"));
    nameSource->addItem(tr("英雄名或单位名"), static_cast<int>(UnitNameSource::Unit));
    nameSource->addItem(tr("玩家名"), static_cast<int>(UnitNameSource::Player));
    auto* players = new QSpinBox(&ui.dialog);
    players->setObjectName(QStringLiteral("gameUiTargetPlayers"));
    players->setRange(1, static_cast<int>(kTargetMaxPlayers));
    players->setValue(static_cast<int>(kTargetMaxPlayers));
    ui.form->addRow(tr("玩家数"), players);
    auto* buffIcons = ui.IconValues("gameUiTargetBuffIcons", tr("增减益图标"));
    auto* buffs = ui.ObjectList(project_, "gameUiTargetBuffs", tr("增减益"), QStringLiteral("buff"));
    auto* portraits = ui.ObjectList(project_, "gameUiTargetPortraits", tr("头像单位"), QStringLiteral("unit"));

    FillDescriptions(frame, Descriptions(), {}, tr("新建目标框架"), "target");
    const auto refill = [&, this] {
        const UiDescription* description = Description(frame->currentData().toString());
        FillValues(shown, description, UiValueType::Boolean, 0, false, {"shown", "visible", "show"});
        FillValues(health, description, UiValueType::Real, 0, true, {"hp", "health", "life"});
        FillValues(mana, description, UiValueType::Real, 0, true, {"mp", "mana"});
        FillValues(healthNow, description, UiValueType::Real, 0, true, {"hp_now", "health_now", "life_now"});
        FillValues(healthMax, description, UiValueType::Real, 0, true, {"hp_max", "health_max", "life_max"});
        FillValues(manaNow, description, UiValueType::Real, 0, true, {"mp_now", "mana_now"});
        FillValues(manaMax, description, UiValueType::Real, 0, true, {"mp_max", "mana_max"});
        FillValues(name, description, UiValueType::String, 0, true, {"name", "label"});
        FillValues(level, description, UiValueType::Integer, 0, true, {"level"});
        FillValues(dead, description, UiValueType::Boolean, 0, true, {"dead"});
        FillValues(portrait, description, UiValueType::String, 0, true, {"portrait", "avatar"});
        FillIconValues(buffIcons, description, 0);
        FillClickEvents(select, description);
        for (QWidget* box : std::initializer_list<QWidget*>{shown, health, mana, healthNow, healthMax, manaNow, manaMax, name, level, dead, portrait,
                 select, buffIcons}) {
            box->setEnabled(description != nullptr);
        }
    };
    connect(frame, &QComboBox::currentIndexChanged, &ui.dialog, refill);
    if (const int index = frame->findData(SelectedDescriptionId(tree_.get(), selected_)); index >= 0) frame->setCurrentIndex(index);
    refill();
    if (!ui.Run()) return;

    TargetSystemRequest request;
    request.frame = frame->currentData().toString();
    request.systemName = systemName->text().trimmed();
    request.key = key->text().trimmed();
    request.source = static_cast<TargetSource>(source->currentData().toInt());
    request.nameSource = static_cast<UnitNameSource>(nameSource->currentData().toInt());
    request.players = players->value();
    request.buffs = buffs->Ids();
    request.portraits = portraits->Ids();
    QString error;
    if (request.frame.startsWith(QLatin1Char('+'))) {
        const auto created = CreateFromPreset(QStringLiteral("target"), &error);
        if (!created) {
            ReportAuthoring(error);
            return;
        }
        using namespace TargetPreset;
        request.frame = *created;
        request.shown = Text(kShown);
        request.health = Text(kHealth);
        request.mana = Text(kMana);
        request.healthNow = Text(kHealthNow);
        request.healthMax = Text(kHealthMax);
        request.name = Text(kName);
        request.level = Text(kLevel);
        request.dead = Text(kDead);
        request.portrait = Text(kPortrait);
        request.select = Text(kSelect);
        for (const auto icon : kBuffs) request.buffIcons.append(Text(icon));
    } else {
        request.shown = shown->currentData().toString();
        request.health = health->currentData().toString();
        request.mana = mana->currentData().toString();
        request.healthNow = healthNow->currentData().toString();
        request.healthMax = healthMax->currentData().toString();
        request.manaNow = manaNow->currentData().toString();
        request.manaMax = manaMax->currentData().toString();
        request.name = name->currentData().toString();
        request.level = level->currentData().toString();
        request.dead = dead->currentData().toString();
        request.portrait = portrait->currentData().toString();
        request.select = select->currentData().toString();
        request.buffIcons = CheckedValues(buffIcons);
    }
    const auto generated = GenerateTargetSystem(request, false, &error);
    if (!generated) {
        ReportAuthoring(error);
        return;
    }
    ReportAuthoring({});
    if (host_) host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(kTargetTriggerCategory)), 8000);
    OfferMapRuntime();
}

void GameUiModule::ShowMeterSystemDialog() {
    SystemDialog ui(page_, "gameUiMeterDialog", tr("伤害统计"));
    auto* frame = ui.Combo("gameUiMeterFrame", tr("界面"));
    auto* systemName = new QLineEdit(tr("伤害"), &ui.dialog);
    systemName->setObjectName(QStringLiteral("gameUiMeterSystemName"));
    ui.form->addRow(tr("系统名"), systemName);
    auto* key = new QLineEdit(QStringLiteral("Damage"), &ui.dialog);
    key->setObjectName(QStringLiteral("gameUiMeterKey"));
    ui.form->addRow(tr("键"), key);
    auto* shown = ui.Combo("gameUiMeterShown", tr("显示"));
    auto* name = ui.Combo("gameUiMeterName", tr("名字"));
    auto* amount = ui.Combo("gameUiMeterAmount", tr("伤害"));
    auto* amountText = ui.Combo("gameUiMeterAmountText", tr("伤害文字"));
    auto* share = ui.Combo("gameUiMeterShare", tr("比例"));
    auto* percent = ui.Combo("gameUiMeterPercent", tr("占比"));
    auto* rate = ui.Combo("gameUiMeterRate", tr("每秒伤害"));
    auto* color = ui.Combo("gameUiMeterColor", tr("颜色"));
    auto* total = ui.Combo("gameUiMeterTotal", tr("总伤害"));
    auto* time = ui.Combo("gameUiMeterTime", tr("秒数"));
    auto* players = new QSpinBox(&ui.dialog);
    players->setObjectName(QStringLiteral("gameUiMeterPlayers"));
    players->setRange(1, static_cast<int>(kMeterMaxPlayers));
    players->setValue(static_cast<int>(kMeterMaxPlayers));
    ui.form->addRow(tr("玩家数"), players);
    auto* enemiesOnly = new QCheckBox(tr("只记打敌人的伤害"), &ui.dialog);
    enemiesOnly->setObjectName(QStringLiteral("gameUiMeterEnemiesOnly"));
    enemiesOnly->setChecked(true);
    ui.form->addRow(QString(), enemiesOnly);

    FillDescriptions(frame, Descriptions(), {}, tr("新建伤害统计"), "meter");
    // 每行的数值按“显示”数组的格数筛选；总伤害与秒数是单值。
    const auto refillRows = [&, this] {
        const UiDescription* description = Description(frame->currentData().toString());
        std::uint32_t size = 0;
        if (description) {
            for (const auto& value : description->values) {
                if (Text(value.id) == shown->currentData().toString()) size = value.size;
            }
        }
        const UiDescription* rows = size ? description : nullptr;
        FillValues(name, rows, UiValueType::String, size, true, {"name", "label"});
        FillValues(amount, rows, UiValueType::Real, size, true, {"amount", "damage"});
        FillValues(amountText, rows, UiValueType::String, size, true, {"amount_text", "damage_text"});
        FillValues(share, rows, UiValueType::Real, size, true, {"share", "ratio"});
        FillValues(percent, rows, UiValueType::Real, size, true, {"percent"});
        FillValues(rate, rows, UiValueType::Real, size, true, {"rate", "dps"});
        FillValues(color, rows, UiValueType::String, size, true, {"color", "colour"});
        FillValues(total, description, UiValueType::Real, 0, true, {"total"});
        FillValues(time, description, UiValueType::Real, 0, true, {"time", "seconds"});
    };
    const auto refill = [&, this] {
        const UiDescription* description = Description(frame->currentData().toString());
        shown->clear();
        if (description) {
            for (const auto& value : description->values) {
                if (value.type == UiValueType::Boolean && value.size > 0 && value.size <= kMeterMaxRows) shown->addItem(Labeled(value.label, value.id), Text(value.id));
            }
            Prefer(shown, {"shown", "visible", "show"});
        }
        refillRows();
        for (QWidget* box : std::initializer_list<QWidget*>{shown, name, amount, amountText, share, percent, rate, color, total, time}) {
            box->setEnabled(description != nullptr);
        }
    };
    connect(frame, &QComboBox::currentIndexChanged, &ui.dialog, refill);
    connect(shown, &QComboBox::currentIndexChanged, &ui.dialog, refillRows);
    if (const int index = frame->findData(SelectedDescriptionId(tree_.get(), selected_)); index >= 0) frame->setCurrentIndex(index);
    refill();
    if (!ui.Run()) return;

    MeterSystemRequest request;
    request.frame = frame->currentData().toString();
    request.systemName = systemName->text().trimmed();
    request.key = key->text().trimmed();
    request.players = players->value();
    request.enemiesOnly = enemiesOnly->isChecked();
    QString error;
    if (request.frame.startsWith(QLatin1Char('+'))) {
        const auto created = CreateFromPreset(QStringLiteral("meter"), &error);
        if (!created) {
            ReportAuthoring(error);
            return;
        }
        using namespace MeterPreset;
        request.frame = *created;
        request.shown = Text(kShown);
        request.name = Text(kName);
        request.amount = Text(kAmount);
        request.amountText = Text(kAmountText);
        request.share = Text(kShare);
        request.percent = Text(kPercent);
        request.rate = Text(kRate);
        request.color = Text(kColor);
        request.total = Text(kTotal);
        request.time = Text(kTime);
    } else {
        request.shown = shown->currentData().toString();
        request.name = name->currentData().toString();
        request.amount = amount->currentData().toString();
        request.amountText = amountText->currentData().toString();
        request.share = share->currentData().toString();
        request.percent = percent->currentData().toString();
        request.rate = rate->currentData().toString();
        request.color = color->currentData().toString();
        request.total = total->currentData().toString();
        request.time = time->currentData().toString();
    }
    const auto generated = GenerateMeterSystem(request, false, &error);
    if (!generated) {
        ReportAuthoring(error);
        return;
    }
    ReportAuthoring({});
    if (host_) host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(kMeterTriggerCategory)), 8000);
    OfferMapRuntime();
}

void GameUiModule::ShowTalentSystemDialog() {
    SystemDialog ui(page_, "gameUiTalentDialog", tr("天赋树"));
    auto* frame = ui.Combo("gameUiTalentFrame", tr("界面"));
    FillDescriptions(frame, Descriptions(), {}, tr("新建天赋树"), "talents");
    if (const int index = frame->findData(SelectedDescriptionId(tree_.get(), selected_)); index >= 0) frame->setCurrentIndex(index);
    auto* name = new QLineEdit(QStringLiteral("天赋"), &ui.dialog);
    name->setObjectName(QStringLiteral("gameUiTalentName"));
    ui.form->addRow(tr("名字"), name);
    auto* levels = new QLineEdit(QStringLiteral("5 10 15 20 25"), &ui.dialog);
    levels->setObjectName(QStringLiteral("gameUiTalentLevels"));
    ui.form->addRow(tr("解锁等级"), levels);
    auto* syncUnit = ui.Combo("gameUiTalentSyncUnit", tr("同步单位"));
    for (const auto& entry : QtApp::BuildObjectViewEntries(project_)) {
        if (entry.kind == QLatin1String("unit")) syncUnit->addItem(QStringLiteral("%1  %2").arg(entry.name, entry.id), entry.id);
    }
    Prefer(syncUnit, {"hfoo"});
    auto* players = new QSpinBox(&ui.dialog);
    players->setObjectName(QStringLiteral("gameUiTalentPlayers"));
    players->setRange(1, static_cast<int>(kTalentMaxPlayers));
    players->setValue(12);
    ui.form->addRow(tr("玩家数"), players);
    auto* afterPick = ui.Combo("gameUiTalentAfterPick", tr("选定后运行"));
    afterPick->addItem(tr("无"), QString());
    if (project_.triggerDocument_) {
        for (const auto& row : project_.triggerDocument_->triggers) {
            if (row.name.text.find("Talent_") == std::string::npos) afterPick->addItem(Text(row.name.text), Text(row.name.text));
        }
    }
    if (!ui.Run()) return;

    using namespace TalentPreset;
    TalentSystemRequest request;
    request.name = name->text().trimmed();
    for (const auto& part : levels->text().split(QLatin1Char(' '), Qt::SkipEmptyParts)) request.unlockLevels.append(part.toInt());
    request.syncUnitType = syncUnit->currentData().toString();
    request.players = players->value();
    request.afterPick = afterPick->currentData().toString();
    request.frame = frame->currentData().toString();
    QString error;
    if (request.frame.startsWith(QLatin1Char('+'))) {
        const auto created = CreateFromPreset(QStringLiteral("talents"), &error);
        if (!created) {
            ReportAuthoring(error);
            return;
        }
        request.frame = *created;
    }
    request.shown = Text(kShown);
    request.entryShown = Text(kEntryShown);
    request.entry = Text(kEntry);
    request.title = Text(kTitle);
    request.points = Text(kPoints);
    request.level = Text(kLevel);
    request.open = Text(kOpen);
    request.close = Text(kClose);
    request.tipShown = Text(kTipShown);
    request.tipTitle = Text(kTipTitle);
    request.tipText = Text(kTipText);
    for (std::size_t c = 0; c < kOptions; ++c) {
        request.columns.append({Text(kIcons[c]), Text(kNames[c]), Text(kPicked[c]), Text(kPicks[c]), Text(kEnters[c]), Text(kLeaves[c])});
    }
    const auto generated = GenerateTalentSystem(request, false, &error);
    if (!generated) {
        ReportAuthoring(error);
        return;
    }
    ReportAuthoring({});
    if (host_) host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(kTalentTriggerCategory)), 8000);
    OfferMapRuntime();
}

void GameUiModule::OfferMapRuntime() {
    const auto generation = project_.editRevision_.Generation();
    if (runtimeOfferDeclined_ == generation) return;
    const auto requirement = QueryRuntimeRequirement();
    if (requirement.approved) return;
    const auto report = [this](const char* code, const QString& message) {
        authoringDiagnostics_.push_back(Diagnostic{DiagnosticSeverity::Warning, "GameUi", code, message.toStdString(), std::nullopt});
        UpdateProblems();
    };
    if (!requirement.problem.isEmpty()) return report("AUI_RUNTIME_STATE", requirement.problem);
    std::vector<const Services::MapRuntimePackageInfo*> usable;
    for (const auto& package : requirement.packages) {
        if (package.available) usable.push_back(&package);
    }
    if (usable.empty()) {
        return report("AUI_RUNTIME_MISSING", tr("界面触发器要用 Audastra 运行库，本机没有安装可用的运行库包，测试运行和保存会拒绝"));
    }
    const auto label = [](const Services::MapRuntimePackageInfo& package) { return Text(package.id) + QLatin1Char(' ') + Text(package.version); };
    // 地图里已锁定的包优先；有多个可选时让作者挑。
    const auto locked = std::find_if(usable.begin(), usable.end(),
        [&](const auto* package) { return package->packageSha256 == requirement.settings.packageSha256; });
    const Services::MapRuntimePackageInfo* chosen = locked != usable.end() ? *locked : usable.front();
    if (locked == usable.end() && usable.size() > 1) {
        QStringList items;
        for (const auto* package : usable) items.append(label(*package));
        bool picked = false;
        const auto item = QInputDialog::getItem(page_, tr("启用运行库"), tr("运行库包"), items, 0, false, &picked);
        if (!picked) {
            runtimeOfferDeclined_ = generation;
            return;
        }
        chosen = usable[static_cast<std::size_t>(std::max<qsizetype>(0, items.indexOf(item)))];
    }
    QString text = tr("界面触发器要用 Audastra 运行库：地图会带上原生代码（DLL），只支持经典版 1.27a。\n\n包：%1\nDLL：%2")
                       .arg(label(*chosen), Text(chosen->payloadSha256).left(16));
    if (chosen->internalOnly) text += tr("\n\n这个包只能用于测试运行和内部验收副本，正常保存会拒绝。");
    text += tr("\n\n为本图启用？");
    if (QMessageBox::question(page_, tr("启用运行库"), text) != QMessageBox::Yes) {
        runtimeOfferDeclined_ = generation;
        return;
    }
    const auto mode = requirement.settings.mode == Services::MapRuntimeMode::Disabled ? Services::MapRuntimeMode::Automatic
                                                                                      : requirement.settings.mode;
    const auto applied = QtProject::PublishMapRuntimeSettings(project_, {mode, chosen->packageSha256}, tr("启用地图运行库"));
    if (!applied.Succeeded()) {
        return report("AUI_RUNTIME_ENABLE", applied.diagnostics.empty() ? tr("启用运行库失败") : Text(applied.diagnostics.front().message));
    }
    if (host_) host_->ShowStatusMessage(tr("已为本图启用运行库 %1").arg(label(*chosen)), 8000);
}

std::optional<QString> GameUiModule::CreateFromPreset(const QString& preset, QString* error) {
    QString id = preset;
    for (int n = 2; Description(id); ++n) id = preset + QString::number(n);
    if (!CreateDescription(id, {}, error, preset) || !GenerateDescriptionTriggers(id, false, error)) return std::nullopt;
    return id;
}

} // namespace War3Map::Modules
