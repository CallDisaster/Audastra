#include "GameUiCanvas.h"

#include "GameUiSources.h"
#include "Theme/ThemeManager.h"
#include "War3MapCore/Text/WarcraftText.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

constexpr double kMargin = 12.0;
// fdf 没写字号时的游戏默认高度（屏幕单位）。
constexpr float kDefaultFontHeight = 0.01f;

struct Segment {
    QString text;
    QColor color;
};
using Line = std::vector<Segment>;

QColor FromFloats(const std::array<float, 4>& color) {
    const auto channel = [](float value) { return std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255); };
    return QColor(channel(color[0]), channel(color[1]), channel(color[2]), channel(color[3]));
}

std::vector<Line> ParseLines(const std::string& source, const QColor& base) {
    const auto parsed = Text::ParseWarcraftText(source);
    std::vector<Line> lines(1);
    for (const auto& run : parsed.runs) {
        if (run.kind == Text::WarcraftTextRunKind::Newline) {
            lines.emplace_back();
            continue;
        }
        QColor color = base;
        if (run.argb) color = QColor((*run.argb >> 16) & 0xFF, (*run.argb >> 8) & 0xFF, *run.argb & 0xFF, base.alpha());
        lines.back().push_back({QString::fromUtf8(parsed.plainText.data() + run.textOffset, run.textBytes), color});
    }
    return lines;
}

double LineWidth(const Line& line, const QFontMetricsF& metrics) {
    double width = 0.0;
    for (const auto& segment : line) width += metrics.horizontalAdvance(segment.text);
    return width;
}

// 超出宽度时按字符换行（中文没有空格；英文行首的空格丢弃）。
std::vector<Line> Wrap(const std::vector<Line>& lines, double maxWidth, const QFontMetricsF& metrics) {
    std::vector<Line> wrapped;
    for (const auto& line : lines) {
        Line current;
        double width = 0.0;
        for (const auto& segment : line) {
            QString pending;
            for (const QChar ch : segment.text) {
                const double advance = metrics.horizontalAdvance(ch);
                if (width + advance > maxWidth && (width > 0.0 || !pending.isEmpty())) {
                    if (!pending.isEmpty()) current.push_back({pending, segment.color});
                    wrapped.push_back(std::move(current));
                    current = {};
                    pending.clear();
                    width = 0.0;
                    if (ch.isSpace()) continue;
                }
                pending.append(ch);
                width += advance;
            }
            if (!pending.isEmpty()) current.push_back({pending, segment.color});
        }
        wrapped.push_back(std::move(current));
    }
    return wrapped;
}

QPainter::CompositionMode Composition(DrawBlend blend) {
    switch (blend) {
    case DrawBlend::Add: return QPainter::CompositionMode_Plus;
    case DrawBlend::Modulate: return QPainter::CompositionMode_Multiply;
    default: return QPainter::CompositionMode_SourceOver;
    }
}

} // namespace

GameUiCanvas::GameUiCanvas(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("gameUiCanvas"));
    setMouseTracking(true);
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::ClickFocus);
}

void GameUiCanvas::SetScene(const FrameTree* tree, FrameDrawList list) {
    tree_ = tree;
    list_ = std::move(list);
    hits_.clear();
    update();
}

void GameUiCanvas::ClearScene() {
    SetScene(nullptr, {});
    selected_ = kNoNode;
}

void GameUiCanvas::SetTextures(const QHash<QString, QImage>* textures) { textures_ = textures; update(); }
void GameUiCanvas::SetFontFamilies(const QHash<QString, QString>* families) { families_ = families; update(); }
void GameUiCanvas::SetAspect(double aspect) { aspect_ = aspect > 0.1 ? aspect : 4.0 / 3.0; update(); }
void GameUiCanvas::SetShowContent(bool show) { showContent_ = show; update(); }
void GameUiCanvas::SetShowBounds(bool show) { showBounds_ = show; update(); }
void GameUiCanvas::SetShowNames(bool show) { showNames_ = show; update(); }
void GameUiCanvas::SetSelected(std::uint32_t node) { selected_ = node; update(); }

QRectF GameUiCanvas::ScreenRect() const {
    const QRectF available = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    if (available.width() <= 0.0 || available.height() <= 0.0) return {};
    double width = available.width();
    double height = width / aspect_;
    if (height > available.height()) {
        height = available.height();
        width = height * aspect_;
    }
    return QRectF(available.center().x() - width / 2.0, available.center().y() - height / 2.0, width, height);
}

