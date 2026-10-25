// 世界视图的地形笔刷面板：笔刷种类（剖面图标）、地面贴图缩略图、悬崖/荒芜选项、尺寸与强度，
// 以及视口中的笔刷落点。笔划的提交仍在 WorldModuleEditing.cpp。
#include "WorldModule.h"
#include "WorldPanelWidgets.h"
#include "WorldTerrainBuild.h"
#include "WorldViewport.h"
#include "EditorProject.h"
#include "Fonts/FontLoader.h"
#include "Icons/SvgIcons.h"
#include "Widgets/Motion.h"
#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/MapData/TerrainEditing.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace War3Map::Modules {
namespace {

using namespace WorldUi;

constexpr int kSwatchColumns = 5;
constexpr int kStrengthSteps = 100;

// 地形剖面图标：以横截面表达每种笔刷对地面的作用，颜色随图标状态与当前主题变化。
class BrushGlyph final : public QIconEngine {
public:
    explicit BrushGlyph(MapData::TerrainBrushKind kind, std::optional<MapData::TerrainWaterBrushKind> water = {})
        : kind_(kind), water_(water) {}
    QIconEngine* clone() const override { return new BrushGlyph(kind_, water_); }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const auto colors = UiKit::CurrentIconPalette();
        const QColor color = mode == QIcon::Disabled ? colors.disabled : state == QIcon::On ? colors.on
            : mode == QIcon::Active || mode == QIcon::Selected ? colors.active : colors.normal;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const qreal scale = std::min(rect.width(), rect.height()) / 24.0;
        painter->translate(rect.center() - QPointF(12 * scale, 12 * scale) + QPointF(0.5, 0.5));
        painter->scale(scale, scale);
        QPen pen(color, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter->setPen(pen);
        QColor soft = color; soft.setAlphaF(0.32f);
        QPainterPath path;
        if (water_) {
            const qreal bottom = *water_ == MapData::TerrainWaterBrushKind::Deep ? 21 : 16;
            painter->fillRect(QRectF(7, 10, 10, bottom - 10), soft);
            path.moveTo(3, 5); path.lineTo(7, 5); path.lineTo(7, bottom);
            path.lineTo(17, bottom); path.lineTo(17, 5); path.lineTo(21, 5);
            path.moveTo(3, 10); path.lineTo(21, 10);
            painter->drawPath(path); painter->restore(); return;
        }
        switch (kind_) {
        case MapData::TerrainBrushKind::Texture:
            painter->fillRect(QRectF(5, 5, 7, 7), soft); painter->fillRect(QRectF(12, 12, 7, 7), soft);
            path.addRoundedRect(QRectF(5, 5, 14, 14), 2, 2);
            path.moveTo(12, 5); path.lineTo(12, 19); path.moveTo(5, 12); path.lineTo(19, 12);
            break;
        case MapData::TerrainBrushKind::Blight:
            painter->setBrush(color);
            painter->drawEllipse(QPointF(12, 12), 3.2, 3.2);
            for (const QPointF dot : {QPointF(6, 8), QPointF(18, 7), QPointF(17.5, 17), QPointF(7, 17.5)})
                painter->drawEllipse(dot, 1.1, 1.1);
            painter->setBrush(Qt::NoBrush);
            pen.setColor(soft); painter->setPen(pen);
            path.addEllipse(QPointF(12, 12), 8.5, 8.5);
            break;
        case MapData::TerrainBrushKind::Raise:
            path.moveTo(3, 19); path.cubicTo(8, 19, 8.5, 11, 12, 11); path.cubicTo(15.5, 11, 16, 19, 21, 19);
            path.moveTo(12, 8); path.lineTo(12, 3); path.moveTo(9.5, 5.5); path.lineTo(12, 3); path.lineTo(14.5, 5.5);
            break;
        case MapData::TerrainBrushKind::Lower:
            path.moveTo(3, 8); path.cubicTo(8, 8, 8.5, 19, 12, 19); path.cubicTo(15.5, 19, 16, 8, 21, 8);
            path.moveTo(12, 4); path.lineTo(12, 13); path.moveTo(9.5, 10.5); path.lineTo(12, 13); path.lineTo(14.5, 10.5);
            break;
        case MapData::TerrainBrushKind::Plateau:
            path.moveTo(3, 19); path.lineTo(7, 19); path.lineTo(9.5, 9); path.lineTo(14.5, 9); path.lineTo(17, 19); path.lineTo(21, 19);
            painter->save(); pen.setColor(soft); painter->setPen(pen);
            painter->drawLine(QPointF(5, 9), QPointF(19, 9)); painter->restore();
            break;
        case MapData::TerrainBrushKind::Smooth:
            path.moveTo(3, 15); path.cubicTo(7, 10, 9, 10, 12, 13); path.cubicTo(15, 16, 17, 16, 21, 11);
            painter->save(); pen.setColor(soft); painter->setPen(pen);
            painter->drawPolyline(QPolygonF({QPointF(3, 9), QPointF(6, 5), QPointF(9, 9), QPointF(12, 4), QPointF(15, 8),
                QPointF(18, 4), QPointF(21, 7)}));
            painter->restore();
            break;
        case MapData::TerrainBrushKind::CliffLevel:
        case MapData::TerrainBrushKind::CliffTexture:
            path.moveTo(3, 19); path.lineTo(9, 19); path.lineTo(11, 8); path.lineTo(21, 8);
            painter->save(); pen.setColor(soft); pen.setWidthF(1.25); painter->setPen(pen);
            for (const qreal y : {11.0, 14.0, 17.0}) painter->drawLine(QPointF(10.2, y), QPointF(13.5, y));
            painter->restore();
            break;
        case MapData::TerrainBrushKind::Noise:
            path.moveTo(3, 16); path.lineTo(6, 8); path.lineTo(9, 15); path.lineTo(12, 5);
            path.lineTo(15, 18); path.lineTo(18, 10); path.lineTo(21, 14);
            break;
        case MapData::TerrainBrushKind::WaterHeight:
            path.moveTo(3, 15); path.cubicTo(6, 9, 9, 21, 12, 15); path.cubicTo(15, 9, 18, 21, 21, 15);
            path.moveTo(12, 11); path.lineTo(12, 3); path.moveTo(9, 6); path.lineTo(12, 3); path.lineTo(15, 6);
            break;
        case MapData::TerrainBrushKind::RemoveWater:
            path.moveTo(3, 15); path.cubicTo(6, 9, 9, 21, 12, 15); path.cubicTo(15, 9, 18, 21, 21, 15);
            path.moveTo(5, 4); path.lineTo(19, 20);
            break;
        case MapData::TerrainBrushKind::Ramp:
        case MapData::TerrainBrushKind::StraightRamp:
            path.moveTo(3, 18); path.lineTo(7, 18); path.lineTo(17, 6); path.lineTo(21, 6);
            break;
        case MapData::TerrainBrushKind::CornerRamp:
            path.moveTo(3, 18); path.lineTo(11, 18); path.lineTo(21, 8);
            path.moveTo(3, 18); path.lineTo(3, 10); path.lineTo(13, 0);
            path.moveTo(11, 18); path.lineTo(11, 10); path.lineTo(21, 0);
            break;
        case MapData::TerrainBrushKind::Boundary:
            path.addRect(QRectF(4, 4, 16, 16));
            path.moveTo(4, 12); path.lineTo(20, 12); path.moveTo(12, 4); path.lineTo(12, 20);
            painter->fillRect(QRectF(4, 4, 8, 8), soft); painter->fillRect(QRectF(12, 12, 8, 8), soft);
            break;
        case MapData::TerrainBrushKind::PathingRestriction:
            path.addRect(QRectF(4, 4, 16, 16));
            path.moveTo(4, 20); path.lineTo(20, 4);
            path.moveTo(4, 4); path.lineTo(20, 20);
            break;
        }
        painter->drawPath(path);
        painter->restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pixmap(size * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return pixmap;
    }

private:
    MapData::TerrainBrushKind kind_;
    std::optional<MapData::TerrainWaterBrushKind> water_;
};

// 地表贴图缩略图：取 atlas 左上格（完整贴图格），圆角裁切。
QIcon SwatchIcon(const Render::TerrainAtlasSnapshot& atlas) {
    const auto& image = atlas.image;
    if (atlas.cells.empty() || image.width == 0 || image.height == 0
        || image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4) return {};
    const auto& cell = atlas.cells.front();
    const QImage whole(reinterpret_cast<const uchar*>(image.rgba.data()), static_cast<int>(image.width),
        static_cast<int>(image.height), static_cast<qsizetype>(image.width) * 4, QImage::Format_RGBA8888);
    const QImage tile = whole.copy(static_cast<int>(cell.x), static_cast<int>(cell.y),
        static_cast<int>(cell.width), static_cast<int>(cell.height));
    QPixmap pixmap(72, 72);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip; clip.addRoundedRect(QRectF(0, 0, 72, 72), 9, 9);
    painter.setClipPath(clip);
    painter.drawImage(QRect(0, 0, 72, 72), tile);
    return QIcon(pixmap);
}

} // namespace

QWidget* WorldModule::BuildTerrainPage() {
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("worldTerrainScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    terrainPalette_ = Surface();
    terrainPalette_->setObjectName(QStringLiteral("worldTerrainPalette"));
    auto* column = new QVBoxLayout(terrainPalette_);
    column->setContentsMargins(12, 12, 12, 14); column->setSpacing(8);

    column->addWidget(SectionLabel(tr("地形块"), terrainPalette_));
    auto* blockTools = new QHBoxLayout;
    terrainBlockSelect_ = new QPushButton(tr("框选地形"), terrainPalette_);
    terrainBlockCopy_ = new QPushButton(tr("复制"), terrainPalette_);
    terrainBlockPaste_ = new QPushButton(tr("粘贴"), terrainPalette_);
    terrainBlockSelect_->setObjectName(QStringLiteral("worldTerrainBlockSelect"));
    terrainBlockCopy_->setObjectName(QStringLiteral("worldTerrainBlockCopy"));
    terrainBlockPaste_->setObjectName(QStringLiteral("worldTerrainBlockPaste"));
    blockTools->addWidget(terrainBlockSelect_); blockTools->addWidget(terrainBlockCopy_); blockTools->addWidget(terrainBlockPaste_);
    column->addLayout(blockTools);
    terrainBlockStatus_ = new QLabel(tr("同图复制 · 绝对高度；源块变化须重新复制"), terrainPalette_);
    terrainBlockStatus_->setObjectName(QStringLiteral("worldTerrainBlockStatus"));
    terrainBlockStatus_->setWordWrap(true); column->addWidget(terrainBlockStatus_);
    connect(terrainBlockSelect_, &QPushButton::clicked, this, &WorldModule::StartTerrainBlockSelection);
    connect(terrainBlockCopy_, &QPushButton::clicked, this, &WorldModule::CopyTerrainBlock);
    connect(terrainBlockPaste_, &QPushButton::clicked, this, &WorldModule::StartTerrainBlockPaste);

    // 笔刷种类：地形剖面图标网格
    column->addWidget(SectionLabel(tr("笔刷"), terrainPalette_));
    auto* kinds = new QGridLayout;
    kinds->setSpacing(6);
    terrainKinds_ = new QButtonGroup(terrainPalette_);
    terrainKinds_->setExclusive(true);
    const std::array brushes{
        std::tuple{tr("贴图"), MapData::TerrainBrushKind::Texture, "Texture"},
        std::tuple{tr("荒芜"), MapData::TerrainBrushKind::Blight, "Blight"},
        std::tuple{tr("升高"), MapData::TerrainBrushKind::Raise, "Raise"},
        std::tuple{tr("降低"), MapData::TerrainBrushKind::Lower, "Lower"},
        std::tuple{tr("高原"), MapData::TerrainBrushKind::Plateau, "Plateau"},
        std::tuple{tr("平滑"), MapData::TerrainBrushKind::Smooth, "Smooth"},
        std::tuple{tr("悬崖"), MapData::TerrainBrushKind::CliffLevel, "Cliff"},
        std::tuple{tr("噪波"), MapData::TerrainBrushKind::Noise, "Noise"},
        std::tuple{tr("悬崖类型"), MapData::TerrainBrushKind::CliffTexture, "CliffTexture"},
        std::tuple{tr("水面高度"), MapData::TerrainBrushKind::WaterHeight, "WaterHeight"},
        std::tuple{tr("移除水面"), MapData::TerrainBrushKind::RemoveWater, "RemoveWater"},
        std::tuple{tr("直坡"), MapData::TerrainBrushKind::Ramp, "Ramp"},
        std::tuple{tr("含水直坡"), MapData::TerrainBrushKind::StraightRamp, "StraightRamp"},
        std::tuple{tr("拐角坡道"), MapData::TerrainBrushKind::CornerRamp, "CornerRamp"},
        std::tuple{tr("边界"), MapData::TerrainBrushKind::Boundary, "Boundary"},
        std::tuple{tr("通行限制"), MapData::TerrainBrushKind::PathingRestriction, "PathingRestriction"}};
    for (std::size_t index = 0; index < brushes.size(); ++index) {
        const auto& [label, kind, name] = brushes[index];
        auto* button = new QToolButton(terrainPalette_);
        button->setObjectName(QStringLiteral("worldTerrainBrush") + QLatin1String(name));
        button->setProperty("brushTile", true);
        button->setCheckable(true);
        button->setIcon(QIcon(new BrushGlyph(kind)));
        button->setIconSize(QSize(20, 20));
        button->setText(label);
        if (kind == MapData::TerrainBrushKind::RemoveWater)
            button->setToolTip(tr("移除水面，保留地形高度和原始水高；保存时重新核对通行性与小地图。"));
        if (kind == MapData::TerrainBrushKind::StraightRamp)
            button->setToolTip(tr("单击创建或移除一级直坡，支持干地、含水地形和连接相邻完整直坡；拖动不扩大范围。"));
        if (kind == MapData::TerrainBrushKind::CornerRamp)
            button->setToolTip(tr("单击创建或移除一级外角或内角坡道，支持形态完整的干地与含水地形；多级台阶和不规则交汇会拒绝。"));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(52);
        button->setCursor(Qt::PointingHandCursor);
        terrainKinds_->addButton(button, static_cast<int>(kind));
        kinds->addWidget(button, static_cast<int>(index / 4), static_cast<int>(index % 4));
    }
    const std::array waterBrushes{
        std::tuple{tr("浅水"), MapData::TerrainWaterBrushKind::Shallow, kShallowWaterBrushId, "ShallowWater"},
        std::tuple{tr("深水"), MapData::TerrainWaterBrushKind::Deep, kDeepWaterBrushId, "DeepWater"}};
    for (std::size_t index = 0; index < waterBrushes.size(); ++index) {
        const auto& [label, kind, id, name] = waterBrushes[index];
        auto* button = new QToolButton(terrainPalette_);
        button->setObjectName(QStringLiteral("worldTerrainBrush") + QLatin1String(name));
        button->setProperty("brushTile", true); button->setCheckable(true);
        button->setIcon(QIcon(new BrushGlyph(MapData::TerrainBrushKind::WaterHeight, kind)));
        button->setIconSize(QSize(20, 20)); button->setText(label);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(52); button->setCursor(Qt::PointingHandCursor);
        terrainKinds_->addButton(button, id);
        const auto position = brushes.size() + index;
        kinds->addWidget(button, static_cast<int>(position / 4), static_cast<int>(position % 4));
    }
    terrainKinds_->button(static_cast<int>(MapData::TerrainBrushKind::Texture))->setChecked(true);
    column->addLayout(kinds);
    column->addSpacing(8);

    // 地面贴图：真实贴图缩略图
    terrainTextureSection_ = Surface(terrainPalette_);
    auto* texture = new QVBoxLayout(terrainTextureSection_);
    texture->setContentsMargins(0, 0, 0, 0); texture->setSpacing(8);
    texture->addWidget(SectionLabel(tr("地面贴图"), terrainTextureSection_));
    swatchGrid_ = Surface(terrainTextureSection_);
    auto* swatches = new QGridLayout(swatchGrid_);
    swatches->setContentsMargins(0, 0, 0, 0); swatches->setSpacing(6);
    terrainSwatches_ = new QButtonGroup(terrainTextureSection_);
    terrainSwatches_->setExclusive(true);
    texture->addWidget(swatchGrid_);
    swatchName_ = new QLabel(terrainTextureSection_);
    swatchName_->setObjectName(QStringLiteral("worldSwatchName"));
    swatchName_->setWordWrap(true);
    texture->addWidget(swatchName_);
    terrainVariation_ = new QComboBox(terrainTextureSection_);
    terrainVariation_->setObjectName(QStringLiteral("worldTerrainVariation"));
    terrainVariation_->addItem(tr("随机变体"), -1);
    for (int value = 0; value <= 17; ++value) terrainVariation_->addItem(tr("变体 %1").arg(value), value);
    texture->addWidget(terrainVariation_);
    auto* slotButtons = new QHBoxLayout;
    auto* replaceSlot = new QPushButton(tr("替换贴图…"), terrainTextureSection_);
    auto* addSlot = new QPushButton(tr("添加贴图…"), terrainTextureSection_);
    replaceSlot->setObjectName(QStringLiteral("worldTerrainReplaceTexture"));
    addSlot->setObjectName(QStringLiteral("worldTerrainAddTexture"));
    slotButtons->addWidget(replaceSlot); slotButtons->addWidget(addSlot); texture->addLayout(slotButtons);
    connect(replaceSlot, &QPushButton::clicked, this, [this] { EditTerrainTextureSlot(false, false); });
    connect(addSlot, &QPushButton::clicked, this, [this] { EditTerrainTextureSlot(false, true); });
    column->addWidget(terrainTextureSection_);

    // 悬崖：类型与层级变化
    terrainCliffSection_ = Surface(terrainPalette_);
    auto* cliff = new QVBoxLayout(terrainCliffSection_);
    cliff->setContentsMargins(0, 0, 0, 0); cliff->setSpacing(8);
    cliff->addWidget(SectionLabel(tr("悬崖"), terrainCliffSection_));
    terrainCliffTexture_ = new QComboBox(terrainCliffSection_);
    terrainCliffTexture_->setObjectName(QStringLiteral("worldTerrainCliffTexture"));
    cliff->addWidget(terrainCliffTexture_);
    terrainCliffDeltas_ = new QButtonGroup(terrainCliffSection_);
    cliff->addWidget(Segment(terrainCliffSection_, terrainCliffDeltas_,
        {{tr("降低一级"), -1}, {tr("保持"), 0}, {tr("升高一级"), 1}}, "worldTerrainCliffDelta"));
    terrainCliffDeltas_->button(1)->setChecked(true);
    auto* replaceCliff = new QPushButton(tr("替换悬崖贴图…"), terrainCliffSection_);
    replaceCliff->setObjectName(QStringLiteral("worldTerrainReplaceCliffTexture"));
    auto* addCliff = new QPushButton(tr("添加悬崖贴图…"), terrainCliffSection_);
    addCliff->setObjectName(QStringLiteral("worldTerrainAddCliffTexture"));
    auto* cliffButtons = new QHBoxLayout; cliffButtons->addWidget(replaceCliff); cliffButtons->addWidget(addCliff);
    cliff->addLayout(cliffButtons);
    connect(replaceCliff, &QPushButton::clicked, this, [this] { EditTerrainTextureSlot(true, false); });
    connect(addCliff, &QPushButton::clicked, this, [this] { EditTerrainTextureSlot(true, true); });
    column->addWidget(terrainCliffSection_);

    terrainWaterSection_ = Surface(terrainPalette_);
    auto* water = new QVBoxLayout(terrainWaterSection_);
    water->setContentsMargins(0, 8, 0, 0); water->setSpacing(8);
    water->addWidget(SectionLabel(tr("已有水面基础高度"), terrainWaterSection_));
    terrainWaterHeight_ = new QDoubleSpinBox(terrainWaterSection_);
    terrainWaterHeight_->setObjectName(QStringLiteral("worldTerrainWaterHeight"));
    terrainWaterHeight_->setRange(-2048, 2047.75); terrainWaterHeight_->setDecimals(2);
    terrainWaterHeight_->setSingleStep(0.25);
    water->addWidget(terrainWaterHeight_);
    column->addWidget(terrainWaterSection_);

    terrainWaterBrushSection_ = Surface(terrainPalette_);
    auto* waterBrush = new QVBoxLayout(terrainWaterBrushSection_);
    waterBrush->setContentsMargins(0, 8, 0, 0); waterBrush->setSpacing(8);
    terrainEnforceWaterLimits_ = new QCheckBox(tr("限制水面高度"), terrainWaterBrushSection_);
    terrainEnforceWaterLimits_->setObjectName(QStringLiteral("worldTerrainEnforceWaterLimits"));
    terrainEnforceWaterLimits_->setChecked(true);
    waterBrush->addWidget(terrainEnforceWaterLimits_);
    terrainWaterBrushStatus_ = new QLabel(terrainWaterBrushSection_);
    terrainWaterBrushStatus_->setObjectName(QStringLiteral("worldTerrainWaterBrushStatus"));
    terrainWaterBrushStatus_->setWordWrap(true);
    waterBrush->addWidget(terrainWaterBrushStatus_);
    column->addWidget(terrainWaterBrushSection_);
    connect(terrainEnforceWaterLimits_, &QCheckBox::toggled, this, [this] {
        CancelTerrainStroke(); InvalidateWaterBrushCapture(); QueueWaterBrushCapture();
    });

    terrainRampSection_ = Surface(terrainPalette_);
    auto* ramp = new QVBoxLayout(terrainRampSection_);
    ramp->setContentsMargins(0, 8, 0, 0); ramp->setSpacing(8);
    ramp->addWidget(SectionLabel(tr("坡道"), terrainRampSection_));
    terrainRampModes_ = new QButtonGroup(terrainRampSection_);
    ramp->addWidget(Segment(terrainRampSection_, terrainRampModes_, {{tr("创建"), 1}, {tr("移除"), 0}},
        "worldTerrainRampMode"));
    terrainRampModes_->button(1)->setChecked(true);
    column->addWidget(terrainRampSection_);

    // 荒芜：铺设 / 清除
    terrainBlightSection_ = Surface(terrainPalette_);
    auto* blight = new QVBoxLayout(terrainBlightSection_);
    blight->setContentsMargins(0, 0, 0, 0); blight->setSpacing(8);
    blight->addWidget(SectionLabel(tr("荒芜"), terrainBlightSection_));
    terrainBlightModes_ = new QButtonGroup(terrainBlightSection_);
    blight->addWidget(Segment(terrainBlightSection_, terrainBlightModes_, {{tr("铺设"), 1}, {tr("清除"), 0}},
        "worldTerrainBlightMode"));
    terrainBlightModes_->button(1)->setChecked(true);
    column->addWidget(terrainBlightSection_);

    terrainBoundarySection_ = Surface(terrainPalette_);
    auto* boundary = new QVBoxLayout(terrainBoundarySection_);
    boundary->setContentsMargins(0, 8, 0, 0); boundary->setSpacing(8);
    boundary->addWidget(SectionLabel(tr("边界标志"), terrainBoundarySection_));
    terrainBoundaryModes_ = new QButtonGroup(terrainBoundarySection_);
    boundary->addWidget(Segment(terrainBoundarySection_, terrainBoundaryModes_, {{tr("设置"), 1}, {tr("清除"), 0}},
        "worldTerrainBoundaryMode"));
    terrainBoundaryModes_->button(1)->setChecked(true);
    terrainBoundaryStatus_ = new QLabel(terrainBoundarySection_);
    terrainBoundaryStatus_->setObjectName(QStringLiteral("worldTerrainBoundaryStatus"));
    terrainBoundaryStatus_->setWordWrap(true); boundary->addWidget(terrainBoundaryStatus_);
    column->addWidget(terrainBoundarySection_);
    column->addWidget(BuildPathingRestrictionControls(terrainPalette_));
    column->addSpacing(8);

    // 尺寸：半径滑条 + 数值，形状分段
    column->addWidget(SectionLabel(tr("尺寸"), terrainPalette_));
    auto* radiusRow = new QHBoxLayout;
    radiusRow->setSpacing(8);
    terrainRadiusSlider_ = new QSlider(Qt::Horizontal, terrainPalette_);
    terrainRadiusSlider_->setObjectName(QStringLiteral("worldTerrainRadiusSlider"));
    terrainRadiusSlider_->setRange(1, 8);
    terrainRadius_ = new QSpinBox(terrainPalette_);
    terrainRadius_->setObjectName(QStringLiteral("worldTerrainRadius"));
    terrainRadius_->setRange(1, 8);
    terrainRadius_->setToolTip(tr("经典笔刷尺寸 1–8：半径为尺寸减一格，1 格 = 128；[ 与 ] 键调整。"));
    terrainRadius_->setValue(3); terrainRadiusSlider_->setValue(3);
    radiusRow->addWidget(terrainRadiusSlider_, 1); radiusRow->addWidget(terrainRadius_);
    column->addLayout(radiusRow);
    terrainShapes_ = new QButtonGroup(terrainPalette_);
    column->addWidget(Segment(terrainPalette_, terrainShapes_,
        {{tr("圆形"), static_cast<int>(MapData::TerrainBrushShape::Circle)},
         {tr("方形"), static_cast<int>(MapData::TerrainBrushShape::Square)}}, "worldTerrainShape"));
    terrainShapes_->button(static_cast<int>(MapData::TerrainBrushShape::Circle))->setChecked(true);

    // 强度：升降 / 平滑
    terrainStrengthSection_ = Surface(terrainPalette_);
    auto* strength = new QVBoxLayout(terrainStrengthSection_);
    strength->setContentsMargins(0, 8, 0, 0); strength->setSpacing(8);
    strength->addWidget(SectionLabel(tr("强度"), terrainStrengthSection_));
    auto* strengthRow = new QHBoxLayout;
    strengthRow->setSpacing(8);
    terrainStrengthSlider_ = new QSlider(Qt::Horizontal, terrainStrengthSection_);
    terrainStrengthSlider_->setObjectName(QStringLiteral("worldTerrainStrengthSlider"));
    terrainStrengthSlider_->setRange(0, kStrengthSteps);
    terrainStrength_ = new QDoubleSpinBox(terrainStrengthSection_);
    terrainStrength_->setObjectName(QStringLiteral("worldTerrainStrength"));
    terrainStrength_->setDecimals(2); terrainStrength_->setRange(0.01, 1024); terrainStrength_->setValue(32);
    for (QAbstractSpinBox* spin : {static_cast<QAbstractSpinBox*>(terrainRadius_), static_cast<QAbstractSpinBox*>(terrainStrength_)}) {
        spin->setFont(UiKit::MonoFont(12));
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons); // 滑条负责步进，数值框只用于精确输入
        spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        spin->setFixedWidth(72);
    }
    strengthRow->addWidget(terrainStrengthSlider_, 1); strengthRow->addWidget(terrainStrength_);
    strength->addLayout(strengthRow);
    column->addWidget(terrainStrengthSection_);
    column->addWidget(BuildTerrainBoundsControls(terrainPalette_));
    column->addWidget(BuildTerrainResizeControls(terrainPalette_));
    column->addStretch(1);
    scroll->setWidget(terrainPalette_);

    // 滑条与数值框双向同步；强度滑条覆盖常用区间（平滑 0–1，升降 0–256），数值框可输入更大值。
    connect(terrainRadiusSlider_, &QSlider::valueChanged, terrainRadius_, &QSpinBox::setValue);
    connect(terrainRadius_, &QSpinBox::valueChanged, this, [this](int value) {
        const QSignalBlocker blocker(terrainRadiusSlider_);
        terrainRadiusSlider_->setValue(value);
        UpdateToolHint(); UpdateOverlay();
    });
    connect(terrainStrengthSlider_, &QSlider::valueChanged, this, [this](int step) {
        const double top = CurrentBrushKind() == MapData::TerrainBrushKind::Smooth ? 1.0 : 256.0;
        const QSignalBlocker blocker(terrainStrength_);
        terrainStrength_->setValue(std::max(terrainStrength_->minimum(), top * step / kStrengthSteps));
    });
    connect(terrainStrength_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        const double top = CurrentBrushKind() == MapData::TerrainBrushKind::Smooth ? 1.0 : 256.0;
        const QSignalBlocker blocker(terrainStrengthSlider_);
        terrainStrengthSlider_->setValue(static_cast<int>(std::lround(std::clamp(value / top, 0.0, 1.0) * kStrengthSteps)));
    });
    connect(terrainShapes_, &QButtonGroup::idToggled, this, [this](int, bool on) { if (on) UpdateOverlay(); });
    connect(terrainSwatches_, &QButtonGroup::idToggled, this, [this](int id, bool on) {
        if (!on || id < 0 || static_cast<std::size_t>(id) >= swatchIds_.size()) return;
        swatchName_->setText(terrainSwatches_->button(id)->toolTip());
        UpdateToolHint();
    });
    return scroll;
}

