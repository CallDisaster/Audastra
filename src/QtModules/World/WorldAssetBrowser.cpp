#include "WorldAssetBrowser.h"

#include "EditorProject.h"
#include "Icons/IconProvider.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Workspace.h"

#include <QApplication>
#include <QComboBox>
#include <QDrag>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace War3Map::Modules {
namespace {
constexpr int kObjectIndexRole = Qt::UserRole + 1;
constexpr int kSectionRole = Qt::UserRole + 2;
constexpr int kNameRole = Qt::UserRole + 3;
const QStringList& Sections() {
    static const QStringList values{QObject::tr("单位"), QObject::tr("英雄"), QObject::tr("建筑"), QObject::tr("特殊"), QObject::tr("分类未确认")};
    return values;
}
bool Decimal(const QJsonValue& value, std::uint64_t& output) {
    if (!value.isString()) return false;
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 20) return false;
    for (const auto ch : text) if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) return false;
    bool ok = false; output = text.toULongLong(&ok); return ok;
}
QByteArray DragBytes(const WorldAssetBinding& binding, const ObjectData::ObjectKey& key) {
    return QJsonDocument(QJsonObject{{"schema", "forge-world-asset-v1"}, {"mapKey", binding.mapKey},
        {"generation", QString::number(binding.generation)}, {"revision", QString::number(binding.revision)},
        {"type", static_cast<int>(key.type)}, {"rawcode", QString::fromStdString(key.rawcode)}}).toJson(QJsonDocument::Compact);
}
bool NonCatalogMapScope(std::string_view scope) {
    if (!scope.starts_with("map/")) return false;
    std::string name(scope.substr(4));
    for (auto& ch : name) {
        if (ch == '\\') ch = '/';
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (name.starts_with("map/")) name.erase(0, 4);
    // WTS 与 W3I（数据集/tileset）必须参与目录身份；未知范围也不自动略过。
    // 地形草稿的来源/可重放性和通行意图只影响工程保存，不改变物编目录或文件列表。
    // 仅排除已知内部范围，不能把未来未知editor范围也当作无关。
    constexpr std::array<std::string_view, 10> names{"war3mapunits.doo", "war3map.doo", "war3map.w3r",
        "war3map.w3c", "war3map.w3s", "war3map.w3e", "war3map.wpm", "editor/terrain-journal",
        "editor/terrain-journal-warning", "editor/pathing-restrictions"};
    return std::find(names.begin(), names.end(), name) != names.end();
}
} // namespace

struct WorldAssetBrowser::ContentContext {
    EditScopeSnapshot scopes;
    std::uint64_t sessionGeneration = 0;
    const MapData::MapDataEditSession* mapEdits = nullptr;
    const MapDocument* mapDocument = nullptr;
    const MapArchiveSession* archive = nullptr;
    const ObjectData::StandardObjectCatalog* catalog = nullptr;
    const ObjectData::StandardObjectEntry* catalogEntries = nullptr;
    std::size_t catalogSize = 0;
    std::vector<std::string> catalogFiles, uiLayers;
    std::shared_ptr<const Game::WorldEditStrings> strings;
    std::array<std::filesystem::path, 7> roots;
    std::array<const void*, 7> objects{}, nativeObjects{};
    std::string uiSha;
    std::optional<std::uint8_t> mainTileset;
    [[nodiscard]] bool operator==(const ContentContext&) const = default;
};

