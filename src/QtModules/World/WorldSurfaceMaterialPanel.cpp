#include "WorldSurfaceMaterialPanel.h"

#include "AssetOwnedPayload.h"
#include "War3MapCore/Hash.h"
#include "Icons/SvgIcons.h"
#include "Widgets/SearchField.h"
#include "Widgets/TablePopupAnchor.h"
#include "WorldPanelWidgets.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <span>

namespace War3Map::Modules {
namespace {
using Render::SurfaceMaterialKeyKind;
using Render::kSurfaceMaterialKeys;
using QtProject::EditorProject;

constexpr int kSliderSteps = 1000;
constexpr int kRawcodeRole = Qt::UserRole;
// 图片按作者原文件名导入到这个目录（与 Agent 命令文档建议的位置一致）。
constexpr auto kImportFolder = "WarVK\\Materials\\";
enum Filter { All, WithMaterial, WithProblems, WithoutMaterial };

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
std::size_t KeyIndex(std::string_view name) {
    return static_cast<std::size_t>(Render::FindSurfaceMaterialKey(name) - kSurfaceMaterialKeys.data());
}
bool LogScale(const Render::SurfaceMaterialKey& key) { return key.name == "detailScale"; }
QString Category(Services::SurfaceMaterialTypeCategory category) {
    using Kind = Services::SurfaceMaterialTypeCategory;
    switch (category) {
    case Kind::Doodad: return QObject::tr("装饰物");
    case Kind::Destructible: return QObject::tr("可破坏物");
    case Kind::Ambiguous: return QObject::tr("装饰物与可破坏物同码");
    case Kind::Unknown: break;
    }
    return QObject::tr("未知类型");
}
QString Shader(const std::optional<Services::SurfaceMaterialSection>& section) {
    if (!section) return {};
    // 表面着色没有图片时 WarVK 与编辑器都忽略整节。
    if (!Services::SurfaceMaterialSectionApplies(*section)) return QObject::tr("不生效");
    const auto& value = section->values[KeyIndex("shader")];
    return value && *value == "water" ? QObject::tr("水面") : QObject::tr("表面");
}
// 一个类型的全部诊断：类型级与图片级，加上表级里点名这一类型的（同名节、没有图片等）。列表计数与详情共用。
std::vector<const Diagnostic*> TypeDiagnostics(const Services::SurfaceMaterialValidation& validation, std::string_view code) {
    std::vector<const Diagnostic*> items;
    for (const auto& type : validation.types)
        if (type.rawcode == code) for (const auto& item : type.diagnostics) items.push_back(&item);
    const auto tag = "[" + std::string(code) + "]";
    for (const auto& item : validation.table)
        if (item.message.find(tag) != std::string::npos) items.push_back(&item);
    return items;
}
void SetWritten(QWidget* widget, bool written) {
    const auto current = widget ? widget->property("materialWritten") : QVariant();
    if (!widget || (current.isValid() && current.toBool() == written)) return;
    widget->setProperty("materialWritten", written);
    WorldUi::Repolish(widget);
}
// 导入名：已只含 ASCII 字母、数字和 _ - . 时原样用。否则其余字符换成“_”，并在主名后加内容哈希前 8 位：
// 同长的中文名不会撞成同一个名字（撞名会改掉别的类型在用的图），同一份内容仍得到同一个名字。
QString AsciiFileName(const QString& name, std::span<const std::byte> bytes) {
    const auto safe = [](QChar c) {
        const auto u = c.unicode();
        return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || u == '_' || u == '-' || u == '.';
    };
    if (!name.isEmpty() && !name.startsWith(QLatin1Char('.')) && std::all_of(name.begin(), name.end(), safe)) return name;
    QString out;
    for (const QChar c : name) out += safe(c) ? c : QLatin1Char('_');
    const auto dot = out.lastIndexOf(QLatin1Char('.'));
    QString base = dot > 0 ? out.left(dot) : (dot == 0 ? QString() : out);
    const QString suffix = dot >= 0 ? out.mid(dot) : QString();
    while (base.startsWith(QLatin1Char('_'))) base.remove(0, 1);
    while (base.endsWith(QLatin1Char('_'))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("image");
    const auto hash = Sha256(bytes);
    const auto tag = hash.Succeeded() && hash.value ? QString::fromStdString(hash.value->substr(0, 8)) : QStringLiteral("00000000");
    return base + QLatin1Char('_') + tag + suffix;
}
QToolButton* IconButton(QWidget* parent, const char* icon, const QString& tip, const QString& name) {
    auto* button = new QToolButton(parent);
    button->setIcon(UiKit::SvgIcon(QString::fromLatin1(icon)));
    button->setToolTip(tip); button->setAccessibleName(tip);
    button->setObjectName(name); button->setAutoRaise(true);
    return button;
}
} // namespace

WorldSurfaceMaterialPanel::WorldSurfaceMaterialPanel(EditorProject& project, QWidget* parent)
    : QWidget(parent), project_(project), alive_(std::make_shared<bool>(true)) {
    setObjectName(QStringLiteral("worldSurfaceMaterialPanel"));
    setProperty("worldPanel", true);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(10, 10, 10, 10); column->setSpacing(8);

    auto* filterRow = new QHBoxLayout; filterRow->setSpacing(6);
    search_ = new QLineEdit(this); search_->setObjectName(QStringLiteral("worldMaterialSearch"));
    search_->setPlaceholderText(tr("搜索名称或四字码")); search_->setClearButtonEnabled(true);
    UiKit::InstallSearchIcon(search_);
    filter_ = new QComboBox(this); filter_->setObjectName(QStringLiteral("worldMaterialFilter"));
    filter_->addItem(tr("全部类型"), All); filter_->addItem(tr("有材质"), WithMaterial);
    filter_->addItem(tr("有问题"), WithProblems); filter_->addItem(tr("没有材质"), WithoutMaterial);
    filterRow->addWidget(search_, 1); filterRow->addWidget(filter_);
    column->addLayout(filterRow);

    auto* split = new QSplitter(Qt::Vertical, this); split->setObjectName(QStringLiteral("worldMaterialSplit"));
    split->setChildrenCollapsible(false);
    types_ = new QTreeWidget(split); types_->setObjectName(QStringLiteral("worldMaterialTypes"));
    types_->setColumnCount(4); types_->setHeaderLabels({tr("类型"), tr("摆放"), tr("材质"), tr("问题")});
    types_->setRootIsDecorated(false); types_->setUniformRowHeights(true); types_->setAlternatingRowColors(true);
    types_->setSortingEnabled(true); types_->sortByColumn(0, Qt::AscendingOrder);
    types_->header()->setSectionResizeMode(QHeaderView::Interactive); types_->header()->setStretchLastSection(true);
    types_->setColumnWidth(0, 150); types_->setColumnWidth(1, 44); types_->setColumnWidth(2, 44);
    split->addWidget(types_);

    auto* scroll = new QScrollArea(split); scroll->setObjectName(QStringLiteral("worldMaterialEditorScroll"));
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor_ = new QWidget; editor_->setObjectName(QStringLiteral("worldMaterialEditor")); editor_->setProperty("worldPanel", true);
    auto* body = new QVBoxLayout(editor_); body->setContentsMargins(0, 4, 0, 4); body->setSpacing(8);
    title_ = new QLabel(editor_); title_->setObjectName(QStringLiteral("worldPanelTitle")); title_->setWordWrap(true);
    title_->setTextFormat(Qt::PlainText);
    meta_ = new QLabel(editor_); meta_->setObjectName(QStringLiteral("worldPanelSubtitle")); meta_->setWordWrap(true);
    meta_->setTextFormat(Qt::PlainText);
    body->addWidget(title_); body->addWidget(meta_);

    auto* grid = new QGridLayout; grid->setHorizontalSpacing(6); grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
    int row = 0;
    const auto section = [&](const QString& text) {
        grid->addWidget(WorldUi::SectionLabel(text, editor_), row++, 0, 1, 3);
    };
    for (std::size_t i = 0; i < kSurfaceMaterialKeys.size(); ++i) {
        const auto& key = kSurfaceMaterialKeys[i];
        if (key.name == "shader") section(tr("着色"));
        else if (key.name == "normal") section(tr("图片"));
        else if (key.name == "normalY") section(tr("参数"));
        auto& field = fields_[i];
        const auto name = Text(key.name);
        field.label = new QLabel(Text(key.label), editor_);
        field.label->setObjectName(QStringLiteral("worldFieldLabel")); field.label->setWordWrap(true);
        field.label->setToolTip(name);
        field.reset = IconButton(editor_, "lucide/rotate-ccw", tr("删去 %1（用默认值）").arg(name),
            QStringLiteral("worldMaterialReset_") + name);
        connect(field.reset, &QToolButton::clicked, this, [this, i] { SetValue(current_, i, std::nullopt); });
        auto* value = new QWidget(editor_);
        auto* line = new QHBoxLayout(value); line->setContentsMargins(0, 0, 0, 0); line->setSpacing(4);
        if (key.kind == SurfaceMaterialKeyKind::Image) {
            field.path = new QLineEdit(value); field.path->setObjectName(QStringLiteral("worldMaterialPath"));
            field.path->setReadOnly(true); field.path->setAccessibleName(Text(key.label));
            line->addWidget(field.path, 1);
            auto* choose = IconButton(value, "lucide/folder-open", tr("从地图或游戏文件里选"), QStringLiteral("worldMaterialChoose_") + name);
            auto* import = IconButton(value, "lucide/file-plus-2", tr("导入本机图片"), QStringLiteral("worldMaterialImport_") + name);
            line->addWidget(choose); line->addWidget(import);
            connect(choose, &QToolButton::clicked, this, [this, i] { ChooseImage(i); });
            connect(import, &QToolButton::clicked, this, [this, i] { ImportImage(i); });
            field.reset->setIcon(UiKit::SvgIcon(QStringLiteral("lucide/x")));
            field.reset->setToolTip(tr("清除 %1").arg(name)); field.reset->setAccessibleName(field.reset->toolTip());
        } else if (key.kind == SurfaceMaterialKeyKind::Choice) {
            field.choice = new QButtonGroup(value);
            line->addWidget(WorldUi::Segment(value, field.choice,
                {{key.name == "shader" ? tr("表面") : QStringLiteral("OpenGL"), 0},
                 {key.name == "shader" ? tr("水面") : QStringLiteral("DirectX"), 1}},
                key.name == "shader" ? "worldMaterialShader" : "worldMaterialNormalY"), 1);
            connect(field.choice, &QButtonGroup::idClicked, this, [this, i](int id) {
                // 点的就是当前生效值（含没写时的默认值）时不写：免得多一步撤销或凭空新建一节。
                const auto& key = kSurfaceMaterialKeys[i];
                const auto section = CurrentSection();
                const auto& written = section ? section->values[i] : std::optional<std::string>{};
                if ((written && *written == key.choices[1]) == (id == 1)) { ShowType(); return; }
                SetValue(current_, i, Text(key.choices[id == 1 ? 1 : 0]));
            });
        } else {
            field.slider = new QSlider(Qt::Horizontal, value); field.slider->setObjectName(QStringLiteral("worldMaterialSlider_") + name);
            field.slider->setRange(0, kSliderSteps); field.slider->setAccessibleName(Text(key.label));
            field.spin = new QDoubleSpinBox(value); field.spin->setObjectName(QStringLiteral("worldMaterialValue_") + name);
            field.spin->setRange(key.minimum, key.maximum); field.spin->setKeyboardTracking(false);
            field.spin->setDecimals(LogScale(key) ? 1 : 3);
            field.spin->setSingleStep(LogScale(key) ? 8.0 : (key.maximum - key.minimum) / 100.0);
            field.spin->setAccessibleName(Text(key.label));
            line->addWidget(field.slider, 1); line->addWidget(field.spin);
            // 拖动中只跟着显示数值，松手（或键盘、点击槽一次）才写入一步。
            connect(field.slider, &QSlider::valueChanged, this, [this, i](int position) {
                if (syncing_) return;
                syncing_ = true; fields_[i].spin->setValue(SliderValue(i, position)); syncing_ = false;
                if (!fields_[i].slider->isSliderDown()) CommitNumber(i);
            });
            connect(field.slider, &QSlider::sliderReleased, this, [this, i] { CommitNumber(i); });
            connect(field.spin, &QDoubleSpinBox::valueChanged, this, [this, i](double number) {
                if (syncing_) return;
                syncing_ = true; fields_[i].slider->setValue(SliderPosition(i, number)); syncing_ = false;
                CommitNumber(i);
            });
        }
        grid->addWidget(field.label, row, 0);
        grid->addWidget(value, row, 1);
        grid->addWidget(field.reset, row++, 2);
    }
    body->addLayout(grid);
    body->addWidget(WorldUi::SectionLabel(tr("问题"), editor_));
    diagnostics_ = new QListWidget(editor_); diagnostics_->setObjectName(QStringLiteral("worldMaterialDiagnostics"));
    diagnostics_->setWordWrap(true); diagnostics_->setMinimumHeight(72);
    body->addWidget(diagnostics_);
    message_ = new QLabel(editor_); message_->setObjectName(QStringLiteral("worldMaterialMessage"));
    message_->setWordWrap(true); message_->setTextFormat(Qt::PlainText); message_->hide();
    body->addWidget(message_);
    remove_ = new QPushButton(UiKit::SvgIcon(QStringLiteral("lucide/trash-2")), tr("删除这一类型的材质"), editor_);
    remove_->setObjectName(QStringLiteral("worldMaterialRemove"));
    connect(remove_, &QPushButton::clicked, this, &WorldSurfaceMaterialPanel::RemoveCurrent);
    body->addWidget(remove_, 0, Qt::AlignLeft);
    body->addStretch(1);
    scroll->setWidget(editor_);
    split->addWidget(scroll);
    split->setStretchFactor(0, 2); split->setStretchFactor(1, 3);
    column->addWidget(split, 1);

    reloadTimer_ = new QTimer(this); reloadTimer_->setSingleShot(true); reloadTimer_->setInterval(0);
    connect(reloadTimer_, &QTimer::timeout, this, [this] { if (isVisible() && dirty_) Reload(); });
    connect(search_, &QLineEdit::textChanged, this, &WorldSurfaceMaterialPanel::RebuildList);
    connect(filter_, &QComboBox::currentIndexChanged, this, &WorldSurfaceMaterialPanel::RebuildList);
    connect(types_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (syncing_ || !item) return;
        current_ = item->data(0, kRawcodeRole).toString();
        message_->hide();
        ShowType();
    });
    // 表、图片、摆放或物编名称变化后重读；隐藏时只记下，显示时再读。
    connect(&project_, &EditorProject::ImportsChanged, this, &WorldSurfaceMaterialPanel::Invalidate);
    connect(&project_, &EditorProject::MapDocumentChanged, this, &WorldSurfaceMaterialPanel::Invalidate);
    connect(&project_, &EditorProject::ObjectDocumentsReplaced, this, &WorldSurfaceMaterialPanel::Invalidate);
    for (auto* stack : project_.objectUndoStacks_)
        if (stack) connect(stack, &QUndoStack::indexChanged, this, &WorldSurfaceMaterialPanel::Invalidate);
    if (project_.mapDataUndoStack_) connect(project_.mapDataUndoStack_, &QUndoStack::indexChanged, this, &WorldSurfaceMaterialPanel::Invalidate);
    ShowType();
}

WorldSurfaceMaterialPanel::~WorldSurfaceMaterialPanel() { *alive_ = false; }

void WorldSurfaceMaterialPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (dirty_) reloadTimer_->start();
}