// ── 地形笔刷 ──

MapData::TerrainBrushKind WorldModule::CurrentBrushKind() const {
    const int id = terrainKinds_ ? terrainKinds_->checkedId() : -1;
    // 水刷单独分派。此返回值只供共用光标的格点足迹使用，不执行普通笔刷。
    if (id == kShallowWaterBrushId || id == kDeepWaterBrushId) return MapData::TerrainBrushKind::Plateau;
    return id < 0 ? MapData::TerrainBrushKind::Texture : static_cast<MapData::TerrainBrushKind>(id);
}

std::optional<MapData::TerrainWaterBrushKind> WorldModule::CurrentWaterBrushKind() const {
    const int id = terrainKinds_ ? terrainKinds_->checkedId() : -1;
    if (id == kShallowWaterBrushId) return MapData::TerrainWaterBrushKind::Shallow;
    if (id == kDeepWaterBrushId) return MapData::TerrainWaterBrushKind::Deep;
    return std::nullopt;
}

QString WorldModule::TerrainName(const std::string& raw, const std::string& rawcode) const {
    QString name;
    if (!raw.empty()) {
        const auto strings = project_.CurrentWorldEditStrings();
        name = QString::fromUtf8(strings && Game::WorldEditStrings::IsKey(raw) ? strings->Resolve(raw) : raw);
    }
    const auto code = QString::fromLatin1(rawcode.data(), static_cast<qsizetype>(rawcode.size()));
    return name.isEmpty() || name == code ? code : name;
}

