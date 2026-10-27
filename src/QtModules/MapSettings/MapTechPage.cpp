#include "MapTechPage.h"

#include "EditorProject.h"
#include "Icons/IconProvider.h"
#include "ObjectNames.h"
#include "ProjectMapCustomSettings.h"
#include "ProjectObjectCatalog.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace War3Map::Modules {
namespace {

using Section = Services::MapCustomSettingsSection;
using Action = Services::MapCustomSettingsAction;
using ObjectData::ObjectType;

QString RawText(const MapData::SettingsRawcode& id) {
    QString text;
    for (const auto byte : id) text.append(QChar(static_cast<char16_t>(static_cast<unsigned char>(byte))));
    return text;
}

std::optional<MapData::SettingsRawcode> RawCode(const QString& text) {
    if (text.size() != 4) return std::nullopt;
    MapData::SettingsRawcode id{};
    for (int i = 0; i < 4; ++i) {
        const auto unit = text.at(i).unicode();
        if (unit < 0x20 || unit > 0x7e) return std::nullopt;
        id[static_cast<std::size_t>(i)] = static_cast<std::byte>(unit);
    }
    return id;
}

// 科技限制的对象可能是单位、物品或技能；只在恰好一类中存在时给出类别。
std::optional<ObjectType> TechObjectType(const QtProject::EditorProject& project, const std::string& raw,
    std::initializer_list<ObjectType> types) {
    std::optional<ObjectType> found;
    for (const auto type : types) {
        const auto* document = project.objectDocuments_[static_cast<std::size_t>(type)].get();
        const bool exists = (document && document->FindObject({type, raw}))
            || (project.standardCatalog_ && project.standardCatalog_->Find(type, raw));
        if (!exists) continue;
        if (found) return std::nullopt;
        found = type;
    }
    return found;
}

QString AvailabilityText(std::int32_t value) {
    switch (value) {
    case 0: return QObject::tr("不可用");
    case 1: return QObject::tr("可用");
    case 2: return QObject::tr("已研究");
    default: return QObject::tr("未知 %1").arg(value);
    }
}

// 表单只显示首个错误；服务给批次内命令加的“operations[n]: ”前缀对单条命令没有意义。
QString FirstProblem(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Error) continue;
        QString message = QString::fromStdString(diagnostic.message);
        if (message.startsWith(QStringLiteral("operations["))) message = message.section(QStringLiteral(": "), 1);
        return message;
    }
    return diagnostics.empty() ? QObject::tr("操作失败。") : QString::fromStdString(diagnostics.front().message);
}

struct Choice {
    QString raw;
    QString display;
};

