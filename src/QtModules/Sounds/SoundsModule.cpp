#include "SoundsModule.h"
#include "SoundPreviewPlayer.h"
#include "../NavigationBookmarkDetail.h"

#include "EditorProject.h"
#include "ObjectNames.h"
#include "Commands/MapDataCommands.h"
#include "Modules/ModuleHost.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/MapDataFieldEditing.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <QAbstractItemModel>
#include <QAudioOutput>
#include <QByteArray>
#include <QBuffer>
#include <QCheckBox>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMediaPlayer>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace War3Map::Modules {
namespace {

constexpr std::array<std::string_view, 19> kFields{
    "name", "filePath", "eaxSetting", "fadeInRate", "fadeOutRate", "volume", "pitch",
    "pitchVariance", "priority", "channel", "minDistance", "maxDistance", "distanceCutoff",
    "coneAngleInside", "coneAngleOutside", "coneOutsideVolume",
    "coneOrientationX", "coneOrientationY", "coneOrientationZ"};

constexpr std::array<const char*, 19> kLabels{
    "定义名", "资源文件", "EAX 设置", "淡入速率", "淡出速率", "音量", "音高", "音高浮动",
    "优先级", "通道", "最近距离", "最远距离", "截止距离", "锥角内", "锥角外",
    "锥外音量", "方向 X", "方向 Y", "方向 Z"};

constexpr std::array<const char*, 4> kFlagLabels{"循环", "3D", "超范围停止", "音乐"};

constexpr std::string_view kDefaultInput = "默认";

std::string_view SharedFieldId(std::string_view field) {
    if (field == "volume") return "volume";
    if (field == "fadeInRate") return "fade_in";
    if (field == "fadeOutRate") return "fade_out";
    return {};
}

bool IsDefaultInput(std::string_view value) {
    if (value == kDefaultInput) return true;
    const QByteArray translated = QObject::tr("默认").toUtf8();
    return value == std::string_view(translated.constData(), static_cast<std::size_t>(translated.size()));
}

QString IntegerText(std::int32_t value) {
    return MapData::IsSoundDefaultInteger(value) ? QObject::tr("默认") : QString::number(value);
}

QString RealText(float value) {
    return MapData::IsSoundDefaultReal(value) ? QObject::tr("默认") : QLocale::c().toString(value, 'g', 9);
}

QString Text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

bool DecodeText(std::string_view bytes, QString& result, bool& local) {
    const QByteArray original(bytes.data(), static_cast<qsizetype>(bytes.size()));
    result = QString::fromUtf8(original);
    local = false;
    if (result.toUtf8() == original) return true;
    result = QString::fromLocal8Bit(original);
    local = true;
    return result.toLocal8Bit() == original;
}

bool EncodeText(const QString& text, bool local, std::string& output) {
    const QByteArray bytes = local ? text.toLocal8Bit() : text.toUtf8();
    if ((local ? QString::fromLocal8Bit(bytes) : QString::fromUtf8(bytes)) != text) return false;
    output.assign(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    return output.find('\0') == std::string::npos;
}

QString Issue(const Status& status) {
    return status.diagnostics.empty() ? QObject::tr("声音定义修改失败") : Text(status.diagnostics.front().message);
}

Status Bad(std::string code, std::string text) {
    return Status::Failure("Sounds", std::move(code), std::move(text));
}

bool IsAudioPath(const QString& path) {
    return path.endsWith(QLatin1String(".wav"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".ogg"), Qt::CaseInsensitive)
        || path.endsWith(QLatin1String(".flac"), Qt::CaseInsensitive);
}

// 无空格的变量名和反斜杠路径也必须可换行；不向模型插入换行符或零宽空格。
// 默认样式的 sizeHint 不保证使用当前列宽，这里让测量与绘制共用同一文本布局。
class WrappedSoundTextDelegate final : public QStyledItemDelegate {
public:
    explicit WrappedSoundTextDelegate(QTableWidget& table) : QStyledItemDelegate(&table), table_(table) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem styled = option;
        initStyleOption(&styled, index);
        const int width = table_.columnWidth(index.column());
        QTextLayout layout(styled.text, styled.font);
        const auto height = Layout(layout, width - 2 * kHorizontalPadding);
        return {width, static_cast<int>(std::ceil(height)) + 2 * kVerticalPadding + 1};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem styled = option;
        initStyleOption(&styled, index);
        QTextLayout layout(styled.text, styled.font);
        const auto height = Layout(layout, styled.rect.width() - 2 * kHorizontalPadding);
        styled.text.clear();
        styled.features &= ~QStyleOptionViewItem::HasDisplay;
        const auto* style = styled.widget ? styled.widget->style() : table_.style();
        style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, styled.widget);
        const auto group = !(styled.state & QStyle::State_Enabled) ? QPalette::Disabled
            : ((styled.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive);
        const auto role = (styled.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text;
        painter->save();
        painter->setClipRect(styled.rect);
        painter->setPen(styled.palette.color(group, role));
        layout.draw(painter, QPointF(styled.rect.left() + kHorizontalPadding,
            styled.rect.top() + std::max<double>(kVerticalPadding, (styled.rect.height() - height) / 2.0)));
        painter->restore();
    }

private:
    static qreal Layout(QTextLayout& layout, int width) {
        QTextOption textOption;
        textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(textOption);
        const QString text = layout.text();
        layout.beginLayout();
        qreal height = 0;
        while (true) {
            auto line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(std::max(1, width));
            // 路径优先在反斜杠后换行；单段放不下这一行时才在段内断开。
            const int start = line.textStart(), end = start + line.textLength();
            if (end < text.size() && text[end - 1] != QLatin1Char('\\') && text[end - 1] != QLatin1Char(' ')) {
                const auto slash = text.lastIndexOf(QLatin1Char('\\'), end - 1);
                if (slash >= start) line.setNumColumns(static_cast<int>(slash) + 1 - start);
            }
            line.setPosition({0, height});
            height += line.height();
        }
        layout.endLayout();
        return height;
    }

    // 与 Workbench 表格项目的内边距一致；字体和 DPI 从实际样式选项取得。
    static constexpr int kHorizontalPadding = 7;
    static constexpr int kVerticalPadding = 4;
    QTableWidget& table_;
};

void ConfigureWrappedTable(QTableWidget* table) {
    table->setWordWrap(true);
    table->setTextElideMode(Qt::ElideNone);
    // 拖动列宽或分栏时合并连续更新，避免逐像素扫描整张表。
    // 数据加载也共用此定时器；资源表本身已有 400 行显示上限。
    auto* rowHeightTimer = new QTimer(table);
    rowHeightTimer->setSingleShot(true);
    rowHeightTimer->setInterval(100);
    const auto schedule = [rowHeightTimer] { rowHeightTimer->start(); };
    QObject::connect(table->horizontalHeader(), &QHeaderView::sectionResized, table, schedule);
    QObject::connect(table->model(), &QAbstractItemModel::dataChanged, table, schedule);
    QObject::connect(table->model(), &QAbstractItemModel::rowsInserted, table, schedule);
    QObject::connect(table->model(), &QAbstractItemModel::rowsRemoved, table, schedule);
    QObject::connect(table->model(), &QAbstractItemModel::modelReset, table, schedule);
    QObject::connect(rowHeightTimer, &QTimer::timeout, table, [table] {
        // 只调整纵向尺寸；不再次修改列宽，防止表头信号递归。
        table->resizeRowsToContents();
    });
}

template <typename T>
bool ParseInteger(std::string_view input, T& output) {
    T candidate{};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), candidate);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) return false;
    output = candidate;
    return true;
}

bool ParseReal(std::string_view input, float& output) {
    float candidate = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), candidate);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || !std::isfinite(candidate)) return false;
    output = candidate;
    return true;
}