namespace {

QPointF ToPixel(const QRectF& screen, const FrameTreeOptions& options, float x, float y) {
    return {screen.left() + static_cast<double>(x) / options.screenWidth * screen.width(),
        screen.top() + (options.screenHeight - static_cast<double>(y)) / options.screenHeight * screen.height()};
}

QRectF ToPixelRect(const QRectF& screen, const FrameTreeOptions& options, const FrameRect& rect) {
    return QRectF(ToPixel(screen, options, rect.left, rect.top), ToPixel(screen, options, rect.right, rect.bottom)).normalized();
}

} // namespace

void GameUiCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());
    const QRectF screen = ScreenRect();
    if (screen.isEmpty()) return;
    painter.fillRect(screen, QColor(10, 12, 13));
    hits_.clear();
    PaintScene(painter, screen, true, &hits_);
    painter.setPen(QPen(palette().mid().color(), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(screen.adjusted(-0.5, -0.5, 0.5, 0.5));
}

QImage GameUiCanvas::RenderImage(const QSize& size) const {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(10, 12, 13));
    QPainter painter(&image);
    PaintScene(painter, QRectF(QPointF(0, 0), QSizeF(size)), false, nullptr);
    return image;
}

void GameUiCanvas::PaintScene(QPainter& painter, const QRectF& screen, bool overlays, std::vector<HitBox>* hits) const {
    if (!tree_) return;
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.save();
    painter.setClipRect(screen);
    for (const auto& command : list_.commands) {
        if (command.kind == DrawKind::Image) PaintImage(painter, screen, command, hits);
        else if (command.kind == DrawKind::Text) PaintText(painter, screen, command, hits);
        else PaintCodeArea(painter, screen, command, hits);
    }
    painter.restore();
    if (overlays) PaintOverlays(painter, screen);
}

void GameUiCanvas::PaintImage(QPainter& painter, const QRectF& screen, const FrameDrawCommand& command,
    std::vector<HitBox>* hits) const {
    const auto& options = tree_->Options();
    const ImageDraw& draw = command.image;
    const QRectF dest = ToPixelRect(screen, options, draw.rect);
    const QImage* image = nullptr;
    if (textures_) {
        const auto found = textures_->constFind(GameUiAssetKey(QString::fromStdString(draw.path)));
        if (found != textures_->constEnd() && !found->isNull()) image = &*found;
    }
    if (!image) {
        if (hits) hits->push_back({command.node, dest, nullptr, {}});
        return;
    }
    const double width = image->width();
    const double height = image->height();
    const auto texel = [&](int corner) { return QPointF(draw.uv[corner].u * width, draw.uv[corner].v * height); };
    // 四角顺序：左上、右上、右下、左下。
    const QPolygonF source{texel(0), texel(1), texel(3), texel(2)};
    const QPolygonF target{ToPixel(screen, options, draw.rect.left, draw.rect.top),
        ToPixel(screen, options, draw.rect.right, draw.rect.top), ToPixel(screen, options, draw.rect.right, draw.rect.bottom),
        ToPixel(screen, options, draw.rect.left, draw.rect.bottom)};
    QTransform mapping;
    if (!QTransform::quadToQuad(source, target, mapping)) return;
    if (showContent_ || !hits) {
        painter.save();
        painter.setCompositionMode(Composition(draw.blend));
        if (draw.repeat) {
            QBrush brush(*image);
            brush.setTransform(mapping);
            painter.fillRect(dest, brush);
        } else {
            painter.setTransform(mapping, true);
            const QRectF bounds = source.boundingRect();
            painter.drawImage(bounds, *image, bounds);
        }
        painter.restore();
    }
    if (hits) hits->push_back({command.node, dest, image, mapping.inverted()});
}

