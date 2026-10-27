#include "MapSettingsModule.h"
#include "MapPlayerEnumDelegate.h"
#include "MapTechPage.h"
#include "../NavigationBookmarkDetail.h"

#include "EditorProject.h"
#include "Widgets/WarcraftTextPreview.h"
#include "Modules/ModuleHost.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapDocument.h"

#include <QCheckBox>
#include <QByteArray>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

namespace War3Map::Modules {
namespace {

QString Number(float value) { return QLocale::c().toString(value, 'g', 9); }
QString Hex(std::uint32_t value) { return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0')); }

struct MapFlagDefinition {
    const char* raw;
    const char* display;
};

// 经典 W3I v25 位序。位 13–16 交叉核对格式规范与 War3Net 的格式枚举：
// https://github.com/ChiefOfGxBxL/WC3MapSpecification/blob/master/Info/0-33.md
// https://github.com/Drake53/War3Net/blob/master/src/War3Net.Build.Core/Info/MapFlags.cs
// 更高版本选项不应用到 v25，保留原始位。
constexpr std::array<MapFlagDefinition, 17> mapFlagDefinitions{{
    {"disable_preview", "关闭预览图"},
    {"custom_ally", "自定义结盟优先权"},
    {"melee_map", "对战地图"},
    {"large_map", "大型地图"},
    {"masked_area_show_terrain", "迷雾区域显示地形"},
    {"fix_force_setting", "自定义玩家分组"},
    {"custom_force", "自定义队伍"},
    {"custom_techtree", "自定义科技树"},
    {"custom_ability", "自定义技能"},
    {"custom_upgrade", "自定义升级"},
    {"map_menu_mark", "地图菜单标记"},
    {"show_wave_on_cliff", "地形悬崖显示水波"},
    {"show_wave_on_rolling", "地形起伏显示水波"},
    {"use_terrain_fog", "使用地形迷雾"},
    {"requires_expansion", "需要冰封王座资料片"},
    {"use_item_classification", "使用物品分类系统"},
    {"use_water_tint", "使用自定义水面颜色"},
}};

constexpr std::uint32_t knownMapFlagMask = (1U << mapFlagDefinitions.size()) - 1U;

// 地图格式不声明文本编码。往返不一致的旧字节不进入可编辑文本控件，
// 避免打开页面再应用时把未知编码的作者字段改坏。
bool Decode(std::string_view bytes, QString& output, bool& local) {
    const QByteArray original(bytes.data(), static_cast<qsizetype>(bytes.size()));
    output = QString::fromUtf8(original);
    local = false;
    if (output.toUtf8() == original) return true;
    output = QString::fromLocal8Bit(original);
    local = true;
    return output.toLocal8Bit() == original;
}

bool Encode(const QString& value, bool local, std::string& output) {
    const QByteArray bytes = local ? value.toLocal8Bit() : value.toUtf8();
    if ((local ? QString::fromLocal8Bit(bytes) : QString::fromUtf8(bytes)) != value) return false;
    output.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    return output.find('\0') == std::string::npos;
}

QString DiagnosticText(const Status& status) {
    if (status.diagnostics.empty()) return QObject::tr("地图设置写入失败");
    return QString::fromUtf8(status.diagnostics.front().message.c_str());
}

QTableWidgetItem* Cell(QTableWidget* table, int row, int column, const QString& value, bool editable = true) {
    auto* item = new QTableWidgetItem(value);
    if (!editable) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    table->setItem(row, column, item);
    return item;
}

bool ReadInt(QTableWidget* table, int row, int column, std::int32_t& output) {
    const auto* item = table->item(row, column);
    bool okay = false;
    const int value = item ? item->text().trimmed().toInt(&okay, 0) : 0;
    if (okay) output = value;
    return okay;
}

bool ReadMask(const QString& text, std::uint32_t& output) {
    bool okay = false;
    const auto value = text.trimmed().toUInt(&okay, 0);
    if (okay) output = value;
    return okay;
}

bool ReadMask(QTableWidget* table, int row, int column, std::uint32_t& output) {
    const auto* item = table->item(row, column);
    return item && ReadMask(item->text(), output);
}

bool ReadFloat(QTableWidget* table, int row, int column, float& output) {
    const auto* item = table->item(row, column);
    bool okay = false;
    const float value = item ? QLocale::c().toFloat(item->text().trimmed(), &okay) : 0.0f;
    if (okay && std::isfinite(value)) output = value;
    return okay && std::isfinite(value);
}

// 事务回执同时保留 Core 联动修改的成员；源图原有不一致也能精确撤销。
class SettingsUndoCommand final : public QUndoCommand {
public:
    SettingsUndoCommand(QtProject::EditorProject& project, MapData::MapDataEditSession& session,
        MapData::MapDataEditSession::EditReceipt receipt, std::function<void()> changed)
        : project_(&project), session_(&session), receipt_(std::move(receipt)),
          changed_(std::move(changed)) {
        setText(QObject::tr("修改地图设置"));
    }

    void undo() override { Apply(false); }
    void redo() override {
        if (firstRedo_) {
            firstRedo_ = false; // 入栈前已通过 EditSettings 校验并应用。
            if (project_) (void)project_->ObserveEditState("修改地图设置");
            if (changed_) changed_();
            return;
        }
        Apply(true);
    }

private:
    void Apply(bool forward) {
        if (!project_ || project_->MapEdits() != session_) return;
        const auto status = session_->Replay(receipt_, forward);
        if (!status.Succeeded()) {
            project_->AddDiagnostics(status.diagnostics);
            return;
        }
        (void)project_->ObserveEditState("修改地图设置");
        if (changed_) changed_();
    }

    QPointer<QtProject::EditorProject> project_;
    MapData::MapDataEditSession* session_ = nullptr;
    MapData::MapDataEditSession::EditReceipt receipt_;
    std::function<void()> changed_;
    bool firstRedo_ = true;
};

} // namespace

MapSettingsModule::MapSettingsModule(QtProject::EditorProject& project) : project_(project) {
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, &MapSettingsModule::Refresh);
    if (project_.mapDataUndoStack_)
        connect(project_.mapDataUndoStack_, &QUndoStack::indexChanged, this, &MapSettingsModule::Refresh);
}

MapSettingsModule::~MapSettingsModule() {
    rebuilding_ = true;
    delete page_.data();
}

Shell::ModuleInfo MapSettingsModule::Info() const {
    // 入口位于顶栏“地图”菜单，不占顶栏九个常驻模块快捷键。
    auto info = Shell::ModuleInfo{"map-settings", tr("地图设置"), "lucide/map", {}};
    info.showInSwitcher = false;
    return info;
}

QWidget* MapSettingsModule::Widget() {
    if (!page_) BuildPage();
    return page_;
}

void MapSettingsModule::AttachHost(Shell::ModuleHost* host) { host_ = host; }

QUndoStack* MapSettingsModule::UndoStack() { return project_.mapDataUndoStack_; }

bool MapSettingsModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "map-settings") return false;
    (void)Widget();
    const QString section = target.kind.isEmpty() ? target.id : target.kind;
    if (section.isEmpty()) return true;
    const QStringList sections{"general", "players", "forces", "loading", "tech"};
    const int index = sections.indexOf(section);
    if (index < 0) return false;
    sections_->setCurrentRow(index);
    return true;
}