Status SetField(MapData::SoundRecord& item, std::string_view field, std::string_view value) {
    if (value.find('\0') != std::string_view::npos) return Bad("SOUND_TEXT_INVALID", "文本中不能包含空字节");
    if (field == "name") item.name = value;
    else if (field == "filePath") item.filePath = value;
    else if (field == "eaxSetting") item.eaxSetting = value;
    else {
        const auto integer = [&](std::int32_t& target) {
            if (IsDefaultInput(value)) { target = MapData::kSoundDefaultInteger; return true; }
            return ParseInteger(value, target);
        };
        const auto real = [&](float& target) {
            if (IsDefaultInput(value)) { target = std::bit_cast<float>(MapData::kSoundDefaultRealBits); return true; }
            return ParseReal(value, target);
        };
        bool okay = false;
        if (field == "pitch") okay = real(item.pitch);
        else if (field == "pitchVariance") okay = real(item.pitchVariance);
        else if (field == "priority") okay = integer(item.priority);
        else if (field == "channel") okay = integer(item.channel);
        else if (field == "minDistance") okay = real(item.minDistance);
        else if (field == "maxDistance") okay = real(item.maxDistance);
        else if (field == "distanceCutoff") okay = real(item.distanceCutoff);
        else if (field == "coneAngleInside") okay = real(item.coneAngleInside);
        else if (field == "coneAngleOutside") okay = real(item.coneAngleOutside);
        else if (field == "coneOutsideVolume") okay = integer(item.coneOutsideVolume);
        else if (field == "coneOrientationX") okay = real(item.coneOrientation[0]);
        else if (field == "coneOrientationY") okay = real(item.coneOrientation[1]);
        else if (field == "coneOrientationZ") okay = real(item.coneOrientation[2]);
        else if (field == "flags") okay = ParseInteger(value, item.flags);
        else return Bad("SOUND_FIELD_UNKNOWN", "未知的声音字段");
        if (!okay) return Bad("SOUND_NUMBER_INVALID", "声音字段需要有效的整数或有限浮点数");
    }
    if (item.name.empty() || item.filePath.empty())
        return Bad("SOUND_REQUIRED", "定义名与资源文件不能为空");
    return Status::Ok();
}

QString FieldText(const MapData::SoundRecord& item, std::string_view field) {
    if (field == "name") return Text(item.name);
    if (field == "filePath") return Text(item.filePath);
    if (field == "eaxSetting") return Text(item.eaxSetting);
    if (field == "fadeInRate") return IntegerText(item.fadeInRate);
    if (field == "fadeOutRate") return IntegerText(item.fadeOutRate);
    if (field == "volume") return IntegerText(item.volume);
    if (field == "pitch") return RealText(item.pitch);
    if (field == "pitchVariance") return RealText(item.pitchVariance);
    if (field == "priority") return IntegerText(item.priority);
    if (field == "channel") return IntegerText(item.channel);
    if (field == "minDistance") return RealText(item.minDistance);
    if (field == "maxDistance") return RealText(item.maxDistance);
    if (field == "distanceCutoff") return RealText(item.distanceCutoff);
    if (field == "coneAngleInside") return RealText(item.coneAngleInside);
    if (field == "coneAngleOutside") return RealText(item.coneAngleOutside);
    if (field == "coneOutsideVolume") return IntegerText(item.coneOutsideVolume);
    if (field == "coneOrientationX") return RealText(item.coneOrientation[0]);
    if (field == "coneOrientationY") return RealText(item.coneOrientation[1]);
    if (field == "coneOrientationZ") return RealText(item.coneOrientation[2]);
    return {};
}

Status CheckNames(const MapData::SoundsDocument& document) {
    for (std::size_t i = 0; i < document.entries.size(); ++i) {
        const auto& item = document.entries[i];
        if (item.name.empty() || item.filePath.empty()) return Bad("SOUND_REQUIRED", "定义名与资源文件不能为空");
        for (std::size_t j = 0; j < i; ++j) {
            const auto symbol = [](std::string_view name) {
                return name.starts_with("gg_snd_") ? std::string(name) : "gg_snd_" + Script::ConvertObjectName(name);
            };
            if (symbol(item.name) == symbol(document.entries[j].name))
                return Bad("SOUND_NAME_DUPLICATE", "声音定义名会生成重复的脚本变量");
        }
    }
    return Status::Ok();
}

bool HasKnownNameReferences(QtProject::EditorProject& project, std::string_view name) {
    const std::string symbol = name.starts_with("gg_snd_") ? std::string(name)
        : "gg_snd_" + Script::ConvertObjectName(name);
    const std::vector<std::string> tokens{std::string(name), symbol};
    return !project.References().UsagesOf(tokens).empty();
}

} // namespace

SoundsModule::SoundsModule(QtProject::EditorProject& project) : project_(project) {
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, [this] {
        StopPreview();
        selectedDefinition_ = -1;
        selectedBytes_.clear();
        Refresh(true);
    });
    connect(&project_, &QtProject::EditorProject::ImportsChanged, this, [this] { RebuildResources(); });
    if (auto* stack = UndoStack())
        connect(stack, &QUndoStack::indexChanged, this, [this] { Refresh(); });
}

SoundsModule::~SoundsModule() { StopPreview(); delete page_.data(); }

Shell::ModuleInfo SoundsModule::Info() const {
    return {"sounds", tr("声音"), "lucide/audio-lines", QKeySequence(Qt::ALT | Qt::Key_4)};
}

void SoundsModule::AttachHost(Shell::ModuleHost* host) { host_ = host; }
QUndoStack* SoundsModule::UndoStack() { return project_.mapDataUndoStack_; }

QWidget* SoundsModule::Widget() {
    if (!page_) BuildPage();
    return page_;
}