void WorldSurfaceMaterialPanel::Invalidate() {
    dirty_ = true;
    if (isVisible()) reloadTimer_->start();
}

void WorldSurfaceMaterialPanel::FollowType(const QString& rawcode) {
    if (rawcode.isEmpty() || rawcode == current_) return;
    for (int i = 0; i < types_->topLevelItemCount(); ++i)
        if (types_->topLevelItem(i)->data(0, kRawcodeRole).toString() == rawcode) { types_->setCurrentItem(types_->topLevelItem(i)); return; }
    // 还没读过表（面板没显示过）时先记下，读表后列表自动选中它。
    if (!snapshot_) { current_ = rawcode; return; }
    // 列表筛掉了它时也切过去（编辑区照常显示），筛选不变。
    if (snapshot_->types.contains(rawcode.toStdString())) { current_ = rawcode; types_->setCurrentItem(nullptr); ShowType(); }
}

void WorldSurfaceMaterialPanel::Reload() {
    dirty_ = false;
    auto table = QtProject::ReadSurfaceMaterialTable(project_);
    validation_.reset();
    if (!table.Succeeded() || !table.value) {
        state_.reset(); snapshot_.reset();
        RebuildList(); ShowType();
        ShowMessage(table.diagnostics.empty() ? tr("读不出材质表。") : Text(table.diagnostics.front().message), true);
        return;
    }
    state_ = std::move(*table.value);
    snapshot_ = QtProject::CaptureSurfaceMaterialSnapshot(project_, state_->document);
    RebuildList(); ShowType();
    StartValidation();
}