QJsonObject MapSettingsModule::SaveViewState() const {
    return {{"section", sections_ ? sections_->currentRow() : pendingSection_},
        {"rawFields", rawFields_ ? rawFields_->isChecked() : pendingRawFields_}};
}

void MapSettingsModule::RestoreViewState(const QJsonObject& state) {
    pendingSection_ = std::clamp(state.value("section").toInt(), 0, 4);
    pendingRawFields_ = state.value("rawFields").toBool();
    if (rawFields_) rawFields_->setChecked(pendingRawFields_);
    if (sections_) sections_->setCurrentRow(pendingSection_);
}

std::optional<QJsonObject> MapSettingsModule::CaptureNavigationBookmark() const {
    if (!page_ || !sections_ || !pages_ || pending_) return {};
    constexpr std::array<const char*, 5> keys{"general", "players", "forces", "loading", "tech"};
    const int section = sections_->currentRow();
    if (section < 0 || section >= static_cast<int>(keys.size()) || section >= pages_->count()) return {};
    const auto* scroll = qobject_cast<QScrollArea*>(pages_->widget(section));
    if (!scroll) return {};
    return QJsonObject{{"schema", 1}, {"kind", "map-settings-view"}, {"section", QString::fromLatin1(keys[static_cast<std::size_t>(section)])},
        {"scroll_v", scroll->verticalScrollBar()->value()}, {"scroll_h", scroll->horizontalScrollBar()->value()}};
}

bool MapSettingsModule::PrepareNavigationLeave() {
    if (!pending_) return true;
    SetMessage(tr("先应用或放弃当前地图设置修改，再跳转"), true);
    return false;
}

Shell::NavigationRestoreResult MapSettingsModule::RestoreNavigationBookmark(const QJsonObject& bookmark) {
    using Shell::NavigationRestoreResult;
    namespace Nav = NavigationBookmarkDetail;
    if (pending_) { (void)PrepareNavigationLeave(); return NavigationRestoreResult::Blocked; }
    const auto section = Nav::Text(bookmark, "section", 32);
    const auto vertical = Nav::Scroll(bookmark, "scroll_v"), horizontal = Nav::Scroll(bookmark, "scroll_h");
    const QStringList keys{"general", "players", "forces", "loading", "tech"};
    if (!Nav::Header(bookmark, "map-settings-view") || !section || !vertical || !horizontal || !page_ || !pages_ || !sections_)
        return NavigationRestoreResult::Invalid;
    const int index = keys.indexOf(*section);
    if (index < 0 || index >= pages_->count() || index >= sections_->count()) return NavigationRestoreResult::Invalid;
    auto* scroll = qobject_cast<QScrollArea*>(pages_->widget(index));
    if (!scroll) return NavigationRestoreResult::Invalid;
    // 只恢复分组及其滚动位置；不 Refresh、读入书签草稿或应用地图设置。
    sections_->setCurrentRow(index);
    scroll->verticalScrollBar()->setValue(*vertical); scroll->horizontalScrollBar()->setValue(*horizontal);
    return NavigationRestoreResult::Restored;
}