void GameUiCanvas::PaintText(QPainter& painter, const QRectF& screen, const FrameDrawCommand& command,
    std::vector<HitBox>* hits) const {
    const auto& options = tree_->Options();
    const TextDraw& draw = command.text;
    QFont font = this->font();
    if (families_) {
        const auto family = families_->constFind(GameUiAssetKey(QString::fromStdString(draw.fontPath)));
        if (family != families_->constEnd() && !family->isEmpty()) font.setFamily(*family);
    }
    const float height = draw.fontHeight > 0.0f ? draw.fontHeight : kDefaultFontHeight;
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(height / options.screenHeight * screen.height()))));
    const QFontMetricsF metrics(font);
    const QRectF area = ToPixelRect(screen, options, draw.rect);
    const bool fixedWidth = area.width() > 0.5;
    const bool fixedHeight = area.height() > 0.5;
    std::vector<Line> lines = ParseLines(draw.text, FromFloats(draw.color));
    if (fixedWidth) lines = Wrap(lines, area.width(), metrics);
    double blockWidth = 0.0;
    for (const auto& line : lines) blockWidth = std::max(blockWidth, LineWidth(line, metrics));
    const double lineHeight = metrics.height();
    const double blockHeight = lineHeight * static_cast<double>(lines.size());

    double left = area.left();
    double right = area.right();
    if (!fixedWidth) {
        const int column = static_cast<int>(draw.anchor) % 3;
        left = column == 0 ? area.left() : column == 1 ? area.left() - blockWidth / 2.0 : area.left() - blockWidth;
        right = left + blockWidth;
    }
    double top = area.top();
    if (fixedHeight) {
        if (draw.justifyV == JustifyV::Middle) top = area.top() + (area.height() - blockHeight) / 2.0;
        else if (draw.justifyV == JustifyV::Bottom) top = area.bottom() - blockHeight;
    } else {
        const int row = static_cast<int>(draw.anchor) / 3;
        top = row == 0 ? area.top() : row == 1 ? area.top() - blockHeight / 2.0 : area.top() - blockHeight;
    }
    if (showContent_ || !hits) {
        painter.save();
        painter.setFont(font);
        double y = top + metrics.ascent();
        for (const auto& line : lines) {
            const double width = LineWidth(line, metrics);
            double x = left;
            if (draw.justifyH == JustifyH::Center) x = left + (right - left - width) / 2.0;
            else if (draw.justifyH == JustifyH::Right) x = right - width;
            for (const auto& segment : line) {
                painter.setPen(segment.color);
                painter.drawText(QPointF(x, y), segment.text);
                x += metrics.horizontalAdvance(segment.text);
            }
            y += lineHeight;
        }
        painter.restore();
    }
    if (hits) {
        const QRectF box(std::min(left, area.left()), std::min(top, area.top()),
            std::max(right, area.right()) - std::min(left, area.left()),
            std::max(top + blockHeight, area.bottom()) - std::min(top, area.top()));
        hits->push_back({command.node, box, nullptr, {}});
    }
}