void WorldSurfaceMaterialPanel::StartValidation() {
    const auto generation = ++generation_;
    auto snapshot = snapshot_;
    if (!snapshot) return;
    auto result = std::make_shared<Services::SurfaceMaterialValidation>();
    auto* worker = QThread::create([snapshot, result] {
        *result = Services::ValidateSurfaceMaterials(snapshot->document, {},
            snapshot->Sources(&QtProject::SharedSurfaceMaterialImageCache()));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, alive = alive_, generation, snapshot, result] {
        if (!*alive || generation != generation_ || snapshot != snapshot_) return;
        validation_ = result;
        RebuildList(); ShowDiagnostics();
    });
    worker->start(QThread::LowPriority);
}

std::optional<Services::SurfaceMaterialSection> WorldSurfaceMaterialPanel::CurrentSection() const {
    if (!state_ || current_.isEmpty()) return std::nullopt;
    return state_->document.Section(current_.toStdString());
}

void WorldSurfaceMaterialPanel::RebuildList() {
    const bool wasSyncing = syncing_;
    syncing_ = true;
    types_->setSortingEnabled(false);
    types_->clear();
    if (snapshot_) {
        const auto sections = snapshot_->document.Sections();
        const auto search = search_->text().trimmed();
        const auto filter = filter_->currentData().toInt();
        QTreeWidgetItem* selected = nullptr;
        for (const auto& [code, info] : snapshot_->types) {
            if (!Render::SurfaceMaterialRawcode(code)) continue;
            const auto found = std::find_if(sections.begin(), sections.end(), [&](const auto& item) { return item.rawcode == code; });
            const std::optional<Services::SurfaceMaterialSection> section = found != sections.end()
                ? std::optional(*found) : std::nullopt;
            std::size_t errors = 0, warnings = 0;
            if (validation_)
                for (const auto* item : TypeDiagnostics(*validation_, code)) {
                    errors += item->severity == DiagnosticSeverity::Error;
                    warnings += item->severity == DiagnosticSeverity::Warning;
                }
            if ((filter == WithMaterial && !section) || (filter == WithoutMaterial && section)
                || (filter == WithProblems && errors + warnings == 0)) continue;
            const auto rawcode = Text(code), name = Text(info.name);
            if (!search.isEmpty() && !rawcode.contains(search, Qt::CaseInsensitive) && !name.contains(search, Qt::CaseInsensitive)) continue;
            auto* item = new QTreeWidgetItem(types_);
            item->setText(0, name.isEmpty() ? rawcode : QStringLiteral("%1 [%2]").arg(name, rawcode));
            item->setData(0, kRawcodeRole, rawcode);
            item->setToolTip(0, QStringLiteral("%1 · %2").arg(rawcode, Category(info.category)));
            item->setData(1, Qt::DisplayRole, static_cast<qulonglong>(info.placements));
            item->setText(2, Shader(section));
            QStringList problems;
            if (errors) problems.push_back(tr("%1 错").arg(errors));
            if (warnings) problems.push_back(tr("%1 提醒").arg(warnings));
            item->setText(3, section && !validation_ ? tr("校验中") : problems.join(QStringLiteral(" · ")));
            if (errors) item->setIcon(3, UiKit::SvgIcon(QStringLiteral("lucide/circle-x")));
            if (rawcode == current_) selected = item;
        }
        types_->setSortingEnabled(true);
        if (selected) types_->setCurrentItem(selected);
    } else {
        types_->setSortingEnabled(true);
    }
    syncing_ = wasSyncing;
}