void SoundsModule::BuildPage() {
    page_ = new QWidget;
    page_->setObjectName("soundsWorkspace");
    auto* outer = new QVBoxLayout(page_);
    outer->setContentsMargins(12, 10, 12, 10);
    auto* title = new QLabel(tr("声音"), page_);
    title->setObjectName("paneTitle");
    outer->addWidget(title);
    auto* body = new QSplitter(Qt::Horizontal, page_);
    body->setObjectName("soundsSplitter");

    auto* resources = new QWidget(body);
    auto* left = new QVBoxLayout(resources);
    left->addWidget(new QLabel(tr("资源文件"), resources));
    search_ = new QLineEdit(resources);
    search_->setObjectName("soundResourceSearch");
    search_->setPlaceholderText(tr("搜索文件或来源"));
    search_->setClearButtonEnabled(true);
    left->addWidget(search_);
    resourceTable_ = new QTableWidget(resources);
    ConfigureWrappedTable(resourceTable_);
    resourceTable_->setObjectName("soundResourceTable");
    resourceTable_->setColumnCount(2);
    resourceTable_->setHorizontalHeaderLabels({tr("路径"), tr("来源")});
    resourceTable_->setItemDelegateForColumn(0, new WrappedSoundTextDelegate(*resourceTable_));
    resourceTable_->horizontalHeader()->setStretchLastSection(false);
    resourceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    resourceTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    resourceTable_->verticalHeader()->hide();
    resourceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    resourceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    resourceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    left->addWidget(resourceTable_, 1);
    auto* resourceAction = new QPushButton(tr("用选中资源新建定义"), resources);
    resourceAction->setObjectName("soundCreateDefinition");
    left->addWidget(resourceAction);
    auto* assignResource = new QPushButton(tr("设为当前定义的资源"), resources);
    assignResource->setObjectName("soundAssignResource");
    left->addWidget(assignResource);

    auto* definitions = new QWidget(body);
    auto* middle = new QVBoxLayout(definitions);
    auto* heading = new QHBoxLayout;
    heading->addWidget(new QLabel(tr("声音定义"), definitions));
    heading->addStretch();
    showRawValues_ = new QCheckBox(tr("显示原始字段"), definitions);
    showRawValues_->setObjectName("soundRawValues");
    heading->addWidget(showRawValues_);
    connect(showRawValues_, &QCheckBox::toggled, this, [this] { RebuildDefinitions(); });
    auto* duplicate = new QPushButton(tr("复制定义"), definitions);
    duplicate->setObjectName("soundDuplicateDefinition");
    heading->addWidget(duplicate);
    middle->addLayout(heading);
    definitionsTable_ = new QTableWidget(definitions);
    ConfigureWrappedTable(definitionsTable_);
    definitionsTable_->setObjectName("soundDefinitionsTable");
    definitionsTable_->setColumnCount(4);
    definitionsTable_->setHorizontalHeaderLabels({tr("定义"), tr("资源文件"), tr("模式"), tr("音量")});
    auto* definitionText = new WrappedSoundTextDelegate(*definitionsTable_);
    definitionsTable_->setItemDelegateForColumn(0, definitionText);
    definitionsTable_->setItemDelegateForColumn(1, definitionText);
    auto* definitionHeader = definitionsTable_->horizontalHeader();
    definitionHeader->setStretchLastSection(false);
    definitionHeader->setSectionResizeMode(0, QHeaderView::Interactive);
    definitionHeader->setSectionResizeMode(1, QHeaderView::Stretch);
    definitionHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    definitionHeader->setSectionResizeMode(3, QHeaderView::Interactive);
    // 长定义名和路径由委托按实际列宽换行，固定短列的初始宽度，
    // 避免音量中的异常原始值挤占路径列；用户仍可手动调整。
    definitionsTable_->setColumnWidth(0, 180);
    definitionsTable_->setColumnWidth(2, 64);
    definitionsTable_->setColumnWidth(3, 78);
    definitionsTable_->verticalHeader()->hide();
    definitions->setMinimumWidth(520);
    definitionsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    definitionsTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    definitionsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    middle->addWidget(definitionsTable_, 1);

    auto* inspector = new QWidget(body);
    auto* right = new QVBoxLayout(inspector);
    right->addWidget(new QLabel(tr("定义属性"), inspector));
    auto* normal = new QGroupBox(tr("基本"), inspector);
    auto* normalForm = new QFormLayout(normal);
    spatial_ = new QGroupBox(inspector);
    spatial_->setObjectName("soundSpatialFields");
    auto* spatialForm = new QFormLayout(spatial_);
    for (std::size_t i = 0; i < kFields.size(); ++i) {
        auto* editor = new QLineEdit(inspector);
        editor->setObjectName(QStringLiteral("soundField_%1").arg(
            QString::fromLatin1(kFields[i].data(), static_cast<qsizetype>(kFields[i].size()))));
        fields_.push_back(editor);
        if (i < 3) {
            defaultButtons_.push_back(nullptr);
            normalForm->addRow(tr(kLabels[i]), editor);
        } else {
            auto* row = new QWidget(inspector);
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            layout->addWidget(editor, 1);
            auto* useDefault = new QToolButton(row);
            useDefault->setText(tr("默认"));
            useDefault->setToolTip(tr("使用原版默认值"));
            useDefault->setObjectName(QStringLiteral("soundDefault_%1").arg(
                QString::fromLatin1(kFields[i].data(), static_cast<qsizetype>(kFields[i].size()))));
            layout->addWidget(useDefault);
            defaultButtons_.push_back(useDefault);
            (i >= 10 ? spatialForm : normalForm)->addRow(tr(kLabels[i]), row);
            connect(useDefault, &QToolButton::clicked, editor, [editor] { editor->setText(QObject::tr("默认")); });
        }
        connect(editor, &QLineEdit::textChanged, this, &SoundsModule::SetDraftChanged);
    }
    right->addWidget(normal);
    auto* flagsLayout = new QHBoxLayout;
    for (std::size_t i = 0; i < kFlagLabels.size(); ++i) {
        auto* flag = new QCheckBox(tr(kFlagLabels[i]), inspector);
        flag->setObjectName(QStringLiteral("soundFlag%1").arg(i));
        flags_.push_back(flag);
        flagsLayout->addWidget(flag);
        connect(flag, &QCheckBox::toggled, this, &SoundsModule::SetDraftChanged);
    }
    right->addLayout(flagsLayout);
    auto* spatialToggle = new QToolButton(inspector);
    spatialToggle->setObjectName("soundSpatialToggle");
    spatialToggle->setText(tr("空间参数"));
    spatialToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    spatialToggle->setArrowType(Qt::DownArrow);
    spatialToggle->setCheckable(true);
    spatialToggle->setChecked(true);
    spatialToggle->setAutoRaise(true);
    right->addWidget(spatialToggle);
    right->addWidget(spatial_);
    connect(spatialToggle, &QToolButton::toggled, this, [this, spatialToggle](bool expanded) {
        spatial_->setVisible(expanded);
        spatialToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    connect(flags_[1], &QCheckBox::toggled, spatial_, &QWidget::setEnabled);
    apply_ = new QPushButton(tr("应用修改"), inspector);
    apply_->setObjectName("soundApplyDefinition");
    right->addWidget(apply_);
    right->addStretch();

    body->addWidget(resources);
    body->addWidget(definitions);
    auto* inspectorScroll = new QScrollArea(body);
    inspectorScroll->setWidgetResizable(true);
    inspectorScroll->setFrameShape(QFrame::NoFrame);
    inspectorScroll->setWidget(inspector);
    body->addWidget(inspectorScroll);
    body->setStretchFactor(0, 1);
    body->setStretchFactor(1, 3);
    body->setStretchFactor(2, 1);
    body->setSizes({280, 620, 300});
    outer->addWidget(body, 1);
    auto* preview = new QHBoxLayout;
    auto* play = new QPushButton(tr("试听"), page_);
    play->setObjectName("soundPreviewPlay");
    auto* stop = new QPushButton(tr("停止"), page_);
    stop->setObjectName("soundPreviewStop");
    preview->addWidget(play);
    preview->addWidget(stop);
    status_ = new QLabel(page_);
    status_->setObjectName("soundStatus");
    preview->addWidget(status_, 1);
    outer->addLayout(preview);

    previewPlayer_ = new SoundPreviewPlayer(this);
    connect(previewPlayer_, &SoundPreviewPlayer::Error, this, [this](const QString& message) {
        if (!message.isEmpty()) ShowStatus(message, true);
    });
    connect(search_, &QLineEdit::textChanged, this, &SoundsModule::RebuildResources);
    connect(resourceTable_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) { StartPreview(); });
    connect(resourceAction, &QPushButton::clicked, this, [this] {
        const auto row = resourceTable_->currentRow();
        if (row < 0 || static_cast<std::size_t>(row) >= resourceRows_.size()) return;
        const auto path = resourceRows_[static_cast<std::size_t>(row)].path.toStdString();
        const QString stem = resourceRows_[static_cast<std::size_t>(row)].path.section(QLatin1Char('\\'), -1).section(QLatin1Char('.'), 0, 0);
        const auto status = AddDefinition((stem.isEmpty() ? QStringLiteral("Sound") : stem).toStdString(), path);
        if (!status.Succeeded()) ShowStatus(Issue(status), true);
    });
    connect(assignResource, &QPushButton::clicked, this, [this] {
        const int row = resourceTable_->currentRow();
        if (row < 0 || selectedDefinition_ < 0
            || static_cast<std::size_t>(row) >= resourceRows_.size()) return;
        if (opaqueText_[1]) {
            ShowStatus(tr("原文件路径编码无法无损识别，暂不能替换"), true);
            return;
        }
        fields_[1]->setText(resourceRows_[static_cast<std::size_t>(row)].path);
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const int row = definitionsTable_->currentRow();
        if (row < 0) return;
        const auto status = DuplicateDefinition(static_cast<std::size_t>(row));
        if (!status.Succeeded()) ShowStatus(Issue(status), true);
    });
    connect(definitionsTable_, &QTableWidget::currentCellChanged, this,
        [this](int row, int, int previous, int) {
            if (loading_) return;
            if (draftChanged_ && previous >= 0 && row != previous) {
                QSignalBlocker blocker(definitionsTable_);
                definitionsTable_->selectRow(previous);
                ShowStatus(tr("先应用或放弃当前修改，再切换定义"), true);
                return;
            }
            selectedDefinition_ = row;
            LoadInspector();
        });
    connect(apply_, &QPushButton::clicked, this, [this] {
        const auto status = ApplyInspector();
        if (!status.Succeeded()) ShowStatus(Issue(status), true);
    });
    connect(play, &QPushButton::clicked, this, &SoundsModule::StartPreview);
    connect(stop, &QPushButton::clicked, this, &SoundsModule::StopPreview);
    Refresh(true);
}

void SoundsModule::ShowStatus(const QString& message, bool error) const {
    if (status_) {
        status_->setText(message);
        status_->setProperty("error", error);
    }
    if (host_) host_->ShowStatusMessage(message, error ? 7000 : 3500);
}

void SoundsModule::SetDraftChanged() {
    if (loading_ || selectedDefinition_ < 0) return;
    const bool changed = !draftChanged_;
    draftChanged_ = true;
    if (changed) emit DraftChanged(true);
    if (apply_) apply_->setEnabled(true);
}

bool SoundsModule::ApplyPendingDraft() {
    if (!draftChanged_) return true;
    const auto status = ApplyInspector();
    if (!status.Succeeded()) ShowStatus(Issue(status), true);
    return status.Succeeded();
}

void SoundsModule::DiscardPendingDraft() { Refresh(true); }

void SoundsModule::Refresh(bool discardDraft) {
    if (!page_) return;
    if (draftChanged_ && !discardDraft) {
        ShowStatus(tr("地图声音数据已变化；应用或放弃当前属性修改后刷新"), true);
        return;
    }
    const bool hadDraft = draftChanged_;
    draftChanged_ = false;
    const auto snapshot = project_.MapEdits() ? project_.MapEdits()->Snapshot()
        : (project_.mapDocument_ ? *project_.mapDocument_
            : MapDocument::FromMembers(std::span<const MapArchiveMember>{}));
    visibleSounds_ = snapshot.Sounds();
    RebuildDefinitions();
    RebuildResources();
    LoadInspector();
    if (hadDraft) emit DraftChanged(false);
}

void SoundsModule::RebuildDefinitions() {
    if (!definitionsTable_) return;
    loading_ = true;
    QSignalBlocker blocker(definitionsTable_);
    const auto* sounds = visibleSounds_ ? &*visibleSounds_ : nullptr;
    definitionsTable_->setRowCount(sounds ? static_cast<int>(sounds->entries.size()) : 0);
    if (sounds) for (int row = 0; row < definitionsTable_->rowCount(); ++row) {
        const auto& entry = sounds->entries[static_cast<std::size_t>(row)];
        const auto name = ResolveProjectName(project_, entry.name, "string");
        const auto path = ResolveProjectName(project_, entry.filePath, "string");
        const auto rawText = [](std::string_view bytes) {
            return QString::fromStdString(Trigger::DecodeSourceText(std::as_bytes(std::span(bytes.data(), bytes.size()))).text);
        };
        const bool raw = showRawValues_ && showRawValues_->isChecked();
        const std::array columns{raw ? rawText(entry.name) : Text(name.displayName), raw ? rawText(entry.filePath) : Text(path.displayName),
            entry.IsMusic() ? tr("音乐") : (entry.Is3D() ? tr("3D") : tr("2D")), IntegerText(entry.volume)};
        for (int column = 0; column < 4; ++column) {
            auto* item = new QTableWidgetItem(columns[column]);
            item->setToolTip(column == 0 ? rawText(entry.name) + (name.reason.empty() ? QString{} : "\n" + Text(name.reason))
                : column == 1 ? rawText(entry.filePath) + (path.reason.empty() ? QString{} : "\n" + Text(path.reason)) : columns[column]);
            definitionsTable_->setItem(row, column, item);
        }
    }
    if (selectedDefinition_ < 0 || selectedDefinition_ >= definitionsTable_->rowCount())
        selectedDefinition_ = definitionsTable_->rowCount() ? 0 : -1;
    if (selectedDefinition_ >= 0) definitionsTable_->selectRow(selectedDefinition_);
    loading_ = false;
}

void SoundsModule::StartGameIndex() {
    const auto root = project_.war3GameRoot_;
    if (root != gameIndexRoot_) {
        gameIndexRoot_ = root;
        ++gameIndexGeneration_;
        gameDirectoryRows_.clear();
        gameIndexPending_ = false;
        gameIndexLoaded_ = false;
    }
    if (root.empty() || gameIndexPending_ || gameIndexLoaded_) return;
    gameIndexPending_ = true;
    const auto generation = gameIndexGeneration_;
    using Catalog = Result<std::vector<Game::GameResourceFile>>;
    auto result = std::make_shared<Catalog>();
    auto* worker = QThread::create([root, result] {
        auto mounted = Game::GameDataSource::Mount(root);
        if (!mounted.Succeeded() || !mounted.value) {
            result->diagnostics = std::move(mounted.diagnostics);
            return;
        }
        auto listed = mounted.value->ListFilesWithSource();
        auto fromSoundInfo = mounted.value->DiscoverSoundFilesFromSoundInfo();
        std::map<std::string, Game::GameResourceFile> found;
        const auto collect = [&](const auto& inventory) {
            if (!inventory.value) return;
            for (const auto& file : *inventory.value) {
                auto canonical = Resources::CanonicalImportPath(file.path);
                if (canonical.Succeeded() && canonical.value)
                    found.try_emplace(*canonical.value, file);
            }
        };
        collect(listed);
        collect(fromSoundInfo);
        result->value.emplace();
        result->value->reserve(found.size());
        for (auto& entry : found) result->value->push_back(std::move(entry.second));
        for (auto& diagnostic : listed.diagnostics) result->diagnostics.push_back(std::move(diagnostic));
        for (auto& diagnostic : fromSoundInfo.diagnostics) result->diagnostics.push_back(std::move(diagnostic));
    });
    connect(worker, &QThread::finished, this, [this, generation, result] {
        if (generation != gameIndexGeneration_ || gameIndexRoot_ != project_.war3GameRoot_) return;
        gameIndexPending_ = false;
        gameIndexLoaded_ = true;
        if (result->Succeeded() && result->value) {
            gameDirectoryRows_.reserve(result->value->size());
            for (const auto& file : *result->value) {
                const QString path = Text(file.path).replace(QLatin1Char('/'), QLatin1Char('\\'));
                if (!IsAudioPath(path)) continue;
                gameDirectoryRows_.push_back({path,
                    tr("游戏 · %1").arg(Text(file.archiveName)), {}, {}, true});
            }
        }
        if (!result->diagnostics.empty())
            ShowStatus(tr("游戏声音目录不完整：%1").arg(Text(result->diagnostics.front().message)), true);
        RebuildResources();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowestPriority);
}

std::vector<SoundsModule::ResourceRow> SoundsModule::ResourceCatalogRows() const {
    std::map<QString, ResourceRow> found;
    const auto add = [&](ResourceRow row) {
        if (!IsAudioPath(row.path)) return;
        const QString key = QtProject::EditorProject::AssetKey(row.path);
        const auto existing = found.find(key);
        if (existing == found.end()) found.emplace(key, std::move(row));
        else if (!row.diskPath.isEmpty()) {
            row.source = tr("待导入 · 覆盖同名资源");
            existing->second = std::move(row);
        }
    };
    for (const auto& asset : project_.AssetEntries())
        add({asset.path, asset.Pending() ? tr("待导入") : tr("地图 · %1").arg(asset.originalPath), asset.diskPath,
            asset.originalPath, false});
    for (const auto& row : gameDirectoryRows_) add(row);
    if (visibleSounds_) for (const auto& entry : visibleSounds_->entries) {
        const QString path = Text(entry.filePath);
        if (found.contains(QtProject::EditorProject::AssetKey(path))) continue;
        add({path, gameIndexPending_ ? tr("游戏目录读取中") : tr("未定位"), {}, {}, true});
    }
    std::vector<ResourceRow> rows; rows.reserve(found.size());
    for (auto& [key, row] : found) { (void)key; rows.push_back(std::move(row)); }
    return rows;
}

void SoundsModule::RebuildResources() {
    if (!resourceTable_) return;
    StartGameIndex();
    auto found = ResourceCatalogRows();
    const QString selected = resourceTable_->currentRow() >= 0
        && static_cast<std::size_t>(resourceTable_->currentRow()) < resourceRows_.size()
        ? resourceRows_[static_cast<std::size_t>(resourceTable_->currentRow())].path : QString{};
    resourceRows_.clear();
    QSignalBlocker blocker(resourceTable_);
    resourceTable_->setRowCount(0);
    const QString filter = search_ ? search_->text().trimmed() : QString{};
    int selectedRow = -1;
    constexpr int kVisibleLimit = 400;
    int totalMatches = 0;
    for (auto& row : found) {
        if (!filter.isEmpty() && !row.path.contains(filter, Qt::CaseInsensitive)
            && !row.source.contains(filter, Qt::CaseInsensitive)) continue;
        ++totalMatches;
        if (resourceTable_->rowCount() >= kVisibleLimit) continue;
        const int index = resourceTable_->rowCount();
        resourceTable_->insertRow(index);
        auto* pathCell = new QTableWidgetItem(row.path);
        pathCell->setToolTip(row.path);
        resourceTable_->setItem(index, 0, pathCell);
        auto* sourceCell = new QTableWidgetItem(row.source);
        if (!row.diskPath.isEmpty()) sourceCell->setToolTip(row.diskPath);
        else if (!row.archivePath.isEmpty() && project_.archiveSession_)
            sourceCell->setToolTip(QString::fromStdWString(project_.archiveSession_->SourcePath().wstring()));
        resourceTable_->setItem(index, 1, sourceCell);
        if (row.path == selected) selectedRow = index;
        resourceRows_.push_back(std::move(row));
    }
    if (selectedRow >= 0) resourceTable_->selectRow(selectedRow);
    resourceTable_->setToolTip(totalMatches > kVisibleLimit
        ? tr("匹配 %1 项，当前显示前 %2 项；继续输入关键词以缩小范围").arg(totalMatches).arg(kVisibleLimit)
        : tr("匹配 %1 项").arg(totalMatches));
}

void SoundsModule::LoadInspector() {
    if (!page_) return;
    loading_ = true;
    const bool hadDraft = draftChanged_;
    draftChanged_ = false;
    if (hadDraft) emit DraftChanged(false);
    const auto* sounds = visibleSounds_ ? &*visibleSounds_ : nullptr;
    const bool enabled = sounds && selectedDefinition_ >= 0
        && static_cast<std::size_t>(selectedDefinition_) < sounds->entries.size();
    for (auto* field : fields_) { field->setEnabled(enabled); field->clear(); }
    for (auto* button : defaultButtons_) if (button) button->setEnabled(enabled);
    for (auto* flag : flags_) { flag->setEnabled(enabled); flag->setChecked(false); }
    selectedBytes_.clear();
    selectedSessionIdentity_ = project_.MapEdits() ? project_.MapEdits()->SessionIdentity() : 0;
    selectedRevision_ = project_.MapEdits() ? project_.MapEdits()->Revision() : 0;
    if (enabled) {
        const auto& item = sounds->entries[static_cast<std::size_t>(selectedDefinition_)];
        const auto bytes = sounds->Serialize();
        if (bytes.Succeeded() && bytes.value) selectedBytes_ = *bytes.value;
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            QString text = FieldText(item, kFields[i]);
            if (i < 3) {
                originalTexts_[i] = i == 0 ? item.name : i == 1 ? item.filePath : item.eaxSetting;
                opaqueText_[i] = !DecodeText(originalTexts_[i], text, localText_[i]);
                fields_[i]->setReadOnly(opaqueText_[i]);
                if (opaqueText_[i]) fields_[i]->setToolTip(tr("原文编码无法无损识别；保留原字节"));
                else fields_[i]->setToolTip({});
            }
            fields_[i]->setText(text);
        }
        for (std::size_t i = 0; i < flags_.size(); ++i) flags_[i]->setChecked((item.flags & (1u << i)) != 0);
        // 同一来源资格同时用于呈现和提交；只控制本单元的三个字段。
        Status sharedAccess = Bad("SOUND_NO_PROJECT", "尚未打开地图数据");
        if (project_.mapDocument_ && project_.MapEdits()) {
            sharedAccess = MapData::ValidateSoundFieldSource(*project_.mapDocument_);
            if (sharedAccess.Succeeded()) {
                const auto current = MapData::ProjectMapData(*project_.mapDocument_, *project_.MapEdits());
                sharedAccess = current.Succeeded() && current.value
                    ? MapData::ValidateSoundFieldSource(*current.value) : Status{current.diagnostics};
            }
        }
        for (std::size_t i = 0; i < fields_.size(); ++i) if (!SharedFieldId(kFields[i]).empty()) {
            fields_[i]->setEnabled(sharedAccess.Succeeded());
            fields_[i]->setToolTip(sharedAccess.Succeeded() ? QString{} : Issue(sharedAccess));
            if (defaultButtons_[i]) defaultButtons_[i]->setEnabled(sharedAccess.Succeeded());
        }
    }
    spatial_->setEnabled(enabled && sounds->entries[static_cast<std::size_t>(selectedDefinition_)].Is3D());
    apply_->setEnabled(false);
    loading_ = false;
}

Status SoundsModule::ApplySerialized(const std::vector<std::byte>& expected,
    const std::vector<std::byte>& replacement) {
    auto* edits = project_.MapEdits();
    if (!edits) return Bad("SOUND_NO_PROJECT", "尚未打开地图数据");
    const auto snapshot = edits->Snapshot();
    if (snapshot.Sounds()) {
        const auto before = snapshot.Sounds()->Serialize();
        if (!before.Succeeded() || !before.value || *before.value != expected)
            return Bad("SOUND_STALE", "声音定义已改变，请重新载入后再编辑");
    } else if (!expected.empty()) return Bad("SOUND_STALE", "声音定义来源已改变");
    const auto status = replacement.empty() ? edits->Remove(MapDataKind::Sounds)
        : edits->SetSerialized(MapDataKind::Sounds, replacement);
    if (!status.Succeeded()) project_.AddDiagnostics(status.diagnostics);
    else (void)project_.ObserveEditState("修改声音定义");
    return status;
}

Status SoundsModule::Commit(const MapData::SoundsDocument& document,
    const std::vector<std::byte>& before, const QString& description) {
    const auto names = CheckNames(document);
    if (!names.Succeeded()) return names;
    const auto after = document.Serialize();
    if (!after.Succeeded() || !after.value) return Status{after.diagnostics};
    if (*after.value == before) {
        Refresh(true);
        return Status::Ok();
    }
    const auto status = ApplySerialized(before, *after.value);
    if (!status.Succeeded()) return status;
    QtProject::PushMapDataEdit(project_, description, "修改声音定义");
    Refresh(true);
    return Status::Ok();
}

Status SoundsModule::EditField(std::size_t row, std::string_view field, std::string_view value) {
    if (draftChanged_) return Bad("SOUND_DRAFT_PENDING", "先应用或放弃当前属性修改");
    if (!SharedFieldId(field).empty())
        return CommitFields(row, {{std::string(field), std::string(value)}});
    auto* edits = project_.MapEdits();
    if (!edits) return Bad("SOUND_NO_PROJECT", "尚未打开地图数据");
    const auto snapshot = edits->Snapshot();
    if (!snapshot.Sounds()) return Bad("SOUND_SOURCE_MISSING", "此地图没有声音定义文件");
    auto document = *snapshot.Sounds();
    if (row >= document.entries.size()) return Bad("SOUND_ROW_INVALID", "声音定义行不存在");
    if (field == "name" && std::string_view(document.entries[row].name) != value
        && HasKnownNameReferences(project_, document.entries[row].name))
        return Bad("SOUND_NAME_REFERENCED", "该定义已被地图静态引用；请先处理引用再改名");
    const auto before = document.Serialize();
    if (!before.Succeeded() || !before.value) return Status{before.diagnostics};
    const auto status = SetField(document.entries[row], field, value);
    if (!status.Succeeded()) return status;
    selectedDefinition_ = static_cast<int>(row);
    return Commit(document, *before.value, tr("修改声音定义"));
}

Status SoundsModule::CommitFields(std::size_t row,
    const std::vector<std::pair<std::string, std::string>>& values,
    const MapData::SoundsDocument* additional) {
    auto* session = project_.MapEdits();
    if (!session || !project_.mapDocument_) return Bad("SOUND_NO_PROJECT", "尚未打开地图数据");
    const auto& original = *project_.mapDocument_;
    const auto current = MapData::ProjectMapData(original, *session);
    if (!current.Succeeded() || !current.value) return Status{current.diagnostics};
    for (const auto* source : {&original, &*current.value}) {
        const auto checked = MapData::ValidateSoundFieldSource(*source);
        if (!checked.Succeeded()) return checked;
    }
    const auto key = MapData::MapFieldRowKey(*current.value, MapDataKind::Sounds, row);
    if (!key.Succeeded() || !key.value) return Status{key.diagnostics};
    std::vector<MapData::MapFieldEdit> changes;
    for (const auto& [field, text] : values) {
        const auto* descriptor = MapData::FindMapField(MapDataKind::Sounds, SharedFieldId(field));
        if (!descriptor) return Bad("SOUND_FIELD_UNKNOWN", "未知的声音字段");
        const auto value = MapData::ParseMapFieldInput(*descriptor, IsDefaultInput(text) ? "-1" : text);
        if (!value.Succeeded() || !value.value) return Status{value.diagnostics};
        const auto checked = MapData::ValidateMapFieldValue(MapDataKind::Sounds, descriptor->id, *value.value);
        if (!checked.Succeeded()) return checked;
        const auto before = MapData::ReadMapField(*current.value, MapDataKind::Sounds, row, *descriptor);
        if (!before.Succeeded() || !before.value) return Status{before.diagnostics};
        if (*before.value != *value.value)
            changes.push_back({MapDataKind::Sounds, row, *key.value, std::string(descriptor->id), *before.value, *value.value});
    }
    if (changes.empty()) {
        if (additional) return Commit(*additional,
            current.value->Member(MapDataKind::Sounds).sources.front().data, tr("修改声音定义"));
        Refresh(true);
        return Status::Ok();
    }
    const auto sha = Sha256(current.value->Member(MapDataKind::Sounds).sources.front().data);
    if (!sha.Succeeded() || !sha.value) return Status{sha.diagnostics};
    const auto plan = MapData::PrepareMapDataFields(original, *current.value, *sha.value, changes);
    if (!plan.Succeeded() || !plan.value) return Status{plan.diagnostics};
    if (additional) {
        // Inspector 原有完整属性批次：三字段由 Prepared 产生，其他属性保持旧 UI 能力。
        // 不发布中间状态；整批仍只生成一个原会话回执，不扩大 MCP 字段/保存权限。
        auto combined = *additional;
        const auto& prepared = plan.value->After().Sounds()->entries[row];
        combined.entries[row].volume = prepared.volume;
        combined.entries[row].fadeInRate = prepared.fadeInRate;
        combined.entries[row].fadeOutRate = prepared.fadeOutRate;
        return Commit(combined, current.value->Member(MapDataKind::Sounds).sources.front().data,
            tr("修改声音定义"));
    }
    const auto applied = session->ApplyPreparedFields(*plan.value);
    if (!applied.Succeeded()) return applied;
    (void)project_.ObserveEditState("修改声音定义");
    QtProject::PushMapDataEdit(project_, tr("修改声音定义"), "修改声音定义");
    selectedDefinition_ = static_cast<int>(row);
    Refresh(true);
    return Status::Ok();
}

Status SoundsModule::AddDefinition(std::string name, std::string filePath) {
    if (draftChanged_) return Bad("SOUND_DRAFT_PENDING", "先应用或放弃当前属性修改");
    auto* edits = project_.MapEdits();
    if (!edits) return Bad("SOUND_NO_PROJECT", "尚未打开地图数据");
    const auto snapshot = edits->Snapshot();
    if (snapshot.Member(MapDataKind::Sounds).state != MapDataState::Ready
        && snapshot.Member(MapDataKind::Sounds).state != MapDataState::Missing)
        return Bad("SOUND_SOURCE_UNSAFE", "声音定义来源损坏或不明确，不能新增");
    MapData::SoundsDocument document = snapshot.Sounds().value_or(MapData::SoundsDocument{1, {}, {}});
    std::vector<std::byte> before;
    if (snapshot.Sounds()) {
        const auto serialized = snapshot.Sounds()->Serialize();
        if (!serialized.Succeeded() || !serialized.value) return Status{serialized.diagnostics};
        before = *serialized.value;
    }
    if (name.empty() || filePath.empty()) return Bad("SOUND_REQUIRED", "定义名与资源文件不能为空");
    const auto base = name;
    for (unsigned index = 1; std::any_of(document.entries.begin(), document.entries.end(),
        [&](const auto& entry) { return entry.name == name; }); ++index)
        name = base + "_" + std::to_string(index);
    MapData::SoundRecord item{};
    item.name = std::move(name);
    item.filePath = std::move(filePath);
    item.eaxSetting = "DefaultEAXON";
    item.volume = 127;
    item.pitch = 1.0f;
    item.channel = 0;
    item.minDistance = 100.0f;
    item.maxDistance = 10000.0f;
    item.distanceCutoff = 3000.0f;
    item.coneAngleInside = 360.0f;
    item.coneAngleOutside = 360.0f;
    item.coneOutsideVolume = 127;
    selectedDefinition_ = static_cast<int>(document.entries.size());
    document.entries.push_back(std::move(item));
    return Commit(document, before, tr("新建声音定义"));
}

Status SoundsModule::DuplicateDefinition(std::size_t row) {
    if (draftChanged_) return Bad("SOUND_DRAFT_PENDING", "先应用或放弃当前属性修改");
    auto* edits = project_.MapEdits();
    if (!edits) return Bad("SOUND_NO_PROJECT", "尚未打开地图数据");
    const auto snapshot = edits->Snapshot();
    if (!snapshot.Sounds() || row >= snapshot.Sounds()->entries.size())
        return Bad("SOUND_ROW_INVALID", "声音定义行不存在");
    auto document = *snapshot.Sounds();
    const auto before = document.Serialize();
    if (!before.Succeeded() || !before.value) return Status{before.diagnostics};
    auto copy = document.entries[row];
    const auto base = copy.name + "_Copy";
    copy.name = base;
    for (unsigned index = 2; std::any_of(document.entries.begin(), document.entries.end(),
        [&](const auto& entry) { return entry.name == copy.name; }); ++index)
        copy.name = base + std::to_string(index);
    selectedDefinition_ = static_cast<int>(document.entries.size());
    document.entries.push_back(std::move(copy));
    return Commit(document, *before.value, tr("复制声音定义"));
}

Status SoundsModule::ApplyInspector() {
    auto* edits = project_.MapEdits();
    if (!edits || selectedDefinition_ < 0) return Bad("SOUND_NO_SELECTION", "请选择声音定义");
    if (edits->SessionIdentity() != selectedSessionIdentity_ || edits->Revision() != selectedRevision_)
        return Bad("SOUND_STALE", "地图数据修订已变化，请放弃草稿并重新载入");
    const auto snapshot = edits->Snapshot();
    if (!snapshot.Sounds() || static_cast<std::size_t>(selectedDefinition_) >= snapshot.Sounds()->entries.size())
        return Bad("SOUND_ROW_INVALID", "声音定义已不存在");
    const auto current = snapshot.Sounds()->Serialize();
    if (!current.Succeeded() || !current.value || *current.value != selectedBytes_)
        return Bad("SOUND_STALE", "声音定义已变化，请放弃草稿并重新载入");
    auto document = *snapshot.Sounds();
    auto& item = document.entries[static_cast<std::size_t>(selectedDefinition_)];
    std::vector<std::pair<std::string, std::string>> sharedValues;
    if (!opaqueText_[0] && fields_[0]->text() != FieldText(item, "name")
        && HasKnownNameReferences(project_, item.name))
        return Bad("SOUND_NAME_REFERENCED", "该定义已被地图静态引用；请先处理引用再改名");
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        // 未修改的数值保留原始位模式；尤其不能通过显示文本重新解析默认哨兵或未知值。
        if (i >= 3 && fields_[i]->text().trimmed() == FieldText(item, kFields[i])) continue;
        std::string value;
        if (i < 3) {
            if (opaqueText_[i] || fields_[i]->text() == FieldText(item, kFields[i])) value = originalTexts_[i];
            else if (!EncodeText(fields_[i]->text(), localText_[i], value))
                return Bad("SOUND_ENCODING", "修改后的文本无法按原文件编码无损写回");
        } else value = fields_[i]->text().trimmed().toStdString();
        if (!SharedFieldId(kFields[i]).empty()) {
            sharedValues.emplace_back(kFields[i], std::move(value));
            continue;
        }
        const auto status = SetField(item, kFields[i], value);
        if (!status.Succeeded()) return status;
    }
    for (std::size_t i = 0; i < flags_.size(); ++i)
        item.flags = (item.flags & ~(1u << i)) | (flags_[i]->isChecked() ? (1u << i) : 0u);
    if (!sharedValues.empty()) {
        const auto otherBytes = document.Serialize();
        if (!otherBytes.Succeeded() || !otherBytes.value) return Status{otherBytes.diagnostics};
        return CommitFields(static_cast<std::size_t>(selectedDefinition_), sharedValues,
            *otherBytes.value == *current.value ? nullptr : &document);
    }
    return Commit(document, *current.value, tr("修改声音定义"));
}