// 一行科技设置的表单：玩家、对象、（升级）等级与状态。提交失败时保留输入并显示原因。
class TechRowDialog final : public QDialog {
public:
    using Submit = std::function<QString(std::uint32_t mask, const QString& raw, int level, int availability)>;
    TechRowDialog(const QString& title, const std::vector<MapData::MapPlayerSettings>& players, std::uint32_t mask,
        const std::vector<Choice>& objects, const QString& current, bool upgrade, int level, int availability,
        Submit submit, QWidget* parent)
        : QDialog(parent), submit_(std::move(submit)) {
        setObjectName(QStringLiteral("techRowDialog"));
        setWindowTitle(title);
        setMinimumWidth(440);
        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        object_ = new QComboBox;
        object_->setObjectName(QStringLiteral("techObject"));
        object_->setEditable(true);
        object_->setInsertPolicy(QComboBox::NoInsert);
        object_->completer()->setFilterMode(Qt::MatchContains);
        object_->completer()->setCompletionMode(QCompleter::PopupCompletion);
        bool listed = current.isEmpty();
        for (const auto& choice : objects) {
            object_->addItem(choice.display, choice.raw);
            listed = listed || choice.raw == current;
        }
        if (!listed) object_->insertItem(0, current, current); // 原有对象不在候选中时原样保留
        object_->setCurrentIndex(current.isEmpty() ? -1 : object_->findData(current));
        form->addRow(upgrade ? tr("升级") : tr("对象"), object_);
        if (upgrade) {
            level_ = new QSpinBox;
            level_->setObjectName(QStringLiteral("techLevel"));
            level_->setRange(1, 1000);
            level_->setValue(level + 1);
            level_->setFixedWidth(84);
            availability_ = new QComboBox;
            availability_->setObjectName(QStringLiteral("techAvailability"));
            for (int value = 0; value <= 2; ++value) availability_->addItem(AvailabilityText(value), value);
            availability_->setCurrentIndex(std::clamp(availability, 0, 2));
            form->addRow(tr("等级"), level_);
            form->addRow(tr("状态"), availability_);
        }
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(14);
        for (std::size_t i = 0; i < players.size(); ++i) {
            const auto number = players[i].number;
            auto* box = new QCheckBox(tr("玩家 %1").arg(number + 1));
            box->setObjectName(QStringLiteral("techPlayer%1").arg(number));
            box->setChecked(number >= 0 && number < 32 && (mask & (1u << number)));
            box->setProperty("playerNumber", number);
            grid->addWidget(box, static_cast<int>(i / 4), static_cast<int>(i % 4));
            players_.push_back(box);
        }
        form->addRow(tr("玩家"), grid);
        layout->addLayout(form);
        problem_ = new QLabel;
        problem_->setObjectName(QStringLiteral("formProblem"));
        problem_->setWordWrap(true);
        problem_->hide();
        layout->addWidget(problem_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &TechRowDialog::Accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        // 保留原掩码中不在当前玩家表里的位，表单不替作者清掉。
        for (const auto* box : players_) {
            const int number = box->property("playerNumber").toInt();
            if (number >= 0 && number < 32) hiddenMask_ &= ~(1u << number);
        }
        hiddenMask_ &= mask;
    }

private:
    void Accept() {
        std::uint32_t mask = hiddenMask_;
        for (const auto* box : players_) {
            const int number = box->property("playerNumber").toInt();
            if (box->isChecked() && number >= 0 && number < 32) mask |= 1u << number;
        }
        QString raw = object_->currentData().toString();
        if (object_->currentIndex() < 0 || object_->currentText() != object_->itemText(object_->currentIndex())) {
            // 手输的四字符代码只有在候选里存在时才接受。
            const int index = object_->findData(object_->currentText().trimmed());
            raw = index >= 0 ? object_->itemData(index).toString() : QString{};
        }
        QString problem;
        if (raw.isEmpty()) problem = tr("请选择对象。");
        else if (!mask) problem = tr("至少选择一个玩家。");
        else problem = submit_(mask, raw, level_ ? level_->value() - 1 : 0, availability_ ? availability_->currentData().toInt() : 0);
        if (problem.isEmpty()) {
            accept();
            return;
        }
        problem_->setText(problem);
        problem_->show();
    }

    Submit submit_;
    QComboBox* object_ = nullptr;
    QSpinBox* level_ = nullptr;
    QComboBox* availability_ = nullptr;
    std::vector<QCheckBox*> players_;
    std::uint32_t hiddenMask_ = 0xffffffffu;
    QLabel* problem_ = nullptr;
};

QTableWidget* MakeTable(const QString& name, int columns, QWidget* parent) {
    auto* table = new QTableWidget(0, columns, parent);
    table->setObjectName(name);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(28);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setShowGrid(false);
    table->setIconSize(QSize(20, 20));
    table->setMinimumHeight(180);
    return table;
}

} // namespace