void WorldSurfaceMaterialPanel::ShowType() {
    const bool wasSyncing = syncing_;
    syncing_ = true;
    const auto section = CurrentSection();
    const bool usable = state_ && !current_.isEmpty();
    editor_->setEnabled(usable);
    Services::SurfaceMaterialTypeInfo info;
    if (snapshot_ && !current_.isEmpty())
        if (const auto found = snapshot_->types.find(current_.toStdString()); found != snapshot_->types.end()) info = found->second;
    title_->setText(current_.isEmpty() ? tr("未选择类型")
        : info.name.empty() ? current_ : QStringLiteral("%1 [%2]").arg(Text(info.name), current_));
    QStringList meta;
    if (!current_.isEmpty()) {
        meta.push_back(Category(info.category));
        meta.push_back(tr("%1 个摆放").arg(info.placements));
        meta.push_back(section ? (section->headers > 1 ? tr("表中 %1 节（合并）").arg(section->headers) : tr("表中第 %1 行").arg(section->line))
            : tr("表里没有"));
        if (section && !section->unknownKeys.empty()) {
            QStringList keys;
            for (const auto& key : section->unknownKeys) keys.push_back(Text(key));
            meta.push_back(tr("未知键：%1").arg(keys.join(QStringLiteral("、"))));
        }
    }
    meta_->setText(meta.join(QStringLiteral(" · ")));
    for (std::size_t i = 0; i < kSurfaceMaterialKeys.size(); ++i) {
        const auto& key = kSurfaceMaterialKeys[i];
        auto& field = fields_[i];
        const auto& value = section ? section->values[i] : std::optional<std::string>{};
        const bool written = value.has_value();
        const bool invalid = section && section->invalid[i];
        SetWritten(field.label, written);
        field.reset->setEnabled(written);
        field.label->setToolTip(invalid ? tr("%1：表里最后写的值无效，保留之前的有效值（没有时用默认值）").arg(Text(key.name))
            : Text(key.name));
        if (field.path) {
            field.path->setText(written ? Text(*value) : QString());
            field.path->setToolTip(field.path->text());
            SetWritten(field.path, written);
        } else if (field.choice) {
            const int id = value && *value == key.choices[1] ? 1 : 0;
            if (auto* button = field.choice->button(id)) button->setChecked(true);
        } else {
            const auto parsed = value ? Render::ParseSurfaceMaterialNumber(*value, key) : std::nullopt;
            const double number = parsed ? *parsed : key.defaultValue;
            field.spin->setValue(number);
            field.slider->setValue(SliderPosition(i, number));
            field.spin->setToolTip(written ? QString() : tr("默认 %1").arg(key.defaultValue));
            SetWritten(field.spin, written && parsed);
        }
    }
    remove_->setEnabled(section.has_value());
    ShowDiagnostics();
    syncing_ = wasSyncing;
}