std::shared_ptr<const WorldAssetBrowser::ContentContext> WorldAssetBrowser::CaptureContentContext(const QtProject::EditorProject& project) {
    if (!project.editTrackingReady_ || project.editTrackingFailed_ || project.sourceChangeSuspected_ || project.sourceChangeConfirmed_)
        return {};
    auto scopes = project.editRevision_.Current();
    const auto source = scopes.find("source");
    if (source == scopes.end() || source->second.empty()) return {};
    if (project.archiveSession_) {
        if (source->second != "archive:" + project.archiveSession_->SourceSha256()) return {};
    } else if (!project.directorySnapshot_ || source->second != "directory:" + project.directorySnapshot_->Snapshot().aggregateSha256) return {};
    auto result = std::make_shared<ContentContext>();
    result->sessionGeneration = project.editRevision_.Generation();
    for (auto& [scope, fingerprint] : scopes) {
        if (fingerprint.empty()) return {};
        if (!NonCatalogMapScope(scope)) result->scopes.emplace(scope, std::move(fingerprint));
    }
    result->mapEdits = project.mapEdits_.get(); result->mapDocument = project.mapDocument_.get(); result->archive = project.archiveSession_.get();
    result->catalog = project.standardCatalog_.get();
    if (result->catalog) {
        // StandardObjectCatalog 是公开 const 查询的加载快照；正常替换仍须发 Refresh 通知。
        const auto entries = result->catalog->Entries();
        result->catalogEntries = entries.data(); result->catalogSize = entries.size();
        result->catalogFiles = result->catalog->LoadedFiles();
    }
    if (project.uiCatalog_) { result->uiSha = project.uiCatalog_->Sha256(); result->uiLayers = project.uiCatalog_->Layers(); }
    result->strings = project.worldEditStrings_;
    result->roots = {project.UiToolRoot(), project.uiDataRoot_, project.activeUiDataRoot_, project.compilerDataRoot_,
        project.war3GameRoot_, project.extraIconDir_, project.projectRoot_};
    for (std::size_t i = 0; i < result->objects.size(); ++i) {
        result->objects[i] = project.objectDocuments_[i].get(); result->nativeObjects[i] = project.nativeObjectDocuments_[i].get();
    }
    if (project.mapEdits_ && project.mapEdits_->Terrain()) result->mainTileset = project.mapEdits_->Terrain()->header.mainTileset;
    // 这是已载入内容/正常 Observe 与通知的身份，不是外部文件的原子长租约。
    return result;
}

