#include "TriggerModule.h"

#include "TriggerBlueprint.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "TriggerCodeChecking.h"

#include "War3MapCore/MapArchiveSession.h"

#include <QFile>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeView>
#include <QVBoxLayout>

// 地图脚本 war3map.j 的只读查看：与触发器逻辑区共用右侧区域，选中触发器即返回。
namespace War3Map::Modules {

void TriggerModule::BuildMapScriptView(QWidget* pane, QVBoxLayout* layout) {
    mapScriptTools_ = new QWidget(pane);
    auto* tools = new QHBoxLayout(mapScriptTools_); tools->setContentsMargins(0, 0, 0, 0);
    mapScriptRawBytes_ = new QCheckBox(tr("原字节"), mapScriptTools_); mapScriptRawBytes_->setObjectName("mapScriptRawBytes");
    mapScriptRawBytes_->setToolTip(tr("仅改变只读显示；不转换、不写回地图脚本字节。"));
    tools->addWidget(mapScriptRawBytes_); tools->addStretch(); mapScriptTools_->hide(); layout->addWidget(mapScriptTools_);
    mapScriptView_ = new UiKit::CodeView(pane);
    mapScriptView_->setObjectName("triggerMapScriptView");
    mapScriptView_->setFont(QFont(QStringLiteral("Consolas"), 11));
    mapScriptView_->SetSourceName(QStringLiteral("war3map.j"));
    mapScriptView_->SetDecoder(DecodeMapCodeForView);
    mapScriptView_->setToolTip(tr("地图脚本 war3map.j，只读查看；顶栏搜索在脚本内查找。"));
    mapScriptView_->hide();
    layout->addWidget(mapScriptView_, 1);
    mapScriptProblems_ = new UiKit::CodeProblemsView(pane); mapScriptProblems_->SetView(mapScriptView_); mapScriptProblems_->hide(); layout->addWidget(mapScriptProblems_);
    connect(mapScriptRawBytes_, &QCheckBox::toggled, this, [this](bool raw) {
        mapScriptView_->SetDecoder(raw ? UiKit::DecodeCodeBytes(ShowMapCodeRawBytes) : UiKit::DecodeCodeBytes(DecodeMapCodeForView));
        if (showingMapScript_) mapScriptView_->SetBytesAsync(mapScriptBytes_);
    });
    connect(mapScriptView_, &UiKit::CodeView::TextLoaded, this, [this]() {
        ShowStatus(tr("war3map.j 已载入：%1 行").arg(mapScriptView_->blockCount()), 5000);
    });
    connect(mapScriptButton_, &QPushButton::clicked, this, [this]() {
        if (showingMapScript_) {
            LeaveMapScript();
        } else {
            ShowMapScript();
        }
    });
}

std::optional<QByteArray> TriggerModule::ReadMapScriptBytes() const {
    if (project_.archiveSession_) {
        for (const auto* name : {"war3map.j", "Scripts/war3map.j"}) if (project_.archiveSession_->HasMember(name)) {
            const auto bytes = project_.archiveSession_->ReadMember(name);
            if (!bytes.Succeeded() || !bytes.value) return std::nullopt;
            return QByteArray(reinterpret_cast<const char*>(bytes.value->data()), static_cast<qsizetype>(bytes.value->size()));
        }
    }
    if (project_.projectRoot_.empty()) return std::nullopt;
    for (const auto* relative : {L"map/war3map.j", L"map/scripts/war3map.j"}) {
        QFile file(QString::fromStdWString((project_.projectRoot_ / relative).wstring()));
        if (file.open(QIODevice::ReadOnly)) return file.readAll();
    }
    return std::nullopt;
}

void TriggerModule::ShowMapScript(std::optional<QByteArray> prepared) {
    if (!page_) {
        return;
    }
    if (!CommitPendingEdits()) return;
    auto bytes = prepared ? std::move(prepared) : ReadMapScriptBytes();
    if (!bytes) {
        ShowStatus(tr("当前地图没有可读取的 war3map.j。"), 6000);
        return;
    }
    showingMapScript_ = true;
    mapScriptButton_->setText(tr("返回触发器"));
    logicTitle_->setText(tr("war3map.j（只读）"));
    logicHint_->setText(tr("%1 KB · 顶栏搜索在脚本内查找").arg(bytes->size() / 1024));
    propertiesBox_->hide();
    ecaView_->hide();
    blueprint_->hide();
    scriptView_->hide();
    codeTools_->hide(); codeProblems_->hide();
    codeActions_->hide();
    convertCodeButton_->hide(); recoverCodeButton_->hide();
    mapScriptView_->show();
    mapScriptTools_->show(); mapScriptProblems_->show(); mapScriptBytes_ = std::move(*bytes);
    mapScriptView_->SetBytesAsync(mapScriptBytes_);
}

void TriggerModule::LeaveMapScript() {
    if (!showingMapScript_) {
        return;
    }
    showingMapScript_ = false;
    mapScriptButton_->setText(tr("地图脚本"));
    logicTitle_->setText(tr("触发器逻辑"));
    logicHint_->setText(tr("双击 / Enter 编辑参数"));
    mapScriptView_->hide();
    mapScriptTools_->hide(); mapScriptProblems_->hide(); mapScriptBytes_.clear();
    // 大文档不常驻内存；下次进入时重新读取。
    mapScriptView_->SetText({});
    propertiesBox_->show();
    ShowSelectedTrigger();
}

} // namespace War3Map::Modules