// 笔刷落点：按格点吸附后画出实际作用范围（圆形半径 r+0.5 格，方形同宽）。
void WorldModule::UpdateBrushCursor(std::optional<std::array<double, 2>> point) {
    if (brushCursor_ == point) return;
    brushCursor_ = point;
    UpdateOverlay();
}

void WorldModule::RefreshTerrainPalette() {
    if (!terrainCliffTexture_) return;
    const QByteArray previousCliff = terrainCliffTexture_->currentData().toByteArray();
    const auto source = project_.mapEdits_ ? project_.mapEdits_->Terrain() : nullptr;
    const WorldTerrainCache* cache = !worker_ && terrainCache_ ? terrainCache_.get() : nullptr;
    {
        const QSignalBlocker cliffBlock(terrainCliffTexture_);
        terrainCliffTexture_->clear();
        if (source) {
            const auto& ids = source->header.cliffTilesets;
            for (std::size_t index = 0; index < ids.size(); ++index) {
                const auto& id = ids[index];
                const bool named = cache && cache->header.cliffTilesets == ids && index < cache->names.cliffs.size();
                const QByteArray bytes(id.data(), static_cast<qsizetype>(id.size()));
                terrainCliffTexture_->addItem(TerrainName(named ? cache->names.cliffs[index] : std::string{}, id), bytes);
            }
        }
        const auto cliffIndex = terrainCliffTexture_->findData(previousCliff);
        if (cliffIndex >= 0) terrainCliffTexture_->setCurrentIndex(cliffIndex);
    }
    RefreshTerrainSwatches();
    terrainToggle_->setEnabled(source && source == rawTerrain_ && terrain_ &&
        viewport_->VertexCount() == source->vertices.size());
    if (!terrainToggle_->isEnabled() && terrainToggle_->isChecked()) terrainToggle_->setChecked(false);
    UpdateTerrainBrushControls();
    UpdateTerrainBoundsControls();
    UpdateTerrainResizeControls();
    UpdateToolState();
}