void MapSettingsModule::BuildPage() {
    page_ = new QWidget;
    page_->setObjectName("mapSettingsWorkspace");
    page_->installEventFilter(this);
    auto* outer = new QVBoxLayout(page_);
    outer->setContentsMargins(12, 10, 12, 10);
    outer->setSpacing(8);

    auto* top = new QHBoxLayout;
    title_ = new QLabel(tr("地图设置"), page_);
    title_->setObjectName("paneTitle");
    top->addWidget(title_);
    compactSections_ = new QComboBox(page_);
    compactSections_->setObjectName("mapSettingsCompactSections");
    compactSections_->setAccessibleName(tr("设置分组"));
    compactSections_->setToolTip(tr("切换设置分组；未应用的修改会保留"));
    compactSections_->setVisible(false);
    top->addWidget(compactSections_);
    top->addStretch();
    rawFields_ = new QCheckBox(tr("显示原始字段"), page_);
    rawFields_->setObjectName("mapSettingsRawFields");
    rawFields_->setChecked(pendingRawFields_);
    top->addWidget(rawFields_);
    outer->addLayout(top);

    auto* body = new QHBoxLayout;
    sections_ = new QListWidget(page_);
    sections_->setObjectName("mapSettingsSections");
    sections_->setFixedWidth(150);
    body->addWidget(sections_);
    pages_ = new QStackedWidget(page_);
    pages_->setObjectName("mapSettingsPages");
    body->addWidget(pages_, 1);
    outer->addLayout(body, 1);

    auto* footer = new QHBoxLayout;
    message_ = new QLabel(page_);
    message_->setObjectName("mapSettingsMessage");
    message_->setTextFormat(Qt::PlainText);
    message_->setWordWrap(true);
    message_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    footer->addWidget(message_, 1);
    discard_ = new QPushButton(tr("取消修改"), page_);
    discard_->setObjectName("mapSettingsDiscard");
    apply_ = new QPushButton(tr("应用"), page_);
    apply_->setObjectName("mapSettingsApply");
    footer->addWidget(discard_);
    footer->addWidget(apply_);
    outer->addLayout(footer);

    connect(sections_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < pages_->count()) pages_->setCurrentIndex(row);
        const QSignalBlocker blocker(compactSections_);
        compactSections_->setCurrentIndex(row);
        if (!rebuilding_) pendingSection_ = row;
    });
    connect(compactSections_, qOverload<int>(&QComboBox::currentIndexChanged),
        sections_, qOverload<int>(&QListWidget::setCurrentRow));
    connect(rawFields_, &QCheckBox::toggled, this, [this](bool enabled) {
        pendingRawFields_ = enabled;
        UpdateLabels();
    });
    connect(discard_, &QPushButton::clicked, this, &MapSettingsModule::DiscardDraft);
    connect(apply_, &QPushButton::clicked, this, [this] { (void)ApplyDraft(); });
    Refresh();
    UpdateResponsiveLayout();
}

void MapSettingsModule::SetMessage(const QString& message, bool error) {
    if (!message_) return;
    message_->setText(message);
    message_->setProperty("error", error);
    message_->style()->unpolish(message_);
    message_->style()->polish(message_);
    if (error && host_) host_->ShowStatusMessage(message, 8000);
}

void MapSettingsModule::MarkPending() {
    if (rebuilding_ || !draft_ || draft_->formatVersion != 25) return;
    const bool changed = !pending_;
    pending_ = true;
    if (changed) emit DraftChanged(true);
    apply_->setEnabled(draft_.has_value() && project_.MapEdits());
    discard_->setEnabled(draft_.has_value());
    if (!stale_) SetMessage(tr("有未应用的修改"));
}