void WorldSurfaceMaterialPanel::ShowDiagnostics() {
    diagnostics_->clear();
    if (current_.isEmpty() || !state_) return;
    if (!validation_) { diagnostics_->addItem(tr("校验中…")); return; }
    for (const auto* item : TypeDiagnostics(*validation_, current_.toStdString())) {
        auto* row = new QListWidgetItem(Text(item->message), diagnostics_);
        row->setToolTip(Text(item->code));
        const char* icon = item->severity == DiagnosticSeverity::Error ? "lucide/circle-x"
            : item->severity == DiagnosticSeverity::Warning ? "lucide/triangle-alert" : "lucide/info";
        row->setIcon(UiKit::SvgIcon(QString::fromLatin1(icon)));
    }
}

bool WorldSurfaceMaterialPanel::Commit(QString rawcode, const QString& label,
    const std::function<Status(Services::SurfaceMaterialTableDocument&)>& change,
    std::vector<EditorProject::ImportPayload> images, bool replaceImages) {
    if (!state_ || rawcode.isEmpty()) return false;
    // 改动建立在此刻读到的表上；确认框期间表可能被 Agent 改掉并重读，所以核对用的哈希先记下来，
    // 写入时对不上就拒绝，不覆盖别人的修改。
    const auto base = state_->document;
    const auto expected = state_->sha256;
    auto document = base;
    if (const auto status = change(document); !status.Succeeded()) {
        ShowMessage(status.diagnostics.empty() ? tr("改不了。") : Text(status.diagnostics.front().message), true);
        ShowType();
        return false;
    }
    if (document.Text() == base.Text() && images.empty()) return true;
    // 删去最后一张图会让这一类型整节失效：提交前说清楚，作者确认后才写。
    if (const auto stops = Services::SurfaceMaterialStopsApplying(base, document, rawcode.toStdString());
        stops && QMessageBox::question(this, tr("材质将不再生效"), Text(stops->message) + tr("\n\n仍然修改？"))
            != QMessageBox::Yes) {
        ShowType();
        return false;
    }
    const auto rejected = QtProject::CommitSurfaceMaterialTable(project_, expected, document, std::move(images),
        replaceImages, label);
    if (!rejected.isEmpty()) { ShowMessage(rejected, true); Reload(); return false; }
    message_->hide();
    // 提交会发出资源变化并标记重读；面板正显示时马上重读，保持控件与表一致。
    if (dirty_) Reload();
    return true;
}