void WorldModule::RefreshTerrainSwatches() {
    if (!terrainSwatches_) return;
    const auto source = project_.mapEdits_ ? project_.mapEdits_->Terrain() : nullptr;
    std::vector<std::string> ids;
    if (source) for (const auto& id : source->header.groundTilesets) ids.emplace_back(id.data(), id.size());
    const WorldTerrainCache* cache = !worker_ && terrainCache_ ? terrainCache_.get() : nullptr;
    const void* key = cache ? static_cast<const void*>(cache) : swatchSource_;
    if (ids == swatchIds_ && key == swatchSource_ && terrainSwatches_->buttons().size() == static_cast<qsizetype>(ids.size())) return;
    const auto previous = terrainSwatches_->checkedId() >= 0 && static_cast<std::size_t>(terrainSwatches_->checkedId()) < swatchIds_.size()
        ? swatchIds_[static_cast<std::size_t>(terrainSwatches_->checkedId())] : std::string{};
    for (auto* button : terrainSwatches_->buttons()) { terrainSwatches_->removeButton(button); button->deleteLater(); }
    swatchIds_ = ids;
    swatchSource_ = key;
    auto* grid = static_cast<QGridLayout*>(swatchGrid_->layout());
    const bool matched = cache && cache->header.groundTilesets.size() == ids.size()
        && std::equal(ids.begin(), ids.end(), cache->header.groundTilesets.begin(),
            [](const std::string& left, const auto& right) { return left == std::string(right.data(), right.size()); });
    int selected = 0;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        auto* button = new QToolButton(swatchGrid_);
        button->setObjectName(QStringLiteral("worldSwatch"));
        button->setCheckable(true);
        button->setFixedSize(42, 42);
        button->setIconSize(QSize(34, 34));
        button->setCursor(Qt::PointingHandCursor);
        const auto name = TerrainName(matched && index < cache->names.ground.size() ? cache->names.ground[index] : std::string{}, ids[index]);
        const auto code = QString::fromLatin1(ids[index].data(), static_cast<qsizetype>(ids[index].size()));
        button->setToolTip(name == code ? code : tr("%1 · %2").arg(name, code));
        QIcon icon;
        if (matched && index < cache->atlases.size() && cache->atlases[index]) icon = SwatchIcon(*cache->atlases[index]);
        if (icon.isNull()) { button->setText(code); button->setToolButtonStyle(Qt::ToolButtonTextOnly); }
        else button->setIcon(icon);
        terrainSwatches_->addButton(button, static_cast<int>(index));
        grid->addWidget(button, static_cast<int>(index) / kSwatchColumns, static_cast<int>(index) % kSwatchColumns);
        if (ids[index] == previous) selected = static_cast<int>(index);
    }
    if (auto* button = terrainSwatches_->button(selected)) button->setChecked(true);
    else swatchName_->setText(tr("当前地图没有地面贴图"));
}