void MapSettingsModule::AddTextField(QFormLayout* form, const QString& label, const QString& raw,
    std::string& value, bool multiline) {
    QString visible;
    bool local = false;
    const bool decoded = Decode(value, visible, local);
    const bool sharedSetting = raw == "author" || raw == "description" || raw == "recommendedPlayers";
    // 显示当前会话正文，三项设置引用的编辑目标是唯一 WTS；地图名保持既有字面量规则。
    QString reference;
    QString stringSourceSha;
    bool previewBodySafe = true;
    if (decoded && value.starts_with("TRIGSTR_") && project_.mapDocument_) {
        const auto current = project_.MapEdits() ? project_.MapEdits()->Snapshot() : *project_.mapDocument_;
        const auto resolved = current.ResolveString(value);
        if (resolved.Succeeded() && resolved.value) {
            reference = visible; visible = QString::fromUtf8(resolved.value->c_str());
            previewBodySafe = visible.toUtf8().toStdString() == *resolved.value;
        }
        if (sharedSetting && current.Member(MapDataKind::Strings).state == MapDataState::Ready
            && current.Member(MapDataKind::Strings).sources.size() == 1) {
            const auto hash = Sha256(current.Member(MapDataKind::Strings).sources.front().data);
            if (hash.Succeeded() && hash.value) stringSourceSha = QString::fromStdString(*hash.value);
        }
    }
    auto* caption = new QLabel(form->parentWidget());
    caption->setObjectName(QStringLiteral("mapSettingsLabel_%1").arg(raw));
    labels_.push_back({caption, label, raw});
    QWidget* editor = nullptr;
    if (multiline) {
        auto* box = new QPlainTextEdit(form->parentWidget());
        box->setPlainText(decoded ? visible : tr("[无法安全解码的文本]"));
        box->setMinimumHeight(78);
        box->setMaximumHeight(130);
        box->setReadOnly(!decoded);
        if (decoded) connect(box, &QPlainTextEdit::textChanged, this, &MapSettingsModule::MarkPending);
        editor = box;
    } else {
        auto* box = new QLineEdit(decoded ? visible : tr("[无法安全解码的文本]"), form->parentWidget());
        box->setMaxLength(1024 * 1024);
        box->setReadOnly(!decoded);
        if (decoded) connect(box, &QLineEdit::textChanged, this, &MapSettingsModule::MarkPending);
        editor = box;
    }
    editor->setObjectName(QStringLiteral("mapSettings_%1").arg(raw));
    caption->setBuddy(editor);
    if (!decoded) editor->setToolTip(tr("原文件的文本编码无法往返确认，已锁定此字段以保护原始字节"));
    else if (!reference.isEmpty()) editor->setToolTip(sharedSetting
        ? tr("来自字符串表 %1；修改时保留 W3I 原引用并改写对应 WTS 正文，同编号的其它地图引用也会读取新文字").arg(reference)
        : tr("来自字符串表 %1；修改后改为直接写入的文字").arg(reference));
    else if (value.starts_with("TRIGSTR_")) editor->setToolTip(tr("字符串表引用 %1 未找到对应文字").arg(visible));
    // 正文预览只消费已存在的 draft，不重新 Snapshot/WTS hash，也不改变 TextField 的提交控件。
    if (!raw.endsWith(QStringLiteral(".path"))) {
        auto* field = new QWidget(form->parentWidget());
        auto* layout = new QVBoxLayout(field);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(editor);
        auto* preview = new UiKit::WarcraftTextPreview(field);
        const bool sourceAvailable = decoded && previewBodySafe && (!value.starts_with("TRIGSTR_") || !reference.isEmpty());
        const QString sourceText = reference.isEmpty() ? QString() : tr("来自 %1").arg(reference);
        if (auto* box = qobject_cast<QPlainTextEdit*>(editor)) preview->Bind(box, sourceText, sourceAvailable);
        else if (auto* box = qobject_cast<QLineEdit*>(editor)) preview->Bind(box, sourceText, sourceAvailable);
        layout->addWidget(preview);
        form->addRow(caption, field);
    } else form->addRow(caption, editor);
    if (sharedSetting && value.starts_with("TRIGSTR_")) editor->setProperty("mapSettingsStringSourceSha", stringSourceSha);
    textFields_.push_back({editor, &value, value, decoded ? (local ? Encoding::Local8Bit : Encoding::Utf8) : Encoding::Opaque, visible});
}

void MapSettingsModule::AddNumberField(QFormLayout* form, const QString& label, const QString& raw,
    int value, QSpinBox*& output) {
    auto* caption = new QLabel(form->parentWidget());
    caption->setObjectName(QStringLiteral("mapSettingsLabel_%1").arg(raw));
    labels_.push_back({caption, label, raw});
    output = new QSpinBox(form->parentWidget());
    output->setObjectName(QStringLiteral("mapSettings_%1").arg(raw));
    output->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    output->setValue(value);
    caption->setBuddy(output);
    connect(output, qOverload<int>(&QSpinBox::valueChanged), this, &MapSettingsModule::MarkPending);
    form->addRow(caption, output);
}