WorldAssetBrowser::WorldAssetBrowser(QtApp::IconProvider* icons, const War3RenderPluginApiV2* backend, QWidget* parent, bool loadProductionBackend)
    : QWidget(parent), icons_(icons) {
    // 世界资源目录用游戏头像；模型完成帧仍属于单独的选中对象预览。
    (void)backend; (void)loadProductionBackend;
    setObjectName(QStringLiteral("worldAssetBrowser"));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(8, 4, 8, 4); layout->setSpacing(4);
    auto* header = new QHBoxLayout; collapse_ = new QToolButton(this);
    collapse_->setObjectName(QStringLiteral("worldAssetCollapse")); collapse_->setText(tr("资源浏览器"));
    collapse_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); collapse_->setArrowType(Qt::DownArrow);
    collapse_->setCheckable(true); header->addWidget(collapse_);
    pages_ = new QTabBar(this); pages_->setObjectName(QStringLiteral("worldAssetPages"));
    pages_->addTab(tr("物体")); pages_->addTab(tr("已有文件")); header->addWidget(pages_); header->addStretch();
    compactToggle_ = new QToolButton(this); compactToggle_->setObjectName(QStringLiteral("worldAssetCompact"));
    compactToggle_->setText(tr("紧凑")); compactToggle_->setCheckable(true);
    compactToggle_->setToolTip(tr("仅显示小头像，悬停查看名称；取消后显示头像与名称")); header->addWidget(compactToggle_);
    layout->addLayout(header);
    content_ = new QWidget(this); auto* body = new QVBoxLayout(content_); body->setContentsMargins(0, 0, 0, 0); body->setSpacing(4);
    filters_ = new QGridLayout;
    kind_ = new QComboBox(content_); kind_->setObjectName(QStringLiteral("worldAssetKind"));
    kind_->addItem(tr("单位"), QStringLiteral("unit")); kind_->addItem(tr("可破坏物"), QStringLiteral("destructable"));
    kind_->addItem(tr("装饰物"), QStringLiteral("doodad")); kind_->addItem(tr("物品"), QStringLiteral("item"));
    source_ = new QComboBox(content_); source_->setObjectName(QStringLiteral("worldAssetSource"));
    source_->addItems({tr("全部来源"), tr("地图自定义 / 已修改"), tr("游戏标准数据")});
    group_ = new QComboBox(content_); group_->setObjectName(QStringLiteral("worldAssetGroup"));
    catalogSet_ = new QComboBox(content_); catalogSet_->setObjectName(QStringLiteral("worldAssetCatalogSet"));
    catalogSet_->addItem(tr("对战和战役"), QString()); catalogSet_->addItem(tr("对战"), tr("对战"));
    catalogSet_->addItem(tr("战役"), tr("战役"));
    category_ = new QComboBox(content_); category_->setObjectName(QStringLiteral("worldAssetCategory"));
    category_->addItem(tr("全部类型"), QString());
    for (const auto* name : {"单位", "建筑", "英雄", "特殊"}) category_->addItem(tr(name), tr(name));
    body->addLayout(filters_);
    auto* searchRow = new QHBoxLayout;
    search_ = new QLineEdit(content_); search_->setObjectName(QStringLiteral("worldAssetSearch"));
    search_->setPlaceholderText(tr("搜索名称或 ID")); search_->setMaxLength(256);
    search_->setAttribute(Qt::WA_InputMethodEnabled, true); searchRow->addWidget(search_, 1); body->addLayout(searchRow);
    rows_ = new QListWidget(content_); rows_->setObjectName(QStringLiteral("worldAssetRows"));
    rows_->setSelectionMode(QAbstractItemView::SingleSelection); rows_->setViewMode(QListView::IconMode);
    rows_->setResizeMode(QListView::Adjust); rows_->setMovement(QListView::Static); rows_->setWordWrap(false);
    rows_->setUniformItemSizes(false); rows_->setLayoutMode(QListView::Batched); rows_->setBatchSize(128); rows_->setSpacing(2);
    rows_->setTextElideMode(Qt::ElideRight); rows_->setIconSize({56, 56}); rows_->setGridSize({84, 84}); rows_->setMinimumHeight(110);
    rows_->viewport()->installEventFilter(this); body->addWidget(rows_, 1);
    auto* footer = new QHBoxLayout; status_ = new QLabel(content_); status_->setObjectName(QStringLiteral("worldAssetStatus"));
    status_->setWordWrap(true); footer->addWidget(status_, 1); body->addLayout(footer); layout->addWidget(content_, 1);
    connect(collapse_, &QToolButton::toggled, this, &WorldAssetBrowser::SetCollapsed);
    connect(compactToggle_, &QToolButton::toggled, this, &WorldAssetBrowser::SetCompact);
    connect(pages_, &QTabBar::currentChanged, this, [this] { Reload(); });
    connect(kind_, &QComboBox::currentIndexChanged, this, [this] { RefreshFilters(); Reload(); });
    for (auto* combo : {source_, group_, catalogSet_, category_})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] { Reload(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { Reload(); });
    connect(rows_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const auto index = item->data(kObjectIndexRole).isValid() ? item->data(kObjectIndexRole).toInt() : -1;
        if (Current() && pages_->currentIndex() == 0 && index >= 0 && index < objects_.size() && objects_[index].entry.available)
            emit PlacementRequested(objects_[index].entry, binding_);
    });
    connect(rows_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!Current()) return;
        if (item->data(kSectionRole).toBool()) return;
        const auto index = item->data(kObjectIndexRole).isValid() ? item->data(kObjectIndexRole).toInt() : -1;
        if (pages_->currentIndex() == 0 && index >= 0 && index < objects_.size()) {
            const auto& object = objects_[index]; emit DefinitionRequested(object.object.type, object.entry.id, binding_);
        } else if (pages_->currentIndex() == 1) emit FileRequested(item->data(Qt::UserRole).toString(), binding_);
    });
    RefreshFilters();
    ApplyLayout();
    Clear();
}
void WorldAssetBrowser::SetContext(QtProject::EditorProject& project, WorldAssetBinding binding,
    Result<WorldTerrainSources> sources, std::function<bool()> current, ModelResolver resolve) {
    (void)sources; (void)resolve;
    SetContext(project, std::move(binding), std::move(current));
}
void WorldAssetBrowser::SetContext(QtProject::EditorProject& project, WorldAssetBinding binding, std::function<bool()> current) {
    auto content = CaptureContentContext(project);
    const auto revision = project.editRevision_.Revision();
    const bool sameContext = project_ == &project && binding_.mapKey == binding.mapKey && binding_.generation == binding.generation;
    // 地图修订变而 Observe 修订未前进时缺少同步水印，保守重载；正常入口须先完成 Observe。
    const bool observed = !sameContext || binding_.revision == binding.revision || revision > observedEditRevision_;
    const bool rebind = sameContext && observed && content && contentContext_ && *content == *contentContext_;
    project_ = &project; binding_ = std::move(binding); current_ = std::move(current);
    contentContext_ = observed ? std::move(content) : nullptr; observedEditRevision_ = revision;
    if (rebind) return; // queued LoadBatch 保留内容 token，使用刚更新的 current_ 和交互 binding。
    files_.clear(); filesTruncated_ = false;
    auto files = project.AssetEntries(4097); filesTruncated_ = files.size() > 4096;
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(files.size(), 4096); ++i) {
        const auto& file = files[i]; bytes += static_cast<std::size_t>(file.path.toUtf8().size());
        if (file.path.toUtf8().size() > 4096 || bytes > 1024 * 1024) { filesTruncated_ = true; break; }
        files_.push_back({file.path, file.Pending() ? tr("待导入") : tr("地图归档"), file.size});
    }
    Reload();
}
void WorldAssetBrowser::Clear() {
    ++queryGeneration_; project_.clear(); current_ = {}; objects_.clear(); files_.clear(); rows_->clear(); cursor_ = 0;
    contentContext_.reset(); observedEditRevision_ = 0;
    status_->setText(tr("打开地图后浏览物体与已有文件"));
}
bool WorldAssetBrowser::Current() const { return project_ && current_ && current_(); }
bool WorldAssetBrowser::Collapsed() const { return collapse_->isChecked(); }
void WorldAssetBrowser::SetCollapsed(bool value) {
    { const QSignalBlocker block(collapse_); collapse_->setChecked(value); }
    collapse_->setArrowType(value ? Qt::RightArrow : Qt::DownArrow); content_->setVisible(!value); emit CollapsedChanged(value);
}
void WorldAssetBrowser::SetVerticalDock(bool vertical) {
    verticalDock_ = vertical;
    if (!compactChosen_) compact_ = vertical;
    ApplyLayout();
}
void WorldAssetBrowser::SetCompact(bool compact) {
    compact_ = compact; compactChosen_ = true; ApplyLayout();
}
void WorldAssetBrowser::UpdateItemLayout(QListWidgetItem& item) {
    if (item.data(kSectionRole).toBool()) {
        item.setSizeHint({std::max(1, rows_->viewport()->width() - 2 * rows_->spacing()), 28});
    } else if (pages_->currentIndex() == 0) {
        item.setText(compact_ ? QString{} : item.data(kNameRole).toString());
        item.setSizeHint(compact_ ? QSize(40, 40) : QSize(84, 84));
    } else item.setSizeHint({});
}
void WorldAssetBrowser::ApplyLayout() {
    while (auto* item = filters_->takeAt(0)) delete item;
    const std::array<QComboBox*, 5> controls{kind_, group_, catalogSet_, category_, source_};
    for (std::size_t i = 0; i < controls.size(); ++i) {
        auto* control = controls[i]; control->setMinimumWidth(0); control->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        control->setMinimumContentsLength(4);
        if (verticalDock_) filters_->addWidget(control, static_cast<int>(i / 2), static_cast<int>(i % 2), 1, i == 4 ? 2 : 1);
        else filters_->addWidget(control, 0, static_cast<int>(i));
    }
    { const QSignalBlocker block(compactToggle_); compactToggle_->setChecked(compact_); }
    const bool files = pages_->currentIndex() == 1;
    compactToggle_->setEnabled(!files);
    rows_->setViewMode(files ? QListView::ListMode : QListView::IconMode);
    rows_->setGridSize({}); rows_->setIconSize(compact_ ? QSize(32, 32) : QSize(56, 56));
    for (int i = 0; i < rows_->count(); ++i) UpdateItemLayout(*rows_->item(i));
}
void WorldAssetBrowser::RefreshFilters() {
    const QSignalBlocker block(group_); group_->clear();
    const auto kind = kind_->currentData().toString();
    group_->addItem(kind == QLatin1String("unit") ? tr("全部种族") : tr("全部分类"), QString());
    const auto append = [this](std::initializer_list<const char*> names) {
        for (const auto* name : names) group_->addItem(tr(name), tr(name));
    };
    if (kind == QLatin1String("unit")) append({"人族", "兽族", "不死族", "暗夜精灵族", "中立 - 娜迦", "中立敌对", "中立被动"});
    else if (kind == QLatin1String("item")) append({"永久的", "可充的", "力量提升", "人造物品", "可购买的", "战役", "混杂的", "未知"});
    else if (kind == QLatin1String("doodad")) append({"道具", "建筑", "水", "悬崖/地形", "环境", "电影", "其他"});
    else append({"桥/斜坡", "路径阻断器", "树/可破坏", "其他"});
    group_->addItem(tr("分类未确认"), tr("分类未确认"));
}
void WorldAssetBrowser::Reload() {
    const auto generation = ++queryGeneration_;
    cursor_ = 0; objects_.clear(); rows_->clear(); queryIssue_.clear(); section_ = 0; sectionStarted_ = false;
    const bool filePage = pages_->currentIndex() == 1;
    kind_->setEnabled(!filePage); group_->setEnabled(!filePage); source_->setEnabled(!filePage);
    const bool unitPage = !filePage && kind_->currentData() == QStringLiteral("unit");
    catalogSet_->setVisible(unitPage); category_->setVisible(unitPage);
    grouped_ = unitPage && category_->currentData().toString().isEmpty();
    ApplyLayout();
    if (!Current()) { status_->setText(tr("资源来源已失效，请重新打开地图")); return; }
    LoadBatch(generation);
}
void WorldAssetBrowser::LoadBatch(std::uint64_t generation) {
    if (!Current() || generation != queryGeneration_) return;
    if (pages_->currentIndex() == 1) {
        int examined = 0;
        while (cursor_ < files_.size() && examined++ < 128) {
            const auto& file = files_[cursor_++];
            if (!search_->text().isEmpty() && !file.path.contains(search_->text(), Qt::CaseInsensitive)) continue;
            auto* item = new QListWidgetItem(file.path + " · " + file.source, rows_); item->setData(Qt::UserRole, file.path);
            item->setToolTip(tr("%1 字节 · 双击打开资源定义").arg(file.size));
        }
        status_->setText(tr("%1 个地图文件%2").arg(rows_->count()).arg(filesTruncated_ ? tr(" · 来源过大，仅展示有界文件页") : QString()));
        if (cursor_ < files_.size()) QTimer::singleShot(0, this, [this, generation] { LoadBatch(generation); });
        return;
    }
    QtApp::ObjectViewQuery query; query.kind = kind_->currentData().toString(); query.search = search_->text();
    query.group = group_->currentData().toString(); query.source = static_cast<QtApp::ObjectViewSource>(source_->currentIndex());
    const bool unit = query.kind == QLatin1String("unit");
    if (unit) { query.catalogSet = catalogSet_->currentData().toString(); query.catalogCategory = category_->currentData().toString(); }
    if (grouped_) query.catalogCategory = Sections()[section_];
    query.offset = cursor_; query.limit = 128;
    auto page = QtApp::QueryObjectViewEntries(*project_, query); cursor_ = page.nextOffset;
    if (page.preparing) {
        status_->setText(tr("目录索引构建中 · 按当前来源继续读取"));
        QTimer::singleShot(0, this, [this, generation] { LoadBatch(generation); }); return;
    }
    for (auto& row : page.rows) {
        if (grouped_ && !sectionStarted_) {
            auto* section = new QListWidgetItem(Sections()[section_], rows_);
            section->setData(kSectionRole, true); section->setFlags(Qt::ItemIsEnabled);
            auto font = section->font(); font.setBold(true); section->setFont(font);
            section->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter); UpdateItemLayout(*section); sectionStarted_ = true;
        }
        auto* item = new QListWidgetItem(row.entry.name, rows_);
        item->setData(kObjectIndexRole, objects_.size()); item->setData(kNameRole, row.entry.name);
        item->setData(Qt::AccessibleTextRole, row.entry.name);
        const auto source = row.customObject ? tr("地图自定义") : row.mapObject ? tr("地图已修改") : tr("游戏标准数据");
        item->setToolTip(tr("%1 · %2\n%3\n%4\n%5%6").arg(row.entry.name, row.entry.id,
            row.classification.folders.join(QStringLiteral(" / ")), source, row.entry.description,
            row.classification.issue.isEmpty() ? QString() : QLatin1Char('\n') + row.classification.issue));
        item->setData(Qt::UserRole, row.entry.id);
        if (row.mapObject) item->setForeground(QColor(0xd5, 0x98, 0xd7));
        if (!row.entry.available) item->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
        if (icons_) {
            // 装饰物/可破坏物没有自身图标，与原版一样显示类别图标。
            QIcon icon = icons_->IconForTexture(row.entry.iconPath);
            if (icon.isNull()) icon = icons_->IconForObjectCategory(row.object.type, row.classification.category);
            item->setIcon(icon);
        }
        UpdateItemLayout(*item);
        objects_.push_back(std::move(row));
    }
    if (!page.issue.isEmpty()) queryIssue_ = page.issue;
    status_->setText(tr("%1 个物体%2 · 单击摆放，拖入世界，双击定义%3").arg(objects_.size())
        .arg(page.hasMore ? tr(" · 目录读取中") : QString()).arg(queryIssue_.isEmpty() ? QString() : " · " + queryIssue_));
    if (page.hasMore && page.nextOffset > query.offset)
        QTimer::singleShot(0, this, [this, generation] { LoadBatch(generation); });
    else if (grouped_ && section_ + 1 < Sections().size()) {
        ++section_; cursor_ = 0; sectionStarted_ = false;
        QTimer::singleShot(0, this, [this, generation] { LoadBatch(generation); });
    }
}
bool WorldAssetBrowser::eventFilter(QObject* watched, QEvent* event) {
    if (watched == rows_->viewport() && event->type() == QEvent::Resize)
        for (int i = 0; i < rows_->count(); ++i) if (rows_->item(i)->data(kSectionRole).toBool()) UpdateItemLayout(*rows_->item(i));
    if (watched == rows_->viewport() && event->type() == QEvent::MouseButtonPress) press_ = static_cast<QMouseEvent*>(event)->position().toPoint();
    if (watched == rows_->viewport() && event->type() == QEvent::MouseMove && Current() && pages_->currentIndex() == 0) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const auto* item = rows_->currentItem();
        const auto index = item && item->data(kObjectIndexRole).isValid() ? item->data(kObjectIndexRole).toInt() : -1;
        if ((mouse->buttons() & Qt::LeftButton) && (mouse->position().toPoint() - press_).manhattanLength() >= QApplication::startDragDistance()
            && index >= 0 && index < objects_.size() && objects_[index].entry.available) {
            const auto bytes = DragBytes(binding_, objects_[index].object);
            if (!ParseDrag(bytes).Succeeded()) return false;
            auto* mime = new QMimeData; mime->setData(DragMimeType(), bytes);
            QDrag drag(this); drag.setMimeData(mime); drag.exec(Qt::CopyAction); return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void WorldAssetBrowser::showEvent(QShowEvent* event) { QWidget::showEvent(event); }
Result<WorldAssetDrag> WorldAssetBrowser::ParseDrag(const QByteArray& bytes) {
    const auto fail = [] { return Result<WorldAssetDrag>::Failure("World.AssetBrowser", "ASSET_DRAG_INVALID", "资源拖放请求不满足身份与有界格式"); };
    if (bytes.isEmpty() || bytes.size() > 4096) return fail();
    QJsonParseError error; const auto parsed = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !parsed.isObject()) return fail();
    const auto object = parsed.object(); WorldAssetDrag result;
    if (object.size() != 6 || object.value("schema").toString() != QStringLiteral("forge-world-asset-v1") || !object.value("mapKey").isString()
        || !Decimal(object.value("generation"), result.binding.generation) || !result.binding.generation
        || !Decimal(object.value("revision"), result.binding.revision) || !object.value("type").isDouble() || !object.value("rawcode").isString()) return fail();
    result.binding.mapKey = object.value("mapKey").toString(); const auto type = object.value("type").toDouble();
    const auto rawcode = object.value("rawcode").toString();
    if (result.binding.mapKey.isEmpty() || result.binding.mapKey.toUtf8().size() > 2048 || !std::isfinite(type)
        || (type != static_cast<int>(ObjectData::ObjectType::Unit) && type != static_cast<int>(ObjectData::ObjectType::Item)
            && type != static_cast<int>(ObjectData::ObjectType::Doodad) && type != static_cast<int>(ObjectData::ObjectType::Destructable))
        || std::floor(type) != type || rawcode.size() != 4) return fail();
    for (const auto ch : rawcode) if (ch < QLatin1Char('!') || ch > QLatin1Char('~')) return fail();
    result.object = {static_cast<ObjectData::ObjectType>(static_cast<unsigned>(type)), rawcode.toStdString()};
    return Result<WorldAssetDrag>::Success(std::move(result));
}
} // namespace War3Map::Modules