void WorldModule::AppendTerrainBoundaryOverlay(std::vector<War3RenderOverlayLineV2>& lines) {
    if (!terrain_ || !terrainBoundaryStatus_) return;
    if (!viewport_ || !viewport_->HasOverlaySupport()) {
        boundaryOverlaySource_.reset(); boundaryOverlay_.clear();
        terrainBoundaryStatus_->setText(tr("渲染器不支持边界标记"));
        terrainBoundaryStatus_->setProperty("boundaryMarkerCount", QVariant::fromValue(qulonglong{0}));
        return;
    }
    if (boundaryOverlaySource_.lock() != terrain_) {
        boundaryOverlaySource_ = terrain_;
        boundaryOverlay_.clear();
        const auto count = static_cast<std::size_t>(std::count_if(terrain_->vertices.begin(), terrain_->vertices.end(),
            [](const auto& vertex) { return vertex.IsBoundary(); }));
        constexpr std::size_t maximumMarkers = 4096;
        const auto stride = std::max(std::size_t{1}, (count + maximumMarkers - 1) / maximumMarkers);
        boundaryOverlay_.reserve(std::min(count, maximumMarkers) * 2);
        std::size_t ordinal = 0;
        for (const auto& vertex : terrain_->vertices) {
            if (!vertex.IsBoundary() || ordinal++ % stride != 0) continue;
            const auto& p = vertex.position;
            const float z = p[2] + 8.0f;
            boundaryOverlay_.push_back({{p[0] - 20, p[1], z}, {p[0] + 20, p[1], z}, {1.0f, 0.57f, 0.22f}});
            boundaryOverlay_.push_back({{p[0], p[1] - 20, z}, {p[0], p[1] + 20, z}, {1.0f, 0.57f, 0.22f}});
        }
        const auto shown = boundaryOverlay_.size() / 2;
        terrainBoundaryStatus_->setText(shown == count ? tr("边界顶点：%1").arg(count)
            : tr("边界顶点：%1（标记 %2）").arg(count).arg(shown));
        terrainBoundaryStatus_->setProperty("boundaryVertexCount", QVariant::fromValue(static_cast<qulonglong>(count)));
        terrainBoundaryStatus_->setProperty("boundaryMarkerCount", QVariant::fromValue(static_cast<qulonglong>(shown)));
    }
    // 地形修订不变时只追加缓存；鼠标每次移动不再扫描整张地形。
    lines.insert(lines.end(), boundaryOverlay_.begin(), boundaryOverlay_.end());
}