void MapSettingsModule::BuildForms() {
    rebuilding_ = true;
    const QSignalBlocker compactBlocker(compactSections_);
    textFields_.clear();
    labels_.clear();
    forms_.clear();
    flagsLayout_ = nullptr;
    flagColumns_ = 0;
    sections_->clear();
    compactSections_->clear();
    while (pages_->count()) {
        QWidget* widget = pages_->widget(0);
        pages_->removeWidget(widget);
        delete widget;
    }
    players_ = nullptr;
    forces_ = nullptr;
    techPage_ = nullptr;
    mapFlagFields_.clear();
    loadingBackground_ = nullptr;
    gameDataSet_ = nullptr;

    const auto addForm = [this](const QString& name) {
        sections_->addItem(name);
        compactSections_->addItem(name);
        auto* scroll = new QScrollArea(pages_);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->viewport()->setObjectName("mapSettingsFormViewport");
        scroll->viewport()->installEventFilter(this);
        auto* content = new QWidget(scroll);
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(8, 3, 8, 6);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(9);
        layout->addLayout(form);
        forms_.push_back(form);
        layout->addStretch();
        scroll->setWidget(content);
        pages_->addWidget(scroll);
        return form;
    };

    if (!draft_) {
        addForm(tr("地图描述"));
        sections_->setCurrentRow(0);
        apply_->setEnabled(false);
        discard_->setEnabled(false);
        SetMessage(project_.mapDocument_ && project_.mapDocument_->LniSettings()
            ? tr("LNI 地图设置目前仅支持读取") : tr("打开地图后显示设置"));
        rebuilding_ = false;
        UpdateResponsiveLayout();
        return;
    }

    if (draft_->formatVersion != 25) {
        auto* form = addForm(tr("地图描述"));
        QString mapName;
        bool local = false;
        if (!Decode(draft_->mapName, mapName, local)) mapName = tr("[无法安全解码的名称]");
        form->addRow(tr("名称"), new QLabel(mapName, form->parentWidget()));
        form->addRow(tr("格式"), new QLabel(tr("W3I v%1").arg(draft_->formatVersion), form->parentWidget()));
        if (draft_->Layout() && draft_->Layout()->gameVersion)
            form->addRow(tr("游戏版本"), new QLabel(tr("%1.%2.%3.%4")
                .arg(draft_->gameVersion[0]).arg(draft_->gameVersion[1])
                .arg(draft_->gameVersion[2]).arg(draft_->gameVersion[3]), form->parentWidget()));
        if (draft_->Layout() && draft_->Layout()->scriptLanguage)
            form->addRow(tr("脚本语言原值"), new QLabel(QString::number(draft_->scriptLanguage), form->parentWidget()));
        sections_->setCurrentRow(0);
        apply_->setEnabled(false);
        discard_->setEnabled(false);
        SetMessage(tr("W3I v%1 · 只读").arg(draft_->formatVersion));
        rebuilding_ = false;
        UpdateResponsiveLayout();
        return;
    }

    auto* general = addForm(tr("地图描述"));
    AddTextField(general, tr("名称"), "mapName", draft_->mapName);
    AddTextField(general, tr("作者"), "author", draft_->author);
    AddTextField(general, tr("说明"), "description", draft_->description, true);
    AddTextField(general, tr("推荐玩家"), "recommendedPlayers", draft_->recommendedPlayers);
    auto* flagsLabel = new QLabel(page_);
    flagsLabel->setObjectName("mapSettingsLabel_flags");
    labels_.push_back({flagsLabel, tr("地图选项"), "flags"});
    auto* flagGroup = new QWidget(general->parentWidget());
    flagGroup->setObjectName("mapSettings_flags");
    auto* flagLayout = new QGridLayout(flagGroup);
    flagsLayout_ = flagLayout;
    flagColumns_ = 2;
    flagLayout->setContentsMargins(0, 0, 0, 0);
    flagLayout->setHorizontalSpacing(20);
    flagLayout->setVerticalSpacing(6);
    for (std::size_t index = 0; index < mapFlagDefinitions.size(); ++index) {
        const auto mask = std::uint32_t{1} << index;
        const auto& definition = mapFlagDefinitions[index];
        const QString display = QString::fromUtf8(definition.display);
        const QString raw = QString::fromLatin1(definition.raw);
        auto* check = new QCheckBox(display, flagGroup);
        check->setObjectName(QStringLiteral("mapSettingsFlag_%1").arg(raw));
        check->setChecked((draft_->flags & mask) != 0);
        connect(check, &QCheckBox::toggled, this, &MapSettingsModule::MarkPending);
        flagLayout->addWidget(check, static_cast<int>(index / 2), static_cast<int>(index % 2));
        mapFlagFields_.push_back({check, mask, display, raw});
    }
    general->addRow(flagsLabel, flagGroup);
    const auto unknownFlags = draft_->flags & ~knownMapFlagMask;
    if (unknownFlags != 0) {
        auto* unknown = new QLabel(tr("未识别的选项位 %1（原样保留）")
            .arg(Hex(unknownFlags)), general->parentWidget());
        unknown->setObjectName("mapSettingsUnknownFlags");
        unknown->setWordWrap(true);
        unknown->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        unknown->setToolTip(unknown->text());
        // 独占整行，避免嵌套网格把换行文字压进复选框的固定行高。
        general->addRow(unknown);
    }

    auto* dataSetLabel = new QLabel(general->parentWidget());
    dataSetLabel->setObjectName("mapSettingsLabel_gameDataSet");
    labels_.push_back({dataSetLabel, tr("游戏数据集"), "gameDataSet"});
    gameDataSet_ = new QComboBox(general->parentWidget());
    gameDataSet_->setObjectName("mapSettings_gameDataSet");
    dataSetLabel->setBuddy(gameDataSet_);
    gameDataSet_->addItem(tr("默认（按地图类型）"), draft_->gameDataSet == -1 ? -1 : 0);
    gameDataSet_->addItem(tr("自定义"), 1);
    gameDataSet_->addItem(tr("对战（最新版本）"), 2);
    if (draft_->gameDataSet >= 0 && draft_->gameDataSet <= 2) {
        gameDataSet_->setCurrentIndex(draft_->gameDataSet);
    } else if (draft_->gameDataSet != -1) {
        gameDataSet_->insertItem(0, tr("未知设置（保留原值 %1）").arg(draft_->gameDataSet), draft_->gameDataSet);
        gameDataSet_->setCurrentIndex(0);
    }
    connect(gameDataSet_, qOverload<int>(&QComboBox::currentIndexChanged), this, &MapSettingsModule::MarkPending);
    general->addRow(dataSetLabel, gameDataSet_);

    auto* playersForm = addForm(tr("玩家"));
    players_ = new QTableWidget(static_cast<int>(draft_->players.size()), 9, page_);
    players_->setObjectName("mapSettingsPlayers");
    players_->horizontalHeader()->setStretchLastSection(true);
    players_->setAlternatingRowColors(true);
    players_->setMinimumHeight(250);
    const auto rawFields = [this] { return rawFields_ && rawFields_->isChecked(); };
    players_->setItemDelegateForColumn(2,
        new MapPlayerEnumDelegate(MapPlayerEnumDelegate::Kind::Controller, rawFields, players_));
    players_->setItemDelegateForColumn(3,
        new MapPlayerEnumDelegate(MapPlayerEnumDelegate::Kind::Race, rawFields, players_));
    for (int row = 0; row < players_->rowCount(); ++row) {
        const auto& player = draft_->players[static_cast<std::size_t>(row)];
        QString name;
        bool local = false;
        const bool decoded = Decode(player.name, name, local);
        Cell(players_, row, 0, QString::number(player.number), false);
        auto* item = Cell(players_, row, 1, decoded ? name : tr("[无法解码]"), decoded);
        if (!decoded) item->setToolTip(tr("名称编码无法往返确认"));
        Cell(players_, row, 2, QString::number(player.controller));
        Cell(players_, row, 3, QString::number(player.race));
        Cell(players_, row, 4, Hex(player.flags));
        Cell(players_, row, 5, Number(player.startX));
        Cell(players_, row, 6, Number(player.startY));
        Cell(players_, row, 7, Hex(player.allyLowPriorityMask));
        Cell(players_, row, 8, Hex(player.allyHighPriorityMask));
    }
    playersForm->addRow(players_);
    connect(players_, &QTableWidget::itemChanged, this, &MapSettingsModule::MarkPending);

    auto* forcesForm = addForm(tr("势力"));
    forces_ = new QTableWidget(static_cast<int>(draft_->forces.size()), 3, page_);
    forces_->setObjectName("mapSettingsForces");
    forces_->horizontalHeader()->setStretchLastSection(true);
    forces_->setAlternatingRowColors(true);
    forces_->setMinimumHeight(210);
    for (int row = 0; row < forces_->rowCount(); ++row) {
        const auto& force = draft_->forces[static_cast<std::size_t>(row)];
        QString name;
        bool local = false;
        const bool decoded = Decode(force.name, name, local);
        auto* item = Cell(forces_, row, 0, decoded ? name : tr("[无法解码]"), decoded);
        if (!decoded) item->setToolTip(tr("名称编码无法往返确认"));
        Cell(forces_, row, 1, Hex(force.playerMask));
        Cell(forces_, row, 2, Hex(force.flags));
    }
    forcesForm->addRow(forces_);
    connect(forces_, &QTableWidget::itemChanged, this, &MapSettingsModule::MarkPending);

    auto* screen = addForm(tr("载入画面"));
    AddNumberField(screen, tr("背景编号"), "loadingBackground", draft_->loadingBackground, loadingBackground_);
    AddTextField(screen, tr("模型路径"), "loadingScreen.path", draft_->loadingScreen.path);
    AddTextField(screen, tr("标题"), "loadingScreen.title", draft_->loadingScreen.title);
    AddTextField(screen, tr("副标题"), "loadingScreen.subtitle", draft_->loadingScreen.subtitle);
    AddTextField(screen, tr("正文"), "loadingScreen.text", draft_->loadingScreen.text, true);
    AddTextField(screen, tr("序章路径"), "prologueScreen.path", draft_->prologueScreen.path);
    AddTextField(screen, tr("序章标题"), "prologueScreen.title", draft_->prologueScreen.title);
    AddTextField(screen, tr("序章副标题"), "prologueScreen.subtitle", draft_->prologueScreen.subtitle);
    AddTextField(screen, tr("序章正文"), "prologueScreen.text", draft_->prologueScreen.text, true);

    auto* tech = addForm(tr("科技与随机表"));
    if (draft_->custom) {
        // 科技行经共用自定义设置服务逐条提交（各一个撤销记录），与上方草稿的“应用”分开；草稿未应用时不接受。
        QString readOnly;
        const auto* session = project_.MapEdits();
        const auto allowed = session ? Services::MapCustomSettingsEditing::WriteQualification(session->Snapshot())
            : Status::Failure("Map.CustomSettings", "SETTINGS_DOCUMENT_REQUIRED", tr("当前没有打开地图。").toStdString());
        if (!allowed.Succeeded()) readOnly = QString::fromStdString(allowed.diagnostics.front().message);
        else if (!draft_->custom->randomUnits.empty() || !draft_->custom->randomItems.empty())
            readOnly = tr("地图含随机单位/物品表，其初始化尚未接通，科技设置暂只能查看。");
        techPage_ = new MapTechPage(project_, *draft_, rawFields_ && rawFields_->isChecked(), readOnly,
            MapTechPage::ProjectCommit(project_, this,
                [this] { return pending_ ? tr("先应用或放弃上方未应用的地图设置修改。") : QString{}; },
                [this](const QString& problem) { SetMessage(problem, true); }),
            tech->parentWidget());
        tech->addRow(techPage_);
    } else {
        auto* techStatus = new QLabel(tr("原地图未提供自定义表 · 只读"), page_);
        techStatus->setWordWrap(true);
        tech->addRow(techStatus);
    }

    UpdateLabels();
    sections_->setCurrentRow(std::clamp(pendingSection_, 0, pages_->count() - 1));
    apply_->setEnabled(false);
    discard_->setEnabled(false);
    SetMessage(draft_->FullyParsed() ? tr("经典地图设置 · W3I v25")
        : tr("W3I v25 · 尾部原样保留"));
    rebuilding_ = false;
    UpdateResponsiveLayout();
}

