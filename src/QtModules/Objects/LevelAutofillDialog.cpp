#include "LevelAutofillDialog.h"
#include "ObjectModuleShared.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

using War3Map::ObjectData::LevelAutofill;
using War3Map::ObjectData::LevelAutofillKind;

LevelAutofillDialog::LevelAutofillDialog(const QString& fieldName, LevelAutofillKind kind, bool multiline,
    Values current, Commit commit, QWidget* parent)
    : QDialog(parent), kind_(kind), current_(std::move(current)), commit_(std::move(commit)) {
    setObjectName(QStringLiteral("levelAutofillDialog"));
    setWindowTitle(tr("自动填充等级 — %1").arg(fieldName));
    setMinimumWidth(460);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const int first = static_cast<int>(current_.front().first);
    const int last = static_cast<int>(current_.back().first);
    start_ = new QSpinBox;
    start_->setObjectName(QStringLiteral("autofillStartLevel"));
    start_->setRange(first, last);
    end_ = new QSpinBox;
    end_->setObjectName(QStringLiteral("autofillEndLevel"));
    end_->setRange(first, last);
    end_->setValue(last);
    auto* range = new QHBoxLayout;
    for (auto* spin : {start_, end_}) spin->setFixedWidth(84);
    range->addWidget(start_);
    range->addWidget(new QLabel(tr("至")));
    range->addWidget(end_);
    range->addStretch(1);
    form->addRow(tr("等级"), range);
    const auto edit = [this](const char* name, const QString& value) {
        auto* line = new QLineEdit(value);
        line->setObjectName(QLatin1String(name));
        connect(line, &QLineEdit::textEdited, this, &LevelAutofillDialog::Recompute);
        return line;
    };
    const auto numberEdit = [form, &edit](const QString& label, const char* name, const QString& value) {
        auto* line = edit(name, value);
        form->addRow(label, line);
        return line;
    };
    if (kind_ == LevelAutofillKind::Text) {
        if (multiline) {
            baseBlock_ = new QPlainTextEdit;
            baseBlock_->setObjectName(QStringLiteral("autofillBaseText"));
            baseBlock_->setFixedHeight(90);
            form->addRow(tr("基础文本"), baseBlock_);
            connect(baseBlock_, &QPlainTextEdit::textChanged, this, [this] {
                baseEdited_ = true;
                Recompute();
            });
        } else {
            baseLine_ = numberEdit(tr("基础文本"), "autofillBaseText", {});
            connect(baseLine_, &QLineEdit::textEdited, this, [this] { baseEdited_ = true; });
        }
        token_ = numberEdit(tr("用等级替代"), "autofillLevelToken", {});
    } else {
        base_ = numberEdit(tr("基础值"), "autofillBase", {});
        connect(base_, &QLineEdit::textEdited, this, [this] { baseEdited_ = true; });
        base_->setFixedWidth(140);
        // 原版三个因素按公式排成一行：上一级 × 上一个值因素 + 等级 × 等级因素 + 固定因素。
        previousFactor_ = edit("autofillPreviousFactor", QStringLiteral("1"));
        levelFactor_ = edit("autofillLevelFactor", QStringLiteral("0"));
        constant_ = edit("autofillConstant", QStringLiteral("0"));
        previousFactor_->setToolTip(tr("上一个值因素"));
        levelFactor_->setToolTip(tr("等级因素"));
        constant_->setToolTip(tr("固定因素"));
        for (auto* factor : {previousFactor_, levelFactor_, constant_}) factor->setFixedWidth(72);
        auto* formula = new QHBoxLayout;
        formula->addWidget(new QLabel(tr("上一级 ×")));
        formula->addWidget(previousFactor_);
        formula->addWidget(new QLabel(tr("+ 等级 ×")));
        formula->addWidget(levelFactor_);
        formula->addWidget(new QLabel(QStringLiteral("+")));
        formula->addWidget(constant_);
        formula->addStretch(1);
        form->addRow(tr("后续各级"), formula);
    }
    layout->addLayout(form);

    table_ = new QTableWidget(0, 3);
    table_->setObjectName(QStringLiteral("autofillPreview"));
    table_->setHorizontalHeaderLabels({tr("等级"), tr("当前值"), tr("新值")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    War3Map::Modules::ObjectDetail::ConfigureTable(table_);
    table_->verticalHeader()->setDefaultSectionSize(26);
    table_->setColumnWidth(0, 56);
    table_->setColumnWidth(1, 170);
    table_->setMinimumHeight(220);
    layout->addWidget(table_, 1);

    problem_ = new QLabel;
    problem_->setObjectName(QStringLiteral("formProblem"));
    problem_->setWordWrap(true);
    problem_->hide();
    layout->addWidget(problem_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    okButton_ = buttons->button(QDialogButtonBox::Ok);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &LevelAutofillDialog::Submit);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(start_, &QSpinBox::valueChanged, this, [this](int value) {
        end_->setMinimum(value);
        SyncBaseFromStart();
        Recompute();
    });
    connect(end_, &QSpinBox::valueChanged, this, &LevelAutofillDialog::Recompute);
    SyncBaseFromStart();
    Recompute();
}

QString LevelAutofillDialog::CurrentValue(std::uint32_t level) const {
    for (const auto& [currentLevel, value] : current_)
        if (currentLevel == level) return value;
    return {};
}

QString LevelAutofillDialog::BaseText() const {
    if (baseBlock_) return baseBlock_->toPlainText();
    if (baseLine_) return baseLine_->text();
    return base_ ? base_->text() : QString{};
}

void LevelAutofillDialog::SyncBaseFromStart() {
    // 作者没改过基础值时跟随开始等级的现值。
    if (baseEdited_) return;
    const QString value = CurrentValue(static_cast<std::uint32_t>(start_->value()));
    if (baseBlock_) {
        const QSignalBlocker block(baseBlock_);
        baseBlock_->setPlainText(value);
    } else if (baseLine_) baseLine_->setText(value);
    else if (base_) {
        bool numeric = false;
        value.toDouble(&numeric);
        base_->setText(numeric ? value : QStringLiteral("0"));
    }
}

void LevelAutofillDialog::Recompute() {
    LevelAutofill spec;
    spec.startLevel = static_cast<std::uint32_t>(start_->value());
    spec.endLevel = static_cast<std::uint32_t>(end_->value());
    QString problem;
    if (kind_ == LevelAutofillKind::Text) {
        spec.baseText = BaseText().toStdString();
        spec.levelToken = token_->text().toStdString();
    } else {
        const struct { QLineEdit* edit; double* target; QString label; } numbers[] = {
            {base_, &spec.base, tr("基础值")}, {previousFactor_, &spec.previousFactor, tr("上一个值因素")},
            {levelFactor_, &spec.levelFactor, tr("等级因素")}, {constant_, &spec.constant, tr("固定因素")}};
        for (const auto& number : numbers) {
            bool valid = false;
            *number.target = number.edit->text().trimmed().toDouble(&valid);
            if (!valid && problem.isEmpty()) problem = tr("%1不是数字。").arg(number.label);
        }
    }
    preview_.clear();
    if (problem.isEmpty()) {
        const auto result = ComputeLevelAutofill(spec, kind_);
        if (!result.Succeeded()) problem = QString::fromStdString(result.diagnostics.front().message);
        else {
            for (const auto& value : *result.value) {
                const bool exists = std::any_of(current_.begin(), current_.end(),
                    [&](const auto& entry) { return entry.first == value.level; });
                if (exists) preview_.emplace_back(value.level, QString::fromStdString(value.text));
            }
        }
    }
    const auto shown = [](QString text) {
        return text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QChar('\n'), QStringLiteral("  ↵  "));
    };
    table_->setRowCount(0);
    for (const auto& [level, value] : current_) {
        if (level < spec.startLevel || level > spec.endLevel) continue;
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setItem(row, 0, new QTableWidgetItem(QString::number(level)));
        table_->setItem(row, 1, new QTableWidgetItem(shown(value)));
        const auto next = std::find_if(preview_.begin(), preview_.end(), [level](const auto& entry) { return entry.first == level; });
        auto* item = new QTableWidgetItem(next == preview_.end() ? QStringLiteral("—") : shown(next->second));
        // 与字段表一致：将成为修改的值用品红色。
        if (next != preview_.end() && next->second != value) item->setForeground(QColor(0xd8, 0x6c, 0xd8));
        table_->setItem(row, 2, item);
    }
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
    okButton_->setEnabled(problem.isEmpty() && !preview_.empty());
}

void LevelAutofillDialog::Submit() {
    if (preview_.empty()) return;
    const QString problem = commit_ ? commit_(preview_) : QString{};
    if (problem.isEmpty()) {
        accept();
        return;
    }
    problem_->setText(problem);
    problem_->show();
}