bool WorldSurfaceMaterialPanel::SetValue(QString rawcode, std::size_t key, const std::optional<QString>& value) {
    const auto name = std::string(kSurfaceMaterialKeys[key].name);
    const auto code = rawcode.toStdString();
    return Commit(rawcode, value ? tr("修改材质 %1 的%2").arg(rawcode, Text(kSurfaceMaterialKeys[key].label))
                 : tr("删去材质 %1 的%2").arg(rawcode, Text(kSurfaceMaterialKeys[key].label)),
        [&](Services::SurfaceMaterialTableDocument& document) {
            return value ? document.Set(code, name, value->toStdString()) : document.Unset(code, name);
        });
}

void WorldSurfaceMaterialPanel::CommitNumber(std::size_t key) {
    if (syncing_ || !fields_[key].spin) return;
    const auto& definition = kSurfaceMaterialKeys[key];
    const double number = fields_[key].spin->value();
    const auto section = CurrentSection();
    const auto& written = section ? section->values[key] : std::optional<std::string>{};
    const auto current = written ? Render::ParseSurfaceMaterialNumber(*written, definition) : std::nullopt;
    const double before = current ? *current : definition.defaultValue;
    // 与当前生效值相同（含未写时等于默认值）不写，避免拖回原处也多一步撤销。
    if (std::abs(number - before) <= 1e-6 * std::max(1.0, std::abs(before))) { ShowType(); return; }
    SetValue(current_, key, QString::number(number, 'g', 6));
}