void MapSettingsModule::UpdateLabels() {
    const bool raw = rawFields_ && rawFields_->isChecked();
    for (const auto& field : labels_) field.widget->setText(raw ? field.raw : field.display);
    for (const auto& field : mapFlagFields_) field.widget->setText(raw ? field.raw : field.display);
    if (players_) players_->setHorizontalHeaderLabels(raw
        ? QStringList{"number", "name", "controller", "race", "flags", "startX", "startY", "allyLowPriorityMask", "allyHighPriorityMask"}
        : QStringList{tr("序号"), tr("名称"), tr("控制类型"), tr("种族"), tr("选项"), tr("起点 X"), tr("起点 Y"), tr("低优先盟友"), tr("高优先盟友")});
    if (players_) players_->viewport()->update();  // 控制类型/种族列随原始字段开关切换数字与名称
    if (techPage_) techPage_->SetRaw(raw);
    if (forces_) forces_->setHorizontalHeaderLabels(raw
        ? QStringList{"name", "playerMask", "flags"}
        : QStringList{tr("名称"), tr("玩家集合"), tr("联盟选项")});
    UpdateResponsiveLayout();
}

void MapSettingsModule::Refresh() {
    if (!page_) return;
    auto* session = project_.MapEdits();
    if (pending_ && session && session->SessionIdentity() == boundSession_) {
        CheckDraftSource();
        return;
    }
    Reload();
}