// 游戏代码自行绘制的区域：虚线占位框加中文名。
void GameUiCanvas::PaintCodeArea(QPainter& painter, const QRectF& screen, const FrameDrawCommand& command,
    std::vector<HitBox>* hits) const {
    const QRectF area = ToPixelRect(screen, tree_->Options(), command.image.rect);
    if (showContent_ || !hits) {
        painter.save();
        QColor line = palette().color(QPalette::PlaceholderText);
        line.setAlpha(170);
        painter.setPen(QPen(line, 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
        QFont small = font();
        small.setPixelSize(std::max(9, static_cast<int>(screen.height() / 64.0)));
        painter.setFont(small);
        painter.setPen(line);
        const QRectF inner = area.adjusted(3, 2, -3, -2);
        const QString label = QFontMetricsF(small).elidedText(
            QString::fromStdString(tree_->Nodes()[command.node].codeLabel), Qt::ElideRight, inner.width());
        // 大区域（世界画面、聊天消息等）互相重叠且左上角挤在一起，名字写在右下角。
        const bool large = area.width() * area.height() > screen.width() * screen.height() * 0.2;
        painter.setClipRect(area, Qt::IntersectClip);
        painter.drawText(inner, (large ? Qt::AlignRight | Qt::AlignBottom : Qt::AlignLeft | Qt::AlignTop), label);
        painter.restore();
    }
    if (hits) hits->push_back({command.node, area, nullptr, {}});
}

QRectF GameUiCanvas::NodePixelRect(const QRectF& screen, std::uint32_t node) const {
    if (!tree_ || node >= tree_->Nodes().size()) return {};
    QRectF result;
    for (const auto& hit : hits_) {
        if (hit.node == node) result = result.isNull() ? hit.rect : result.united(hit.rect);
    }
    const FrameNode& frame = tree_->Nodes()[node];
    if (frame.layout == LayoutState::Placed) {
        const QRectF own = ToPixelRect(screen, tree_->Options(), frame.rect);
        result = result.isNull() ? own : result.united(own);
    }
    return result;
}

void GameUiCanvas::PaintOverlays(QPainter& painter, const QRectF& screen) const {
    const auto& options = tree_->Options();
    painter.save();
    painter.setClipRect(screen.adjusted(-2, -2, 2, 2));
    // 缺贴图的位置始终标出，避免看起来像透明。
    painter.setPen(QPen(UiKit::ToQColor(UiKit::ThemeManager::Theme({}).colors.red), 1.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    for (const auto& hit : hits_) {
        if (hit.image || hit.node >= tree_->Nodes().size()) continue;
        const FrameNode& node = tree_->Nodes()[hit.node];
        if (node.text == std::nullopt && node.codeLabel.empty()) painter.drawRect(hit.rect.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    if (showBounds_) {
        QColor line = palette().color(QPalette::PlaceholderText);
        line.setAlpha(150);
        painter.setPen(QPen(line, 1.0));
        for (const auto& node : tree_->Nodes()) {
            if (node.id == kScreenNode || node.kind != DefinitionKind::Frame || node.layout != LayoutState::Placed) continue;
            painter.drawRect(ToPixelRect(screen, options, node.rect).adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }
    if (showNames_) {
        QFont small = font();
        small.setPixelSize(10);
        painter.setFont(small);
        const QFontMetricsF metrics(small);
        for (const auto& node : tree_->Nodes()) {
            // 代码区域的占位框已写中文名。
            if (node.id == kScreenNode || node.kind != DefinitionKind::Frame || node.name.empty() ||
                !node.codeLabel.empty() || node.layout != LayoutState::Placed) {
                continue;
            }
            const QRectF frame = ToPixelRect(screen, options, node.rect);
            const QString name = QString::fromStdString(node.name);
            const QRectF chip(frame.left() + 1, frame.top() + 1, metrics.horizontalAdvance(name) + 6, metrics.height() + 2);
            painter.fillRect(chip, QColor(0, 0, 0, 170));
            painter.setPen(palette().color(QPalette::WindowText));
            painter.drawText(chip.adjusted(3, 1, -3, -1), Qt::AlignLeft | Qt::AlignVCenter, name);
        }
    }
    if (selected_ != kNoNode) {
        const QRectF selected = NodePixelRect(screen, selected_);
        if (!selected.isNull()) {
            painter.setPen(QPen(palette().color(QPalette::Accent), 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selected.adjusted(1, 1, -1, -1));
        }
    }
    painter.restore();
}

std::uint32_t GameUiCanvas::NodeAt(const QPointF& position) const {
    const auto isCodeArea = [&](const HitBox& hit) {
        return tree_ && hit.node < tree_->Nodes().size() && !tree_->Nodes()[hit.node].codeLabel.empty();
    };
    // 代码区域互相重叠（聊天消息几乎占满中部），压在其上的界面内容优先；只剩代码区域时取最小的一个。
    const HitBox* codeHit = nullptr;
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!it->rect.contains(position)) continue;
        if (isCodeArea(*it)) {
            const double area = it->rect.width() * it->rect.height();
            if (!codeHit || area < codeHit->rect.width() * codeHit->rect.height()) codeHit = &*it;
            continue;
        }
        if (codeHit) return codeHit->node;
        if (!it->image) return it->node;
        const QPointF texel = it->toImage.map(position);
        const int width = it->image->width();
        const int height = it->image->height();
        const int x = ((static_cast<int>(std::floor(texel.x())) % width) + width) % width;
        const int y = ((static_cast<int>(std::floor(texel.y())) % height) + height) % height;
        if (qAlpha(it->image->pixel(x, y)) >= 16) return it->node;
    }
    if (codeHit) return codeHit->node;
    if (!tree_) return kNoNode;
    // 透明处退回到包含该点的最小框架。
    const QRectF screen = ScreenRect();
    std::uint32_t best = kNoNode;
    double bestArea = 0.0;
    for (const auto& node : tree_->Nodes()) {
        if (node.id == kScreenNode || node.kind != DefinitionKind::Frame || node.layout != LayoutState::Placed) continue;
        const QRectF frame = ToPixelRect(screen, tree_->Options(), node.rect);
        const double area = frame.width() * frame.height();
        if (frame.contains(position) && (best == kNoNode || area < bestArea)) {
            best = node.id;
            bestArea = area;
        }
    }
    return best;
}

void GameUiCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    emit NodeClicked(NodeAt(event->position()));
}

void GameUiCanvas::mouseMoveEvent(QMouseEvent* event) {
    const std::uint32_t node = NodeAt(event->position());
    if (!tree_ || node == kNoNode) {
        setToolTip({});
        return;
    }
    const FrameNode& frame = tree_->Nodes()[node];
    const QString name = !frame.codeLabel.empty() ? QString::fromStdString(frame.codeLabel)
        : frame.name.empty()                      ? QString::fromStdString(frame.frameType)
                                                  : QString::fromStdString(frame.name);
    setToolTip(frame.frameType.empty() ? name : QStringLiteral("%1 · %2").arg(name, QString::fromStdString(frame.frameType)));
}

} // namespace War3Map::Modules