bool SoundsModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "sounds" || (!target.kind.isEmpty() && target.kind != "sound")) return false;
    (void)Widget();
    if (target.extra.contains("sourceIndex")) {
        const auto value = target.extra.value("sourceIndex");
        const auto text = value.toString();
        std::uint64_t row = 0;
        if (draftChanged_ || value.typeId() != QMetaType::QString || text.isEmpty() || text.size() > 20 ||
            !ParseInteger(text.toStdString(), row) || row >= static_cast<std::uint64_t>(definitionsTable_->rowCount())) return false;
        definitionsTable_->selectRow(static_cast<int>(row));
        selectedDefinition_ = static_cast<int>(row);
        LoadInspector();
        return true;
    }
    if (target.id.isEmpty()) return true;
    if (draftChanged_) return false;
    for (int row = 0; row < definitionsTable_->rowCount(); ++row) {
        if (definitionsTable_->item(row, 0)->text() != target.id) continue;
        definitionsTable_->selectRow(row);
        selectedDefinition_ = row;
        LoadInspector();
        return true;
    }
    return false;
}

QJsonObject SoundsModule::SaveViewState() const {
    const QString selected = selectedDefinition_ >= 0 && definitionsTable_
        && selectedDefinition_ < definitionsTable_->rowCount()
        ? definitionsTable_->item(selectedDefinition_, 0)->text() : QString{};
    return {{"definition", selected}, {"search", search_ ? search_->text() : QString{}}};
}