void MapSettingsModule::CheckDraftSource() {
    // 草稿基线与当前 W3I 逐字节比较；字符串表变化仍由 ApplyDraft 按被编辑字段核对。
    const auto current = project_.MapEdits()->Snapshot(MapDataKind::Settings);
    const auto currentBytes = current.Succeeded() && current.value && current.value->Settings()
        ? current.value->Settings()->Serialize() : Result<std::vector<std::byte>>{};
    const auto draftBytes = draft_ ? draft_->Serialize() : Result<std::vector<std::byte>>{};
    const bool stale = !currentBytes.Succeeded() || !currentBytes.value || !draftBytes.Succeeded()
        || !draftBytes.value || *currentBytes.value != *draftBytes.value;
    if (stale == stale_) return;
    stale_ = stale;
    SetMessage(stale ? tr("地图设置已被其他操作修改，请放弃后重新编辑")
                     : tr("有未应用的修改"), stale);
}

void MapSettingsModule::EndDraft() {
    if (!pending_) return;
    pending_ = false;
    stale_ = false;
    emit DraftChanged(false);
}

void MapSettingsModule::Reload() {
    if (!page_) return;
    const bool hadDraft = pending_;
    auto* session = project_.MapEdits();
    boundSession_ = session ? session->SessionIdentity() : 0;
    stale_ = false;
    const MapDocument snapshot = session ? session->Snapshot()
        : (project_.mapDocument_ ? *project_.mapDocument_
            : MapDocument::FromMembers(std::span<const MapArchiveMember>{}));
    draft_ = snapshot.Settings();
    pending_ = false;
    BuildForms();
    if (hadDraft) emit DraftChanged(false);
    if (host_) host_->UndoStackChanged("map-settings");
}

void MapSettingsModule::DiscardDraft() {
    if (!page_) return;
    Reload();
}

bool MapSettingsModule::ReadDraft(MapData::MapSettings& candidate, QString& error) {
    if (!draft_) return false;
    // 文本原始字节仅在控件发生实际变化时编码；其余字段保持文件字节。
    std::vector<std::string> encoded;
    encoded.reserve(textFields_.size());
    for (const auto& field : textFields_) {
        const QString text = qobject_cast<QLineEdit*>(field.editor)
            ? static_cast<QLineEdit*>(field.editor)->text()
            : static_cast<QPlainTextEdit*>(field.editor)->toPlainText();
        if (field.encoding == Encoding::Opaque || text == field.initial) {
            encoded.push_back(field.original);
            continue;
        }
        std::string bytes;
        if (!Encode(text, field.encoding == Encoding::Local8Bit, bytes)) {
            error = tr("字段文本无法用原文件编码无损写回，或包含空字节");
            return false;
        }
        // 旧本地编码沿用原有无损写回路径；UTF-8 地图名与 Agent 使用同一字段约束。
        if (field.target == &draft_->mapName && field.encoding == Encoding::Utf8) {
            MapData::MapFieldValue value;
            value.type = MapData::MapFieldType::Bytes;
            value.bytes = bytes;
            const auto checked = MapData::ValidateMapFieldValue(MapDataKind::Settings, "map_name", value);
            if (!checked.Succeeded()) {
                error = DiagnosticText(checked);
                return false;
            }
        }
        encoded.push_back(std::move(bytes));
    }
    for (std::size_t i = 0; i < textFields_.size(); ++i) *textFields_[i].target = encoded[i];
    candidate = *draft_;
    for (const auto& field : textFields_) *field.target = field.original;

    candidate.flags = draft_->flags & ~knownMapFlagMask;
    if (mapFlagFields_.size() != mapFlagDefinitions.size()) {
        error = tr("地图选项未完整载入，已拒绝写入");
        return false;
    }
    for (const auto& field : mapFlagFields_) {
        if (!field.widget) {
            error = tr("地图选项未完整载入，已拒绝写入");
            return false;
        }
        if (field.widget->isChecked()) candidate.flags |= field.mask;
    }
    bool dataSetOkay = false;
    const int dataSet = gameDataSet_->currentData().toInt(&dataSetOkay);
    if (!dataSetOkay) {
        error = tr("游戏数据集选项无效，未修改地图");
        return false;
    }
    if (dataSet != draft_->gameDataSet) {
        MapData::MapFieldValue value;
        value.type = MapData::MapFieldType::Int32;
        value.integer = dataSet;
        const auto checked = MapData::ValidateMapFieldValue(MapDataKind::Settings, "game_data_set", value);
        if (!checked.Succeeded()) {
            error = DiagnosticText(checked);
            return false;
        }
    }
    candidate.gameDataSet = dataSet;
    candidate.loadingBackground = loadingBackground_->value();

    for (int row = 0; row < players_->rowCount(); ++row) {
        auto& value = candidate.players[static_cast<std::size_t>(row)];
        const auto& original = draft_->players[static_cast<std::size_t>(row)];
        QString name;
        bool local = false;
        const bool decoded = Decode(original.name, name, local);
        if (decoded && players_->item(row, 1)->text() != name
            && !Encode(players_->item(row, 1)->text(), local, value.name)) {
            error = tr("玩家 %1 的名称编码无法写回").arg(row + 1);
            return false;
        }
        if (!ReadInt(players_, row, 2, value.controller) || !ReadInt(players_, row, 3, value.race)
            || !ReadMask(players_, row, 4, value.flags) || !ReadFloat(players_, row, 5, value.startX)
            || !ReadFloat(players_, row, 6, value.startY)
            || !ReadMask(players_, row, 7, value.allyLowPriorityMask)
            || !ReadMask(players_, row, 8, value.allyHighPriorityMask)) {
            error = tr("玩家 %1 存在无效的数值或掩码").arg(row + 1);
            return false;
        }
    }
    for (int row = 0; row < forces_->rowCount(); ++row) {
        auto& value = candidate.forces[static_cast<std::size_t>(row)];
        const auto& original = draft_->forces[static_cast<std::size_t>(row)];
        QString name;
        bool local = false;
        const bool decoded = Decode(original.name, name, local);
        if (decoded && forces_->item(row, 0)->text() != name
            && !Encode(forces_->item(row, 0)->text(), local, value.name)) {
            error = tr("势力 %1 的名称编码无法写回").arg(row + 1);
            return false;
        }
        if (!ReadMask(forces_, row, 1, value.playerMask) || !ReadMask(forces_, row, 2, value.flags)) {
            error = tr("势力 %1 存在无效的玩家集合或选项").arg(row + 1);
            return false;
        }
    }
    return true;
}

