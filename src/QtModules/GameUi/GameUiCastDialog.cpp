#include "GameUiModule.h"

#include "Modules/ModuleHost.h"
#include "War3MapCore/GameUi/UiPresets.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>

#include <algorithm>
#include <initializer_list>

// “读条系统”窗口：选要绑定的读条条界面及其数值与状态，生成读条系统的触发器。技能登记在触发器编辑器里改。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// 按类型列出单值数值；可空的一项以“无”开头。按常用编号预选。
void FillValues(QComboBox* box, const UiDescription* description, UiValueType type, bool optional,
    std::initializer_list<const char*> preferred) {
    box->clear();
    if (optional) box->addItem(QObject::tr("无"), QString());
    if (!description) return;
    for (const auto& value : description->values) {
        if (value.type != type || value.size != 0) continue;
        box->addItem(value.label.empty() ? Text(value.id) : QStringLiteral("%1  %2").arg(Text(value.label), Text(value.id)), Text(value.id));
    }
    for (const char* id : preferred) {
        if (const int index = box->findData(QString::fromLatin1(id)); index >= 0) {
            box->setCurrentIndex(index);
            break;
        }
    }
}

void FillStates(QComboBox* box, const UiDescription* description, std::initializer_list<const char*> preferred) {
    box->clear();
    box->addItem(QObject::tr("无"), QString());
    if (!description) return;
    for (const auto& state : description->states) {
        box->addItem(state.label.empty() ? Text(state.id) : QStringLiteral("%1  %2").arg(Text(state.label), Text(state.id)), Text(state.id));
    }
    for (const char* id : preferred) {
        if (const int index = box->findData(QString::fromLatin1(id)); index >= 0) {
            box->setCurrentIndex(index);
            break;
        }
    }
}

} // namespace

void GameUiModule::ShowCastSystemDialog() {
    QDialog dialog(page_);
    dialog.setObjectName(QStringLiteral("gameUiCastDialog"));
    dialog.setWindowTitle(tr("读条系统"));
    auto* form = new QFormLayout(&dialog);
    const auto combo = [&](const char* name, const QString& label) {
        auto* box = new QComboBox(&dialog);
        box->setObjectName(QLatin1String(name));
        box->setMinimumWidth(220);
        form->addRow(label, box);
        return box;
    };
    auto* bar = combo("gameUiCastBar", tr("读条条"));
    auto* visible = combo("gameUiCastVisible", tr("显示"));
    auto* progress = combo("gameUiCastProgress", tr("进度"));
    auto* text = combo("gameUiCastText", tr("技能名"));
    auto* remaining = combo("gameUiCastRemaining", tr("剩余秒数"));
    auto* channel = combo("gameUiCastChannel", tr("引导状态"));
    auto* interrupted = combo("gameUiCastInterrupted", tr("打断状态"));

    bar->addItem(tr("不绑定"), QString());
    for (const auto& file : Descriptions()) {
        const auto& d = file.description;
        bar->addItem(d.label.empty() ? Text(d.id) : QStringLiteral("%1  %2").arg(Text(d.label), Text(d.id)), Text(d.id));
    }
    // 新建一份读条条模板（连同它的界面触发器）再绑定。
    const QString newBar = QStringLiteral("+castbar");
    bar->addItem(tr("新建读条条"), newBar);
    const auto refill = [&, this] {
        const UiDescription* description = Description(bar->currentData().toString());
        FillValues(visible, description, UiValueType::Boolean, false, {"shown", "visible", "show"});
        FillValues(progress, description, UiValueType::Real, false, {"progress", "ratio", "value"});
        FillValues(text, description, UiValueType::String, true, {"text", "label", "name", "spell"});
        FillValues(remaining, description, UiValueType::Real, true, {"remaining", "left", "time"});
        FillStates(channel, description, {"channel", "channeling"});
        FillStates(interrupted, description, {"interrupt", "interrupted"});
        for (QComboBox* box : {visible, progress, text, remaining, channel, interrupted}) box->setEnabled(description != nullptr);
    };
    connect(bar, &QComboBox::currentIndexChanged, &dialog, refill);
    // 默认绑定所选的界面描述。
    if (tree_ && selected_ < tree_->Nodes().size() && tree_->Nodes()[selected_].uiFrame) {
        const int index = bar->findData(Text(tree_->Nodes()[selected_].uiFrame->description));
        if (index >= 0) bar->setCurrentIndex(index);
    }
    refill();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("生成"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    CastSystemRequest request;
    request.bar = bar->currentData().toString();
    QString error;
    if (request.bar == newBar) {
        QString id = QStringLiteral("castbar");
        for (int n = 2; Description(id); ++n) id = QStringLiteral("castbar%1").arg(n);
        if (!CreateDescription(id, {}, &error, QStringLiteral("castbar")) || !GenerateDescriptionTriggers(id, false, &error)) {
            ReportAuthoring(error);
            return;
        }
        using namespace CastBarPreset;
        request.bar = id;
        request.visible = Text(std::string(kVisible));
        request.progress = Text(std::string(kProgress));
        request.text = Text(std::string(kText));
        request.remaining = Text(std::string(kRemaining));
        request.channel = Text(std::string(kChannel));
        request.interrupted = Text(std::string(kInterrupted));
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
    if (host_) {
        host_->ShowStatusMessage(tr("已生成 %1 个触发器到 %2 类别").arg(generated->triggers.size()).arg(Text(std::string(kCastTriggerCategory))), 8000);
    }
}

} // namespace War3Map::Modules