void SoundsModule::RestoreViewState(const QJsonObject& state) {
    if (!page_) (void)Widget();
    if (search_) search_->setText(state.value("search").toString());
    (void)Reveal({"sounds", "sound", state.value("definition").toString(), {}});
}

namespace {
enum class DefinitionNavigationFailure { None, Unsupported, RowByteBudget, TotalByteBudget, Serialization, Digest };

QString DefinitionNavigationFailureCode(DefinitionNavigationFailure failure) {
    switch (failure) {
    case DefinitionNavigationFailure::Unsupported: return QStringLiteral("sound-navigation-unsupported");
    case DefinitionNavigationFailure::RowByteBudget: return QStringLiteral("sound-navigation-row-byte-budget");
    case DefinitionNavigationFailure::TotalByteBudget: return QStringLiteral("sound-navigation-total-byte-budget");
    case DefinitionNavigationFailure::Serialization: return QStringLiteral("sound-navigation-serialization");
    case DefinitionNavigationFailure::Digest: return QStringLiteral("sound-navigation-digest");
    default: return {};
    }
}

QString DefinitionNavigationFailureText(DefinitionNavigationFailure failure) {
    switch (failure) {
    case DefinitionNavigationFailure::Unsupported: return QObject::tr("声音定义格式不支持导航身份核对");
    case DefinitionNavigationFailure::RowByteBudget: return QObject::tr("声音定义超过每条64KiB导航身份预算");
    case DefinitionNavigationFailure::TotalByteBudget: return QObject::tr("声音定义核对超过合计1MiB导航身份预算");
    case DefinitionNavigationFailure::Serialization: return QObject::tr("声音定义无法按预期完整序列化，导航身份未确认");
    case DefinitionNavigationFailure::Digest: return QObject::tr("声音定义摘要无法生成，导航身份未确认");
    default: return QObject::tr("声音定义导航身份未确认");
    }
}

std::optional<std::string> DefinitionNavigationKey(const MapData::SoundRecord& row, std::uint32_t version,
    std::size_t& processedBytes, DefinitionNavigationFailure& failure) {
    // 复用真实序列化的全部字段；书签只保存散列，不保存正文、显示名或当前数组位置。
    // v1 singleton 固定为79字节头/终止符/标量；先核长度，再复制、序列化及哈希。
    constexpr std::size_t kRowBytes = 64 * 1024, kTotalBytes = 1024 * 1024;
    failure = DefinitionNavigationFailure::None;
    if (version != 1 || row.extended) { failure = DefinitionNavigationFailure::Unsupported; return {}; }
    if (processedBytes > kTotalBytes) { failure = DefinitionNavigationFailure::TotalByteBudget; return {}; }
    std::size_t expectedBytes = 79;
    for (const auto* text : {&row.name, &row.filePath, &row.eaxSetting}) {
        if (text->size() > kRowBytes - expectedBytes) { failure = DefinitionNavigationFailure::RowByteBudget; return {}; }
        expectedBytes += text->size();
    }
    if (expectedBytes > kTotalBytes - processedBytes) { failure = DefinitionNavigationFailure::TotalByteBudget; return {}; }
    processedBytes += expectedBytes; // 全部实际尝试都计费；包括选中项及其唯一性复核。
    const MapData::SoundsDocument singleton{version, {row}, {}};
    const auto bytes = singleton.Serialize();
    if (!bytes.Succeeded() || !bytes.value || bytes.value->size() != expectedBytes) {
        failure = DefinitionNavigationFailure::Serialization; return {};
    }
    const auto digest = Sha256(*bytes.value);
    if (!digest.Succeeded() || !digest.value) { failure = DefinitionNavigationFailure::Digest; return {}; }
    return digest.value;
}
}