void WorldSurfaceMaterialPanel::ChooseImage(std::size_t key) {
    if (!pickerFactory_ || current_.isEmpty()) return;
    auto* anchor = fields_[key].path;
    auto* picker = new ObjectResourcePicker(Services::ObjectResourceField{Services::ObjectResourceKind::MaterialImage, false, true},
        anchor->text(), pickerFactory_(), this);
    picker->setObjectName(QStringLiteral("worldMaterialImagePicker"));
    picker->BrowseImports();
    // 写给打开选择器时的那一类型（选择期间面板换了类型也不会写错地方）。
    const auto code = current_;
    picker->SetCommit([this, key, code, alive = alive_](const QString& value) {
        if (!*alive) return false;
        if (value.trimmed().isEmpty()) return SetValue(code, key, std::nullopt);
        QString path = value.trimmed(); path.replace(QLatin1Char('/'), QLatin1Char('\\'));
        return SetValue(code, key, path);
    });
    picker->show();
    UiKit::AnchorFieldPopup(picker, anchor);
    picker->raise();
}

void WorldSurfaceMaterialPanel::ImportImage(std::size_t key) {
    // 写给点“导入”时的那一类型：文件对话框期间面板换了类型也不会写错地方。
    const auto rawcode = current_;
    if (rawcode.isEmpty()) return;
    const auto disk = QFileDialog::getOpenFileName(this, tr("导入材质图片"), {},
        tr("材质图片 (*.png *.jpg *.jpeg *.bmp *.tga)"));
    if (disk.isEmpty()) return;
    if (!Render::SurfaceMaterialImageExtensionAccepted(disk.toStdString())) {
        ShowMessage(tr("游戏一侧只拷 .png .jpg .jpeg .bmp .tga 图片。"), true);
        return;
    }
    auto payload = QtProject::ReadDiskImportPayload(disk, {});
    if (!payload.Succeeded() || !payload.value) {
        ShowMessage(payload.diagnostics.empty() ? tr("读不了这个文件。") : Text(payload.diagnostics.front().message), true);
        return;
    }
    // 游戏一侧按表里的原始字节找文件，1.27a 对非 ASCII 名没有验证过：导入时改成 ASCII 名（带内容哈希，不会撞名）。
    const auto original = QFileInfo(disk).fileName();
    const auto fileName = AsciiFileName(original,
        std::span<const std::byte>(payload.value->bytes->data(), payload.value->bytes->size()));
    const auto archive = QLatin1String(kImportFolder) + fileName;
    payload.value->archivePath = archive;
    bool replace = false;
    const auto existing = QtProject::ReadSurfaceMaterialImage(project_, archive.toStdString());
    if (existing.bytes && *existing.bytes == *payload.value->bytes) {
        SetValue(rawcode, key, archive); // 地图里已有同一份文件：只改引用，不重复导入。
        return;
    }
    if (existing.state != Services::SurfaceMaterialImageSource::State::Missing) {
        if (QMessageBox::question(this, tr("导入材质图片"), tr("地图里已有 %1，用这次选的文件替换它？").arg(archive))
            != QMessageBox::Yes) return;
        replace = true;
    }
    const auto name = std::string(kSurfaceMaterialKeys[key].name);
    const auto code = rawcode.toStdString();
    std::vector<EditorProject::ImportPayload> images;
    images.push_back(std::move(*payload.value));
    const bool done = Commit(rawcode, tr("导入材质 %1 的%2").arg(rawcode, Text(kSurfaceMaterialKeys[key].label)),
        [&](Services::SurfaceMaterialTableDocument& document) { return document.Set(code, name, archive.toStdString()); },
        std::move(images), replace);
    if (done && fileName != original) ShowMessage(tr("已改名为 %1 导入（游戏里非 ASCII 文件名没有验证过）。").arg(fileName), false);
}

