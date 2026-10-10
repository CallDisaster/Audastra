#include "MainWindow.h"
#include "War3MapCore/MapDocument.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace War3Map;
namespace {
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QString N(double value) { return QString::number(value, 'g', 9); }
QString Mask(std::uint32_t value) { return QString("0x%1").arg(value, 8, 16, QChar('0')); }
QString Raw(const std::array<std::byte, 4>& value) {
    return Text({reinterpret_cast<const char*>(value.data()), value.size()});
}
template<class Array> QString Vector(const Array& values) {
    QStringList parts;
    for (auto value : values) parts << N(value);
    return parts.join("  ·  ");
}
QString State(MapDataState state) {
    switch (state) {
    case MapDataState::Missing: return QObject::tr("未提供");
    case MapDataState::Ready: return QObject::tr("已读取");
    case MapDataState::Partial: return QObject::tr("部分可读");
    case MapDataState::Invalid: return QObject::tr("无法解析");
    case MapDataState::Ambiguous: return QObject::tr("来源冲突");
    case MapDataState::Unreadable: return QObject::tr("无法读取");
    }
    return {};
}
const QStringList categories{QObject::tr("地图设置"), QObject::tr("单位 / 物品实例"),
    QObject::tr("装饰物 / 可破坏物"), QObject::tr("区域"), QObject::tr("镜头"), QObject::tr("声音定义"), QObject::tr("字符串")};

// 单一只读检查面板；大列表每页最多 100 个记录，不为全部实例创建控件。
class MapDataPanel final : public QWidget {
public:
    MapDataPanel(const MapDocument& document, QWidget* parent) : QWidget(parent), document_(document) {
        auto* outer = new QHBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        auto* split = new QSplitter;
        outer->addWidget(split);
        navigation_ = new QListWidget;
        navigation_->setObjectName("mapDataCategories");
        navigation_->setMinimumWidth(205);
        for (const auto& member : document.Members()) {
            const auto count = member.recordCount ? QString::number(*member.recordCount) : QStringLiteral("—");
            auto* item = new QListWidgetItem(categories[static_cast<int>(member.kind)] + "   " + count, navigation_);
            item->setToolTip(State(member.state) + " · " + Text(member.memberName));
        }
        split->addWidget(navigation_);
        auto* right = new QWidget;
        auto* layout = new QVBoxLayout(right);
        layout->setContentsMargins(12, 0, 0, 0);
        heading_ = new QLabel;
        heading_->setObjectName("mapDataHeading");
        layout->addWidget(heading_);
        search_ = new QLineEdit;
        search_->setObjectName("mapDataSearch");
        search_->setPlaceholderText(tr("搜索名称 / ID / 路径"));
        search_->setClearButtonEnabled(true);
        layout->addWidget(search_);
        tree_ = new QTreeWidget;
        tree_->setObjectName("mapDataValues");
        tree_->setHeaderLabels({tr("属性 / 记录"), tr("值")});
        tree_->setColumnWidth(0, 305);
        tree_->setAlternatingRowColors(true);
        tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        tree_->header()->setStretchLastSection(true);
        layout->addWidget(tree_, 1);
        diagnostics_ = new QLabel;
        diagnostics_->setObjectName("mapDataDiagnostics");
        diagnostics_->setWordWrap(true);
        diagnostics_->setTextFormat(Qt::PlainText);
        diagnostics_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(diagnostics_);
        auto* footer = new QHBoxLayout;
        count_ = new QLabel;
        count_->setObjectName("mapDataCount");
        footer->addWidget(count_, 1);
        previous_ = new QPushButton(tr("上一页")); next_ = new QPushButton(tr("下一页"));
        previous_->setObjectName("mapDataPrevious"); next_->setObjectName("mapDataNext");
        footer->addWidget(previous_); footer->addWidget(next_); layout->addLayout(footer);
        split->addWidget(right); split->setStretchFactor(1, 1); split->setSizes({215, 800});
        connect(navigation_, &QListWidget::currentRowChanged, this, [this] { page_ = 0; Refresh(); });
        connect(search_, &QLineEdit::textChanged, this, [this] { page_ = 0; Refresh(); });
        connect(previous_, &QPushButton::clicked, this, [this] { if (page_ > 0) --page_; Refresh(); });
        connect(next_, &QPushButton::clicked, this, [this] { ++page_; Refresh(); });
        navigation_->setCurrentRow(0);
    }
private:
    QTreeWidgetItem* Row(QTreeWidgetItem* parent, const QString& name, const QString& value = {}) {
        auto* item = parent ? new QTreeWidgetItem(parent, {name, value}) : new QTreeWidgetItem(tree_, {name, value});
        item->setToolTip(0, name); item->setToolTip(1, value);
        return item;
    }
    QString Resolve(std::string_view value) const {
        const auto resolved = document_.ResolveString(value);
        return resolved.Succeeded() ? Text(*resolved.value) : Text(value) + tr("  [未解析]");
    }
    template<class Records, class Label, class Fill>
    void RecordsPage(const Records& records, Label label, Fill fill) {
        for (const auto& record : records) {
            const QString text = label(record);
            if (!text.contains(search_->text().trimmed(), Qt::CaseInsensitive)) continue;
            const auto index = matched_++;
            if (index < page_ * 100 || index >= (page_ + 1) * 100) continue;
            auto* group = Row(nullptr, text);
            fill(group, record);
        }
    }
    void Transform(QTreeWidgetItem* parent, const MapData::PlacementTransform& entry) {
        Row(parent, tr("对象 ID"), Text(entry.rawcode)); Row(parent, tr("变体"), N(entry.variation));
        Row(parent, tr("位置 X / Y / Z"), Vector(entry.position)); Row(parent, tr("面向角（弧度）"), N(entry.rotation));
        Row(parent, tr("缩放 X / Y / Z"), Vector(entry.scale));
    }
    void Drops(QTreeWidgetItem* parent, const std::vector<MapData::PlacementItemSet>& sets) {
        for (std::size_t i = 0; i < sets.size(); ++i) {
            auto* group = Row(parent, tr("掉落组 %1").arg(i + 1));
            for (const auto& item : sets[i].items) Row(group, Text(item.rawcode), tr("概率 %1%").arg(item.chance));
        }
    }
    void Settings(const MapData::MapSettings& s) {
        auto* general = Row(nullptr, tr("基本信息"));
        Row(general, tr("名称"), Resolve(s.mapName)); Row(general, tr("作者"), Resolve(s.author));
        Row(general, tr("说明"), Resolve(s.description)); Row(general, tr("推荐玩家"), Resolve(s.recommendedPlayers));
        Row(general, tr("可玩尺寸"), N(s.playableWidth) + " × " + N(s.playableHeight));
        Row(general, tr("地形类型"), QString(QChar(s.tileset))); Row(general, tr("地图选项"), Mask(s.flags));
        Row(general, tr("格式 / 编辑器版本"), N(s.formatVersion) + " / " + N(s.editorVersion));
        Row(general, tr("保存次数"), N(s.saves)); Row(general, tr("游戏数据集"), N(s.gameDataSet));
        Row(general, tr("镜头边界"), Vector(s.cameraBounds)); Row(general, tr("边界补偿"), Vector(s.cameraMargins));
        general->setExpanded(true);
        auto* players = Row(nullptr, tr("玩家"), N(s.players.size()));
        for (const auto& p : s.players) {
            auto* g = Row(players, tr("玩家 %1 · %2").arg(p.number + 1).arg(Resolve(p.name)));
            Row(g, tr("控制类型 / 种族"), N(p.controller) + " / " + N(p.race)); Row(g, tr("固定起点标志"), Mask(p.flags));
            Row(g, tr("出生点 X / Y"), N(p.startX) + " · " + N(p.startY));
            Row(g, tr("低 / 高盟友优先级"), Mask(p.allyLowPriorityMask) + " / " + Mask(p.allyHighPriorityMask));
        }
        auto* forces = Row(nullptr, tr("队伍"), N(s.forces.size()));
        for (const auto& f : s.forces) { auto* g = Row(forces, Resolve(f.name)); Row(g, tr("玩家掩码"), Mask(f.playerMask)); Row(g, tr("联盟选项"), Mask(f.flags)); }
        const auto screen = [this](const QString& title, const auto& value) {
            auto* g = Row(nullptr, title); Row(g, tr("路径"), Text(value.path)); Row(g, tr("标题"), Resolve(value.title));
            Row(g, tr("副标题"), Resolve(value.subtitle)); Row(g, tr("正文"), Resolve(value.text)); return g;
        };
        auto* loading = screen(tr("载入画面"), s.loadingScreen); Row(loading, tr("背景编号"), N(s.loadingBackground));
        screen(tr("序章画面"), s.prologueScreen);
        const auto& e = s.environment;
        auto* env = Row(nullptr, tr("环境"));
        Row(env, tr("雾类型"), N(e.fogStyle)); Row(env, tr("雾起点 / 终点 / 浓度"), N(e.fogStart) + " / " + N(e.fogEnd) + " / " + N(e.fogDensity));
        const auto color = [](const auto& c) { return QString("%1, %2, %3, %4").arg(c.red).arg(c.green).arg(c.blue).arg(c.alpha); };
        Row(env, tr("雾颜色 RGBA"), color(e.fogColor)); Row(env, tr("水颜色 RGBA"), color(e.waterColor));
        Row(env, tr("天气 ID"), Raw(e.weather)); Row(env, tr("声音环境"), Text(e.soundEnvironment));
        Row(env, tr("光照环境"), QString(QChar(e.lightEnvironment)));
        if (s.custom) {
            auto* upgrades = Row(nullptr, tr("科技可用性"));
            for (const auto& u : s.custom->upgrades) Row(upgrades, Raw(u.id), tr("玩家 %1 · 等级 %2 · 状态 %3").arg(Mask(u.playerMask)).arg(u.level + 1).arg(u.availability));
            auto* restrictions = Row(nullptr, tr("科技树限制"));
            for (const auto& r : s.custom->techRestrictions) Row(restrictions, Raw(r.id), Mask(r.playerMask));
            auto* randomUnits = Row(nullptr, tr("随机单位表"));
            for (const auto& table : s.custom->randomUnits) {
                auto* g = Row(randomUnits, N(table.number) + " · " + Resolve(table.name)); Row(g, tr("列类型"), Vector(table.columnTypes));
                for (const auto& entry : table.rows) { QStringList ids; for (const auto& id : entry.ids) ids << Raw(id); Row(g, N(entry.chance) + "%", ids.join(" · ")); }
            }
            auto* randomItems = Row(nullptr, tr("随机物品表"));
            for (const auto& table : s.custom->randomItems) {
                auto* g = Row(randomItems, N(table.number) + " · " + Resolve(table.name));
                for (std::size_t i = 0; i < table.sets.size(); ++i) { auto* set = Row(g, tr("物品组 %1").arg(i + 1)); for (const auto& item : table.sets[i]) Row(set, Raw(item.id), N(item.chance) + "%"); }
            }
        }
        // 设置字段采用递归筛选；匹配字段时保留其父分组。
        const auto filter = [this](auto&& self, QTreeWidgetItem* item, bool inherited) -> bool {
            bool visible = inherited || item->text(0).contains(search_->text(), Qt::CaseInsensitive) || item->text(1).contains(search_->text(), Qt::CaseInsensitive);
            const bool direct = visible;
            for (int i = 0; i < item->childCount(); ++i) visible = self(self, item->child(i), direct) || visible;
            item->setHidden(!visible); if (visible && !search_->text().isEmpty()) item->setExpanded(true); return visible;
        };
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) filter(filter, tree_->topLevelItem(i), false);
    }
    void Refresh() {
        tree_->clear(); matched_ = 0;
        const auto kind = static_cast<MapDataKind>(navigation_->currentRow());
        const auto& member = document_.Member(kind);
        heading_->setText(Text(member.memberName) + "   ·   " + State(member.state) + tr("   ·   只读"));
        QStringList messages;
        for (const auto& d : member.diagnostics) messages << Text(d.code) + " · " + Text(d.message);
        if (member.state == MapDataState::Missing) messages << tr("当前源文件未提供此成员。");
        if (member.state == MapDataState::Ambiguous) for (const auto& source : member.sources) messages << Text(source.name);
        diagnostics_->setText(messages.join('\n')); diagnostics_->setVisible(!messages.isEmpty());
        const auto entryLabel = [](const auto& e) { return Text(e.rawcode) + "  ·  #" + N(e.creationId); };
        if (kind == MapDataKind::Settings && document_.Settings()) Settings(*document_.Settings());
        if (kind == MapDataKind::Units && document_.Units()) RecordsPage(document_.Units()->units, entryLabel, [this](auto* g, const auto& u) {
            Transform(g, u); Row(g, tr("实例 ID"), N(u.creationId)); Row(g, tr("所有者（文件索引）"), N(u.owner));
            Row(g, tr("生命 / 魔法"), N(u.hitPoints) + " / " + N(u.mana)); Row(g, tr("金币"), N(u.gold));
            Row(g, tr("索敌范围"), N(u.targetAcquisition)); Row(g, tr("标志"), Mask(u.flags));
            Row(g, tr("英雄等级"), N(u.heroLevel)); Row(g, tr("力量 / 敏捷 / 智力"), N(u.heroStrength) + " / " + N(u.heroAgility) + " / " + N(u.heroIntelligence));
            Row(g, tr("团队色"), N(u.teamColor)); Row(g, tr("传送门区域 ID"), N(u.waygateRegionId)); Row(g, tr("掉落表 ID"), N(u.droppedItemTable)); Drops(g, u.itemSets);
            auto* inventory = Row(g, tr("物品栏")); for (const auto& i : u.inventory) Row(inventory, tr("槽位 %1").arg(i.slot), Text(i.rawcode));
            auto* abilities = Row(g, tr("技能")); for (const auto& a : u.abilities) Row(abilities, Text(a.rawcode), tr("等级 %1 · 自动施放 %2").arg(a.level).arg(a.autocast ? tr("开") : tr("关")));
            auto* random = Row(g, tr("随机配置"));
            std::visit([this, random](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::monostate>) Row(random, tr("类型"), tr("无"));
                else if constexpr (std::is_same_v<T, MapData::RandomAnyUnit>) { Row(random, tr("随机等级"), N(value.level)); Row(random, tr("物品分类"), N(value.itemClass)); }
                else if constexpr (std::is_same_v<T, MapData::RandomGlobalTable>) { Row(random, tr("全局表 ID"), N(value.tableId)); Row(random, tr("列"), N(value.column)); }
                else for (const auto& item : value.choices) Row(random, Text(item.rawcode), N(item.chance) + "%");
            }, u.random);
        });
        if (kind == MapDataKind::Doodads && document_.Doodads()) {
            RecordsPage(document_.Doodads()->doodads, entryLabel, [this](auto* g, const auto& d) {
                Transform(g, d); Row(g, tr("实例 ID"), N(d.creationId)); Row(g, tr("生命百分比"), N(d.lifePercent));
                Row(g, tr("标志"), Mask(d.flags)); Row(g, tr("掉落表 ID"), N(d.droppedItemTable)); Drops(g, d.itemSets);
            });
            RecordsPage(document_.Doodads()->specialDoodads, [](const auto& d) { return tr("特殊 · ") + Text(d.rawcode) + " · " + Vector(d.position); }, [this](auto* g, const auto& d) { Row(g, tr("网格坐标"), Vector(d.position)); Row(g, tr("变体"), N(d.variation)); });
        }
        if (kind == MapDataKind::Regions && document_.Regions()) RecordsPage(document_.Regions()->entries,
            [this](const auto& r) { return Resolve(r.name) + " · #" + N(r.creationNumber); }, [this](auto* g, const auto& r) {
                Row(g, tr("左 / 下 / 右 / 上"), N(r.left) + " / " + N(r.bottom) + " / " + N(r.right) + " / " + N(r.top));
                Row(g, tr("天气 ID"), Raw(r.weatherId)); Row(g, tr("环境声音"), Text(r.ambientSound)); Row(g, tr("颜色 BGRA"), Vector(r.colorBgra));
            });
        if (kind == MapDataKind::Cameras && document_.Cameras()) RecordsPage(document_.Cameras()->entries,
            [this](const auto& c) { return Resolve(c.name); }, [this](auto* g, const auto& c) {
                Row(g, tr("目标 X / Y"), N(c.targetX) + " / " + N(c.targetY)); Row(g, tr("高度偏移"), N(c.zOffset));
                Row(g, tr("旋转 / 攻角 / 翻滚"), N(c.rotation) + " / " + N(c.angleOfAttack) + " / " + N(c.roll));
                Row(g, tr("距离"), N(c.targetDistance)); Row(g, tr("视场"), N(c.fieldOfView)); Row(g, tr("远 / 近裁剪"), N(c.farClippingPlane) + " / " + N(c.nearClippingPlane));
            });
        if (kind == MapDataKind::Sounds && document_.Sounds()) RecordsPage(document_.Sounds()->entries,
            [](const auto& s) { return Text(s.name) + " · " + Text(s.filePath); }, [this](auto* g, const auto& s) {
                Row(g, tr("定义名"), Text(s.name)); Row(g, tr("文件"), Text(s.filePath)); Row(g, tr("类型"), s.IsMusic() ? tr("音乐") : s.Is3D() ? tr("3D 声音") : tr("2D 声音"));
                Row(g, tr("循环"), s.IsLooping() ? tr("开") : tr("关")); Row(g, tr("超出范围停止"), s.StopsOutOfRange() ? tr("开") : tr("关"));
                Row(g, tr("音量 / 通道"), N(s.volume) + " / " + N(s.channel)); Row(g, tr("音高 / 变化"), N(s.pitch) + " / " + N(s.pitchVariance));
                Row(g, tr("淡入 / 淡出"), N(s.fadeInRate) + " / " + N(s.fadeOutRate)); Row(g, tr("优先级"), N(s.priority)); Row(g, tr("EAX"), Text(s.eaxSetting));
                Row(g, tr("最近 / 最远 / 截止距离"), N(s.minDistance) + " / " + N(s.maxDistance) + " / " + N(s.distanceCutoff));
                Row(g, tr("内 / 外锥角"), N(s.coneAngleInside) + " / " + N(s.coneAngleOutside)); Row(g, tr("锥外音量"), N(s.coneOutsideVolume));
                Row(g, tr("方向 X / Y / Z"), Vector(s.coneOrientation)); Row(g, tr("原始标志"), Mask(s.flags));
            });
        if (kind == MapDataKind::Strings && document_.Strings()) RecordsPage(document_.Strings()->entries,
            [](const auto& s) { return "TRIGSTR_" + N(s.id) + " · " + Text(s.value); }, [this](auto* g, const auto& s) {
                Row(g, tr("ID"), QString::number(s.id)); Row(g, tr("文本"), Text(s.value)); Row(g, tr("来源偏移"), N(s.sourceOffset));
            });
        const bool list = kind != MapDataKind::Settings;
        previous_->setVisible(list); next_->setVisible(list);
        previous_->setEnabled(page_ > 0); next_->setEnabled((page_ + 1) * 100 < matched_);
        count_->setText(list ? tr("%1 条匹配 · 第 %2 / %3 页").arg(matched_).arg(page_ + 1).arg(std::max<std::size_t>(1, (matched_ + 99) / 100)) : tr("地图设置 · 只读"));
    }
    const MapDocument& document_;
    QListWidget* navigation_;
    QLineEdit* search_;
    QTreeWidget* tree_;
    QLabel *heading_, *diagnostics_, *count_;
    QPushButton *previous_, *next_;
    std::size_t page_ = 0, matched_ = 0;
};
}

void MainWindow::LoadMapData() {
    project_->LoadMapDocument();
    mapDataAction_->setEnabled(project_->mapDocument_ != nullptr);
}

void MainWindow::ShowMapData() {
    if (!project_->mapDocument_) return;
    QDialog dialog(this);
    dialog.setObjectName("mapDataDialog");
    dialog.setWindowTitle(tr("地图数据")); dialog.resize(1080, 720);
    auto* layout = new QVBoxLayout(&dialog);
    auto* path = new QLabel(QString::fromStdWString(project_->mapDocument_->SourcePath().wstring()));
    path->setTextFormat(Qt::PlainText); path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(path);
    layout->addWidget(new MapDataPanel(*project_->mapDocument_, &dialog), 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons); dialog.exec();
}