std::optional<QJsonObject> SoundsModule::CaptureNavigationBookmark() const {
    const auto* edits = project_.MapEdits();
    if (!page_ || !edits || !definitionsTable_ || !resourceTable_ || !search_ || draftChanged_
        || selectedSessionIdentity_ != edits->SessionIdentity() || selectedRevision_ != edits->Revision()
        || gameIndexRoot_ != project_.war3GameRoot_) return {};
    if (status_) status_->setProperty("navigationIdentityFailure", QString{});
    if (visibleSounds_ && visibleSounds_->entries.size() > 4096) {
        if (status_) status_->setProperty("navigationIdentityFailure", QStringLiteral("sound-navigation-definition-count-budget"));
        ShowStatus(tr("声音定义超过4096条导航身份预算；不记录不可信位置"), true); return {};
    }
    std::size_t processedBytes = 0;
    DefinitionNavigationFailure failure = DefinitionNavigationFailure::None;
    std::optional<std::string> definition;
    if (selectedDefinition_ >= 0) {
        if (!visibleSounds_ || static_cast<std::size_t>(selectedDefinition_) >= visibleSounds_->entries.size()) return {};
        definition = DefinitionNavigationKey(visibleSounds_->entries[static_cast<std::size_t>(selectedDefinition_)], visibleSounds_->version, processedBytes, failure);
        if (!definition) {
            if (status_) status_->setProperty("navigationIdentityFailure", DefinitionNavigationFailureCode(failure));
            ShowStatus(tr("%1；不记录位置").arg(DefinitionNavigationFailureText(failure)), true); return {};
        }
        std::size_t matches = 0;
        for (const auto& row : visibleSounds_->entries) {
            const auto key = DefinitionNavigationKey(row, visibleSounds_->version, processedBytes, failure);
            if (!key) {
                if (status_) status_->setProperty("navigationIdentityFailure", DefinitionNavigationFailureCode(failure));
                ShowStatus(tr("%1；不记录位置").arg(DefinitionNavigationFailureText(failure)), true); return {};
            }
            if (*key == *definition) ++matches;
        }
        if (matches != 1) return {}; // 同字节双胞胎没有稳定记录身份；不能用显示名或 index 猜一个。
    }
    QString resource; bool game = false;
    const int row = resourceTable_->currentRow();
    if (row >= 0 && static_cast<std::size_t>(row) < resourceRows_.size()) {
        resource = resourceRows_[static_cast<std::size_t>(row)].path;
        resource.replace(QLatin1Char('/'), QLatin1Char('\\'));
        game = resourceRows_[static_cast<std::size_t>(row)].game;
    }
    QJsonObject bookmark{{"schema", 1}, {"kind", "sounds-view"}, {"session_identity", QString::number(selectedSessionIdentity_)},
        {"map_revision", QString::number(selectedRevision_)},
        {"resource_catalog_generation", QString::number(gameIndexGeneration_)},
        {"definition_sha256", definition ? QJsonValue(QString::fromStdString(*definition)) : QJsonValue(QJsonValue::Null)},
        {"resource_path", resource}, {"resource_game", game}, {"filter", search_->text()},
        {"definitions_v", definitionsTable_->verticalScrollBar()->value()}, {"definitions_h", definitionsTable_->horizontalScrollBar()->value()},
        {"resources_v", resourceTable_->verticalScrollBar()->value()}, {"resources_h", resourceTable_->horizontalScrollBar()->value()}};
    if (!NavigationBookmarkDetail::Text(bookmark, "resource_path") || !NavigationBookmarkDetail::Text(bookmark, "filter", 512)
        || QJsonDocument(bookmark).toJson(QJsonDocument::Compact).size() > 16 * 1024) return {};
    return bookmark;
}