bool MapSettingsModule::ApplyDraft() {
    if (!page_) (void)Widget();
    auto* session = project_.MapEdits();
    if (!session || !draft_) {
        SetMessage(tr("当前地图设置不可编辑"), true);
        return false;
    }
    if (draft_->formatVersion != 25) {
        SetMessage(tr("新版地图设置只读"), true);
        return false;
    }
    const auto snapshot = session->Snapshot();
    if (!snapshot.Settings()) {
        SetMessage(tr("地图设置来源不完整"), true);
        return false;
    }
    const auto previousBytes = draft_->Serialize();
    const auto currentBytes = snapshot.Settings()->Serialize();
    if (!previousBytes.Succeeded() || !currentBytes.Succeeded() || !previousBytes.value || !currentBytes.value
        || *previousBytes.value != *currentBytes.value) {
        SetMessage(tr("地图设置已变化，请重新载入后编辑"), true);
        return false;
    }
    MapData::MapSettings candidate;
    QString error;
    if (!ReadDraft(candidate, error)) {
        SetMessage(error, true);
        return false;
    }
    // W3I 引用字节没变也可能有外部 WTS 修改；绑定控件所展示的完整 WTS 来源，防止覆盖新正文。
    for (const auto& field : textFields_) {
        const std::string* requested = field.target == &draft_->author ? &candidate.author
            : field.target == &draft_->description ? &candidate.description
            : field.target == &draft_->recommendedPlayers ? &candidate.recommendedPlayers : nullptr;
        if (!requested || !field.original.starts_with("TRIGSTR_") || *requested == field.original) continue;
        const auto& strings = snapshot.Member(MapDataKind::Strings);
        const auto hash = strings.sources.size() == 1 ? Sha256(strings.sources.front().data) : Result<std::string>{};
        const auto expected = field.editor->property("mapSettingsStringSourceSha").toString();
        if (expected.isEmpty() || strings.state != MapDataState::Ready || !hash.Succeeded() || !hash.value
            || expected != QString::fromStdString(*hash.value)) {
            SetMessage(tr("字符串表已变化，请重新载入"), true);
            return false;
        }
    }
    const auto serialized = candidate.Serialize();
    if (!serialized.Succeeded() || !serialized.value) {
        const Status status{serialized.diagnostics};
        SetMessage(DiagnosticText(status), true);
        return false;
    }
    if (*serialized.value == *currentBytes.value) {
        Reload();
        return true;
    }
    const auto status = session->EditSettings(candidate);
    if (!status.Succeeded()) {
        SetMessage(DiagnosticText(status), true);
        project_.AddDiagnostics(status.diagnostics);
        return false;
    }
    if (!status.diagnostics.empty()) project_.AddDiagnostics(status.diagnostics);
    EndDraft();  // 自己的应用不是外部修改；随后的通知直接重新载入
    const auto notify = [self = QPointer<MapSettingsModule>(this)] {
        if (!self) return;
        self->Refresh();
        emit self->SettingsEdited();
    };
    if (auto* stack = UndoStack())
        stack->push(new SettingsUndoCommand(project_, *session, session->LastEdit(), notify));
    else {
        (void)project_.ObserveEditState("修改地图设置");
        notify();
    }
    return true;
}

} // namespace War3Map::Modules