MapTechPage::MapTechPage(QtProject::EditorProject& project, const MapData::MapSettings& settings, bool raw,
    const QString& readOnly, Commit commit, QWidget* parent)
    : QWidget(parent), project_(project), settings_(settings), raw_(raw), editable_(readOnly.isEmpty()),
      commit_(std::move(commit)) {
    setObjectName(QStringLiteral("mapTechPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    const auto header = [this, layout](const QString& title, const char* name, Section section, QPushButton*& remove) {
        auto* row = new QHBoxLayout;
        auto* label = new QLabel(title);
        label->setObjectName(QStringLiteral("sectionTitle"));
        row->addWidget(label);
        row->addStretch(1);
        auto* add = new QPushButton(tr("添加…"));
        add->setObjectName(QStringLiteral("%1Add").arg(QLatin1String(name)));
        remove = new QPushButton(tr("删除"));
        remove->setObjectName(QStringLiteral("%1Delete").arg(QLatin1String(name)));
        remove->setEnabled(false);
        row->addWidget(add);
        row->addWidget(remove);
        add->setVisible(editable_);
        remove->setVisible(editable_);
        layout->addLayout(row);
        connect(add, &QPushButton::clicked, this, [this, section] { EditRow(section, -1); });
        connect(remove, &QPushButton::clicked, this, [this, section] { DeleteRow(section); });
    };
    header(tr("升级可用性"), "techUpgrades", Section::Upgrades, deleteUpgrade_);
    upgrades_ = MakeTable(QStringLiteral("techUpgrades"), 4, this);
    layout->addWidget(upgrades_, 1);
    header(tr("科技限制"), "techRestrictions", Section::TechRestrictions, deleteRestriction_);
    restrictions_ = MakeTable(QStringLiteral("techRestrictions"), 2, this);
    layout->addWidget(restrictions_, 1);
    const auto& custom = *settings_.custom;
    auto* random = new QLabel(tr("随机单位表 %1 · 随机物品表 %2 · 只读")
        .arg(custom.randomUnits.size()).arg(custom.randomItems.size()) + (editable_ ? QString{} : QStringLiteral("\n") + readOnly));
    random->setWordWrap(true);
    random->setObjectName(QStringLiteral("paneHint"));
    layout->addWidget(random);
    problem_ = new QLabel;
    problem_->setObjectName(QStringLiteral("formProblem"));
    problem_->setWordWrap(true);
    problem_->hide();
    layout->addWidget(problem_);
    for (auto [table, remove, section] : {std::tuple{upgrades_, deleteUpgrade_, Section::Upgrades},
             std::tuple{restrictions_, deleteRestriction_, Section::TechRestrictions}}) {
        connect(table, &QTableWidget::itemSelectionChanged, remove, [table, remove] {
            remove->setEnabled(table->currentRow() >= 0 && !table->selectedItems().isEmpty());
        });
        connect(table, &QTableWidget::cellDoubleClicked, this, [this, section](int row, int) {
            if (editable_) EditRow(section, row);
        });
    }
    SetRaw(raw_);
}

void MapTechPage::SetRaw(bool raw) {
    raw_ = raw;
    upgrades_->setHorizontalHeaderLabels(raw_
        ? QStringList{QStringLiteral("id"), QStringLiteral("level"), QStringLiteral("availability"), QStringLiteral("playerMask")}
        : QStringList{tr("升级"), tr("等级"), tr("状态"), tr("玩家")});
    restrictions_->setHorizontalHeaderLabels(raw_
        ? QStringList{QStringLiteral("id"), QStringLiteral("playerMask")} : QStringList{tr("不可用对象"), tr("玩家")});
    Fill();
}

QString MapTechPage::PlayersText(std::uint32_t mask) const {
    if (raw_) return QStringLiteral("0x%1").arg(mask, 8, 16, QChar('0'));
    std::uint32_t known = 0;
    QStringList names;
    for (const auto& player : settings_.players) {
        if (player.number < 0 || player.number >= 32) continue;
        known |= 1u << player.number;
        if (mask & (1u << player.number)) names.append(tr("玩家 %1").arg(player.number + 1));
    }
    if (known && (mask & known) == known && !(mask & ~known)) return tr("全部玩家");
    if (mask & ~known) names.append(tr("其他位 0x%1").arg(mask & ~known, 0, 16));
    return names.join(tr("、"));
}

QString MapTechPage::ObjectText(const MapData::SettingsRawcode& id, QIcon* icon, QString* tooltip) const {
    const QString raw = RawText(id);
    *tooltip = raw;
    if (raw_) return raw;
    const auto type = TechObjectType(project_, raw.toStdString(), {ObjectType::Upgrade, ObjectType::Unit, ObjectType::Item, ObjectType::Ability});
    if (!type) return raw;
    const auto shown = ResolveProjectObjectPresentation(project_, {*type, raw.toStdString()});
    if (project_.iconProvider_ && !shown.iconPath.isEmpty()) *icon = project_.iconProvider_->IconForTexture(shown.iconPath);
    return shown.name.isEmpty() ? raw : shown.name;
}

void MapTechPage::Fill() {
    // 名称、头像与候选来自当前层序的物编目录；尚未载入时在此载入（失败则显示原始代码）。
    if (!project_.standardCatalog_) (void)QtProject::EnsureProjectObjectCatalog(project_);
    const auto& custom = *settings_.custom;
    const auto objectItem = [this](const MapData::SettingsRawcode& id) {
        QIcon icon;
        QString tooltip;
        auto* item = new QTableWidgetItem(ObjectText(id, &icon, &tooltip));
        item->setIcon(icon);
        item->setToolTip(tooltip);
        return item;
    };
    upgrades_->setRowCount(static_cast<int>(custom.upgrades.size()));
    for (int row = 0; row < upgrades_->rowCount(); ++row) {
        const auto& upgrade = custom.upgrades[static_cast<std::size_t>(row)];
        upgrades_->setItem(row, 0, objectItem(upgrade.id));
        upgrades_->setItem(row, 1, new QTableWidgetItem(QString::number(raw_ ? upgrade.level : upgrade.level + 1)));
        upgrades_->setItem(row, 2, new QTableWidgetItem(raw_ ? QString::number(upgrade.availability) : AvailabilityText(upgrade.availability)));
        upgrades_->setItem(row, 3, new QTableWidgetItem(PlayersText(upgrade.playerMask)));
    }
    restrictions_->setRowCount(static_cast<int>(custom.techRestrictions.size()));
    for (int row = 0; row < restrictions_->rowCount(); ++row) {
        const auto& restriction = custom.techRestrictions[static_cast<std::size_t>(row)];
        restrictions_->setItem(row, 0, objectItem(restriction.id));
        restrictions_->setItem(row, 1, new QTableWidgetItem(PlayersText(restriction.playerMask)));
    }
    upgrades_->setColumnWidth(0, 220);
    upgrades_->setColumnWidth(1, 56);
    upgrades_->setColumnWidth(2, 80);
    restrictions_->setColumnWidth(0, 260);
}

void MapTechPage::EditRow(Section section, int row) {
    const bool upgrade = section == Section::Upgrades;
    const auto& custom = *settings_.custom;
    std::vector<Choice> choices;
    const auto addChoices = [&](const char* uiType, const QString& kind) {
        for (const auto& choice : ProjectObjectChoices(project_, uiType))
            choices.push_back({choice.rawcode, (kind.isEmpty() ? QString{} : kind + QStringLiteral(" · "))
                + choice.name + QStringLiteral(" (") + choice.rawcode + QChar(')')});
    };
    if (upgrade) addChoices("techcode", {});
    else {
        addChoices("unitcode", tr("单位"));
        addChoices("itemcode", tr("物品"));
        addChoices("abilcode", tr("技能"));
    }
    std::uint32_t mask = 0;
    QString current;
    int level = 0, availability = 1;
    if (row >= 0 && upgrade && row < static_cast<int>(custom.upgrades.size())) {
        const auto& value = custom.upgrades[static_cast<std::size_t>(row)];
        mask = value.playerMask;
        current = RawText(value.id);
        level = value.level;
        availability = value.availability;
    } else if (row >= 0 && !upgrade && row < static_cast<int>(custom.techRestrictions.size())) {
        const auto& value = custom.techRestrictions[static_cast<std::size_t>(row)];
        mask = value.playerMask;
        current = RawText(value.id);
    } else row = -1;
    const QString what = upgrade ? tr("升级可用性") : tr("科技限制");
    const QString label = (row < 0 ? tr("添加%1") : tr("修改%1")).arg(what);
    TechRowDialog dialog(label, settings_.players, mask, choices, current, upgrade, level, availability,
        [&](std::uint32_t chosenMask, const QString& raw, int chosenLevel, int chosenAvailability) -> QString {
            const auto id = RawCode(raw);
            if (!id) return tr("对象代码无效。");
            Services::MapCustomSettingsCommand command;
            command.section = section;
            if (upgrade) command.value = MapData::MapUpgradeAvailability{chosenMask, *id, chosenLevel, chosenAvailability};
            else command.value = MapData::MapTechRestriction{chosenMask, *id};
            if (row < 0) {
                command.action = Action::Append;
                command.expectedCount = static_cast<std::uint32_t>(upgrade ? custom.upgrades.size() : custom.techRestrictions.size());
            } else {
                command.action = Action::Update;
                command.index = static_cast<std::uint32_t>(row);
                const Services::MapCustomSettingsValue previous = upgrade
                    ? Services::MapCustomSettingsValue{custom.upgrades[static_cast<std::size_t>(row)]}
                    : Services::MapCustomSettingsValue{custom.techRestrictions[static_cast<std::size_t>(row)]};
                const auto key = Services::MapCustomSettingsEditing::RecordKey(section, previous, settings_.formatVersion);
                if (!key.Succeeded()) return QString::fromStdString(key.diagnostics.front().message);
                command.expectedKey = *key.value;
            }
            return commit_(std::move(command), label);
        }, window());
    dialog.exec();
}

void MapTechPage::DeleteRow(Section section) {
    const bool upgrade = section == Section::Upgrades;
    const int row = (upgrade ? upgrades_ : restrictions_)->currentRow();
    const auto& custom = *settings_.custom;
    if (row < 0 || row >= static_cast<int>(upgrade ? custom.upgrades.size() : custom.techRestrictions.size())) return;
    const Services::MapCustomSettingsValue previous = upgrade
        ? Services::MapCustomSettingsValue{custom.upgrades[static_cast<std::size_t>(row)]}
        : Services::MapCustomSettingsValue{custom.techRestrictions[static_cast<std::size_t>(row)]};
    const auto key = Services::MapCustomSettingsEditing::RecordKey(section, previous, settings_.formatVersion);
    QString problem;
    if (!key.Succeeded()) problem = QString::fromStdString(key.diagnostics.front().message);
    else {
        Services::MapCustomSettingsCommand command;
        command.section = section;
        command.action = Action::Delete;
        command.index = static_cast<std::uint32_t>(row);
        command.expectedKey = *key.value;
        problem = commit_(std::move(command), upgrade ? tr("删除升级可用性") : tr("删除科技限制"));
    }
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
}

MapTechPage::Commit MapTechPage::ProjectCommit(QtProject::EditorProject& project, QObject* context,
    std::function<QString()> blocked, std::function<void(const QString&)> report) {
    QPointer<QtProject::EditorProject> guard(&project);
    QPointer<QObject> owner(context);
    return [guard, owner, blocked, report](Services::MapCustomSettingsCommand command, const QString& label) -> QString {
        if (!guard || !owner) return QObject::tr("地图已关闭。");
        if (blocked) if (const auto reason = blocked(); !reason.isEmpty()) return reason;
        auto* session = guard->MapEdits();
        if (!session) return QObject::tr("当前没有打开地图。");
        const auto current = std::make_shared<const MapDocument>(session->Snapshot());
        const auto identity = Services::MapCustomSettingsEditing::MemberIdentity(*current);
        if (!identity.Succeeded()) return FirstProblem(identity.diagnostics);
        auto validation = QtProject::CaptureProjectMapCustomSettingsValidation(*guard);
        if (!validation.Succeeded()) return FirstProblem(validation.diagnostics);
        Services::MapCustomSettingsEditRequest request;
        request.expectedMemberSha256 = *identity.value;
        request.operations.push_back(std::move(command));
        auto plan = Services::MapCustomSettingsEditing::Prepare(current, request, *validation.value);
        if (!plan.Succeeded()) return FirstProblem(plan.diagnostics);
        if (!plan.value->HasChanges()) return {};
        if (!plan.value->CanCommit()) return QObject::tr("地图含随机单位/物品表，其初始化尚未接通，科技设置暂只能查看。");
        auto prepared = std::make_shared<const Services::PreparedMapCustomSettingsEdit>(std::move(*plan.value));
        auto checks = std::make_shared<const Services::MapCustomSettingsValidation>(std::move(*validation.value));
        // 发布会发出地图变化并重建设置页（含发起本次提交的表单与按钮），推迟到当前调用栈结束后。
        QTimer::singleShot(0, owner.data(), [guard, prepared, checks, label, report] {
            if (!guard) return;
            const auto applied = QtProject::ApplyProjectMapCustomSettings(*guard, *prepared, *checks, label, "地图设置页修改科技设置");
            if (!applied.Succeeded() && report) report(FirstProblem(applied.diagnostics));
        });
        return {};
    };
}

} // namespace War3Map::Modules