bool SoundsModule::PrepareNavigationLeave() {
    if (!draftChanged_) return true;
    ShowStatus(tr("先应用或放弃当前声音属性修改，再跳转"), true);
    return false;
}

Shell::NavigationRestoreResult SoundsModule::RestoreNavigationBookmark(const QJsonObject& bookmark) {
    using Shell::NavigationRestoreResult;
    namespace Nav = NavigationBookmarkDetail;
    if (draftChanged_) { (void)PrepareNavigationLeave(); return NavigationRestoreResult::Blocked; }
    const auto session = Nav::Unsigned(bookmark, "session_identity"), revision = Nav::Unsigned(bookmark, "map_revision");
    const auto catalogGeneration = Nav::Unsigned(bookmark, "resource_catalog_generation");
    const auto resource = Nav::Text(bookmark, "resource_path"), filter = Nav::Text(bookmark, "filter", 512);
    const auto definitionsV = Nav::Scroll(bookmark, "definitions_v"), definitionsH = Nav::Scroll(bookmark, "definitions_h");
    const auto resourcesV = Nav::Scroll(bookmark, "resources_v"), resourcesH = Nav::Scroll(bookmark, "resources_h");
    const auto* edits = project_.MapEdits();
    if (!Nav::Header(bookmark, "sounds-view") || !session || !revision || !*session || !catalogGeneration || !resource || !filter
        || !definitionsV || !definitionsH || !resourcesV || !resourcesH || !edits || !page_
        || !bookmark.value("resource_game").isBool() || resource->contains(QLatin1Char('/'))
        || edits->SessionIdentity() != *session || edits->Revision() != *revision
        || selectedSessionIdentity_ != *session || selectedRevision_ != *revision
        || gameIndexGeneration_ != *catalogGeneration || gameIndexRoot_ != project_.war3GameRoot_
        || QJsonDocument(bookmark).toJson(QJsonDocument::Compact).size() > 16 * 1024) return NavigationRestoreResult::Invalid;
    if (status_) status_->setProperty("navigationIdentityFailure", QString{});
    if (visibleSounds_ && visibleSounds_->entries.size() > 4096) {
        if (status_) status_->setProperty("navigationIdentityFailure", QStringLiteral("sound-navigation-definition-count-budget"));
        ShowStatus(tr("声音定义超过4096条导航身份预算；无法恢复该位置"), true); return NavigationRestoreResult::Invalid;
    }
    std::size_t processedBytes = 0;
    DefinitionNavigationFailure failure = DefinitionNavigationFailure::None;
    int definition = -1;
    if (!bookmark.value("definition_sha256").isNull()) {
        const auto key = Nav::Text(bookmark, "definition_sha256", 64);
        if (!key || key->size() != 64 || !visibleSounds_) return NavigationRestoreResult::Invalid;
        std::size_t matches = 0;
        for (std::size_t row = 0; row < visibleSounds_->entries.size(); ++row) {
            const auto actual = DefinitionNavigationKey(visibleSounds_->entries[row], visibleSounds_->version, processedBytes, failure);
            if (!actual) {
                if (status_) status_->setProperty("navigationIdentityFailure", DefinitionNavigationFailureCode(failure));
                ShowStatus(tr("%1；无法恢复该位置").arg(DefinitionNavigationFailureText(failure)), true);
                return NavigationRestoreResult::Invalid;
            }
            if (QString::fromStdString(*actual) == *key) { ++matches; definition = static_cast<int>(row); }
        }
        if (matches != 1) return NavigationRestoreResult::Invalid;
    }
    if (!resource->isEmpty()) {
        for (const auto& entry : project_.AssetEntries())
            if (QtProject::EditorProject::AssetKey(entry.path) == QtProject::EditorProject::AssetKey(*resource) && entry.ambiguous)
                return NavigationRestoreResult::Invalid;
        const auto candidates = ResourceCatalogRows();
        const auto query = filter->trimmed();
        int visible = 0; bool found = false;
        for (const auto& row : candidates) {
            if (!query.isEmpty() && !row.path.contains(query, Qt::CaseInsensitive) && !row.source.contains(query, Qt::CaseInsensitive)) continue;
            if (visible++ >= 400) break; // 与真实列表的可见窗口相同；不返回一个未显示的目标。
            if (QtProject::EditorProject::AssetKey(row.path) != QtProject::EditorProject::AssetKey(*resource)) continue;
            if (row.game != bookmark.value("resource_game").toBool()) return NavigationRestoreResult::Invalid;
            found = true;
        }
        if (!found) return NavigationRestoreResult::Invalid;
    }
    // 完整验证后只更新视图；不调用 ApplyInspector、DiscardPendingDraft 或保存服务。
    { const QSignalBlocker block(search_); search_->setText(*filter); }
    RebuildResources();
    { const QSignalBlocker block(definitionsTable_);
        definitionsTable_->clearSelection(); definitionsTable_->setCurrentCell(-1, -1);
        if (definition >= 0) definitionsTable_->selectRow(definition);
    }
    selectedDefinition_ = definition; LoadInspector();
    resourceTable_->clearSelection(); resourceTable_->setCurrentCell(-1, -1);
    if (!resource->isEmpty()) for (std::size_t row = 0; row < resourceRows_.size(); ++row)
        if (QtProject::EditorProject::AssetKey(resourceRows_[row].path) == QtProject::EditorProject::AssetKey(*resource))
            resourceTable_->selectRow(static_cast<int>(row));
    definitionsTable_->verticalScrollBar()->setValue(*definitionsV); definitionsTable_->horizontalScrollBar()->setValue(*definitionsH);
    resourceTable_->verticalScrollBar()->setValue(*resourcesV); resourceTable_->horizontalScrollBar()->setValue(*resourcesH);
    return NavigationRestoreResult::Restored;
}