void WorldModule::UpdateTerrainBrushControls() {
    if (!terrainKinds_) return;
    const auto kind = CurrentBrushKind();
    const bool waterBrush = CurrentWaterBrushKind().has_value();
    const bool shaping = kind == MapData::TerrainBrushKind::Raise || kind == MapData::TerrainBrushKind::Lower
        || kind == MapData::TerrainBrushKind::Smooth || kind == MapData::TerrainBrushKind::Noise;
    const std::array sections{std::pair{terrainTextureSection_, kind == MapData::TerrainBrushKind::Texture},
        std::pair{terrainCliffSection_, waterBrush || kind == MapData::TerrainBrushKind::CliffLevel || kind == MapData::TerrainBrushKind::CliffTexture},
        std::pair{terrainWaterSection_, kind == MapData::TerrainBrushKind::WaterHeight},
        std::pair{terrainWaterBrushSection_, NeedsStructuralBrushRules()},
        std::pair{terrainRampSection_, kind == MapData::TerrainBrushKind::Ramp || kind == MapData::TerrainBrushKind::CornerRamp
            || kind == MapData::TerrainBrushKind::StraightRamp},
        std::pair{terrainBlightSection_, kind == MapData::TerrainBrushKind::Blight},
        std::pair{terrainBoundarySection_, kind == MapData::TerrainBrushKind::Boundary},
        std::pair{terrainPathingSection_, kind == MapData::TerrainBrushKind::PathingRestriction},
        std::pair{terrainStrengthSection_, shaping}};
    for (const auto& [section, shown] : sections) {
        const bool appearing = shown && section->isHidden();
        section->setVisible(shown);
        if (appearing) UiKit::FadeIn(section, 160, 0.2);
    }
    for (auto* button : terrainCliffDeltas_->buttons()) button->setVisible(kind == MapData::TerrainBrushKind::CliffLevel);
    const bool pointRamp = kind == MapData::TerrainBrushKind::CornerRamp || kind == MapData::TerrainBrushKind::StraightRamp;
    terrainEnforceWaterLimits_->setVisible(!pointRamp);
    const bool sized = kind != MapData::TerrainBrushKind::Ramp && !pointRamp;
    terrainRadius_->setEnabled(sized); terrainRadiusSlider_->setEnabled(sized);
    for (auto* button : terrainShapes_->buttons()) button->setEnabled(sized);
    if (shaping) {
        const QSignalBlocker block(terrainStrength_);
        const bool smooth = kind == MapData::TerrainBrushKind::Smooth;
        // 地形重绘也会刷新面板；只有在平滑与升降的取值范围切换时才应用默认强度。
        if (smooth != (terrainStrength_->maximum() <= 1.0)) {
            terrainStrength_->setRange(0.01, smooth ? 1.0 : 1024.0);
            terrainStrength_->setValue(smooth ? 0.5 : 32.0);
        }
        terrainStrength_->setSingleStep(smooth ? 0.1 : 8.0);
        const QSignalBlocker sliderBlock(terrainStrengthSlider_);
        const double top = smooth ? 1.0 : 256.0;
        terrainStrengthSlider_->setValue(static_cast<int>(std::lround(terrainStrength_->value() / top * kStrengthSteps)));
    }
    UpdateOverlay();
}

} // namespace War3Map::Modules