void WorldSurfaceMaterialPanel::RemoveCurrent() {
    if (!CurrentSection()) return;
    const auto rawcode = current_;
    const auto code = rawcode.toStdString();
    Commit(rawcode, tr("删除材质 %1").arg(rawcode),
        [&](Services::SurfaceMaterialTableDocument& document) { return document.RemoveSection(code); });
}

double WorldSurfaceMaterialPanel::SliderValue(std::size_t key, int position) const {
    const auto& definition = kSurfaceMaterialKeys[key];
    const double t = std::clamp(position, 0, kSliderSteps) / static_cast<double>(kSliderSteps);
    if (LogScale(definition)) return definition.minimum * std::pow(definition.maximum / definition.minimum, t);
    return definition.minimum + t * (definition.maximum - definition.minimum);
}

int WorldSurfaceMaterialPanel::SliderPosition(std::size_t key, double value) const {
    const auto& definition = kSurfaceMaterialKeys[key];
    value = std::clamp(value, static_cast<double>(definition.minimum), static_cast<double>(definition.maximum));
    const double t = LogScale(definition)
        ? std::log(value / definition.minimum) / std::log(definition.maximum / definition.minimum)
        : (value - definition.minimum) / (definition.maximum - definition.minimum);
    return static_cast<int>(std::lround(t * kSliderSteps));
}

void WorldSurfaceMaterialPanel::ShowMessage(const QString& text, bool error) {
    message_->setText(text);
    message_->setProperty("error", error);
    WorldUi::Repolish(message_);
    message_->setVisible(!text.isEmpty());
}
} // namespace War3Map::Modules