void SoundsModule::StartPreview() {
    if (previewPending_) {
        ShowStatus(tr("上一项声音仍在读取，请稍候"), true);
        return;
    }
    if (!resourceTable_ || resourceTable_->currentRow() < 0
        || static_cast<std::size_t>(resourceTable_->currentRow()) >= resourceRows_.size()) {
        ShowStatus(tr("请选择声音资源"), true);
        return;
    }
    const auto resource = resourceRows_[static_cast<std::size_t>(resourceTable_->currentRow())];
    StopPreview();
    if (!resource.diskPath.isEmpty()) {
        previewPlayer_->PlayFile(resource.diskPath);
        ShowStatus(tr("正在试听 %1").arg(resource.path));
        return;
    }
    const auto archivePath = project_.archiveSession_
        ? project_.archiveSession_->SourcePath() : std::filesystem::path{};
    const auto gameRoot = project_.war3GameRoot_;
    if ((resource.archivePath.isEmpty() || archivePath.empty())
        && (!resource.game || gameRoot.empty())) {
        ShowStatus(tr("无法定位所选声音文件"), true);
        return;
    }
    previewPending_ = true;
    const auto generation = previewGeneration_;
    auto result = std::make_shared<Result<std::vector<std::byte>>>();
    auto* worker = QThread::create([resource, archivePath, gameRoot, result] {
        if (!resource.archivePath.isEmpty() && !archivePath.empty()) {
            auto archive = MapArchiveSession::Open(archivePath);
            if (archive.Succeeded() && archive.value)
                *result = archive.value->ReadMember(resource.archivePath.toStdString());
            else result->diagnostics = std::move(archive.diagnostics);
        } else {
            auto game = Game::GameDataSource::Mount(gameRoot);
            if (game.Succeeded() && game.value)
                *result = game.value->ReadFile(resource.path.toStdString());
            else result->diagnostics = std::move(game.diagnostics);
        }
    });
    connect(worker, &QThread::finished, this, [this, generation, resource, result] {
        if (generation != previewGeneration_) return;
        previewPending_ = false;
        if (!result->Succeeded() || !result->value || result->value->size() > 64u * 1024u * 1024u) {
            ShowStatus(tr("声音资源读取失败或超过 64 MiB 试听上限"), true);
            return;
        }
        PlayBytes(std::move(*result->value), resource.path);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowestPriority);
    ShowStatus(tr("正在读取 %1…").arg(resource.path));
}

void SoundsModule::PlayBytes(std::vector<std::byte> bytes, const QString& path) {
    if (previewPlayer_->PlayBytes(bytes, path)) ShowStatus(tr("正在试听 %1").arg(path));
}

void SoundsModule::StopPreview() {
    ++previewGeneration_;
    previewPending_ = false;
    if (previewPlayer_) previewPlayer_->Stop();
}

} // namespace War3Map::Modules
