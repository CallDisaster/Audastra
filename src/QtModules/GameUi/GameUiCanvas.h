#pragma once

#include "War3MapCore/GameUi/FrameDraw.h"
#include "War3MapCore/GameUi/UiUnits.h"

#include <QHash>
#include <QImage>
#include <QLineF>
#include <QString>
#include <QWidget>

class QTimer;

#include <cstdint>
#include <optional>
#include <vector>

namespace War3Map::Modules {

// 游戏界面画布：0.8×0.6 的界面坐标铺满所选宽高比的游戏画面（经典版在宽屏下横向拉伸），
// 按绘制清单画贴图与文字。不修改界面树：可拖动的选中节点拖动时只画虚线预览，松开后发出位移由调用方处理。
class GameUiCanvas final : public QWidget {
    Q_OBJECT
public:
    explicit GameUiCanvas(QWidget* parent = nullptr);

    // tree 由调用方持有，在下一次 SetScene 前保持有效。
    void SetScene(const GameUi::FrameTree* tree, GameUi::FrameDrawList list);
    void ClearScene();
    // 键为 GameUiAssetKey(路径)。
    void SetTextures(const QHash<QString, QImage>* textures);
    void SetFontFamilies(const QHash<QString, QString>* families);
    void SetAspect(double aspect);
    void SetShowContent(bool show);
    void SetShowBounds(bool show);
    void SetShowNames(bool show);
    void SetSelected(std::uint32_t node);
    // 选中节点能否拖动（自有界面描述的框架）。
    void SetDraggable(bool draggable);
    // 放大到该区域（屏幕单位）铺满控件，留出边距；空为显示整个画面。用于类型预览。
    void SetFocusRect(std::optional<GameUi::FrameRect> rect);
    // 画布上能点中的节点（下标为节点编号）；空为全部能点中。不能点中的节点照常绘制，
    // 点击、右键与悬停穿过它落到后面能点中的节点或空白处。
    void SetPickable(std::vector<bool> pickable);
    [[nodiscard]] bool Pickable(std::uint32_t node) const noexcept {
        return pickable_.empty() || (node < pickable_.size() && pickable_[node]);
    }
    // 选中节点能否拖控制柄改大小（自己界面的控件）。
    void SetResizable(bool resizable);
    // 标注与微调用的长度单位：像素时按参考分辨率换算（方向键一次 1 像素，Shift 10 像素）。
    void SetMeasure(bool pixels, GameUi::UiResolution resolution);
    void SetShowGrid(bool show);
    // 参考图：铺满游戏画面、半透明画在界面上面，用来对照设计稿；空图为不显示。
    void SetReferenceImage(const QImage& image, double opacity);
    [[nodiscard]] bool ShowGrid() const noexcept { return showGrid_; }
    [[nodiscard]] double Zoom() const noexcept { return zoom_; }
    // 视图缩放（滚轮，以光标为中心）与平移（中键或按住空格拖动）；适应窗口回到整个画面。
    void FitView();

    [[nodiscard]] double Aspect() const noexcept { return aspect_; }
    [[nodiscard]] std::uint32_t Selected() const noexcept { return selected_; }
    // 游戏画面在控件中的像素区域。
    [[nodiscard]] QRectF ScreenRect() const;
    // 该位置最上层可见的节点；没有时为 kNoNode。
    [[nodiscard]] std::uint32_t NodeAt(const QPointF& position) const;
    // 按指定像素尺寸离屏绘制（不含边框、名称与选中标记），供截图与测试。
    [[nodiscard]] QImage RenderImage(const QSize& size) const;

signals:
    void NodeClicked(std::uint32_t node);
    // 拖动松开或方向键微调：位移为屏幕单位（右、上为正），已取到 0.0001。
    void NodeDragged(std::uint32_t node, double dx, double dy);
    // 拖控制柄松开：该实例的新矩形（屏幕单位）。
    void NodeResized(std::uint32_t node, GameUi::FrameRect rect);
    // 右键：所点节点（没有为 kNoNode）、所点位置（屏幕单位，左下为原点）与全局像素位置。
    void ContextMenuRequested(std::uint32_t node, QPointF point, QPoint global);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    enum class DragMode : std::uint8_t { None, Move, Resize };
    // 控制柄：0 左上、1 上、2 右上、3 右、4 右下、5 下、6 左下、7 左。
    [[nodiscard]] int HandleAt(const QPointF& position) const;
    [[nodiscard]] std::vector<QPointF> HandlePoints(const QRectF& box) const;
    void UpdateCursor(const QPointF& position);
    // 吸附：拖动开始时收集候选线（像素）；移动时让边或中线贴近候选线，并记下参考线。
    void CollectSnapLines(std::uint32_t node);
    [[nodiscard]] double SnapValue(double value, bool horizontal, double* snapped) const;
    void UpdateDragPreview(const QPointF& position, Qt::KeyboardModifiers modifiers);
    void CommitNudge();
    void CancelDrag();
    [[nodiscard]] double GridStep(const QRectF& screen) const;  // 网格间距（像素）
    [[nodiscard]] QString MeasureText(double units, bool horizontal) const;
    void PaintMeasures(QPainter& painter, const QRectF& screen, const QRectF& box) const;
    struct HitBox {
        std::uint32_t node = GameUi::kNoNode;
        QRectF rect;
        const QImage* image = nullptr;
        QTransform toImage;  // 控件像素 → 贴图像素，用于按透明度判断是否点中
    };

    void PaintScene(QPainter& painter, const QRectF& screen, bool overlays, std::vector<HitBox>* hits) const;
    void PaintImage(QPainter& painter, const QRectF& screen, const GameUi::FrameDrawCommand& command,
        std::vector<HitBox>* hits) const;
    void PaintText(QPainter& painter, const QRectF& screen, const GameUi::FrameDrawCommand& command,
        std::vector<HitBox>* hits) const;
    void PaintCodeArea(QPainter& painter, const QRectF& screen, const GameUi::FrameDrawCommand& command,
        std::vector<HitBox>* hits) const;
    void PaintOverlays(QPainter& painter, const QRectF& screen) const;
    [[nodiscard]] QRectF NodePixelRect(const QRectF& screen, std::uint32_t node) const;
    [[nodiscard]] QRectF SelectionPixelRect(const QRectF& screen) const;

    const GameUi::FrameTree* tree_ = nullptr;
    GameUi::FrameDrawList list_;
    const QHash<QString, QImage>* textures_ = nullptr;
    const QHash<QString, QString>* families_ = nullptr;
    double aspect_ = 16.0 / 9.0;
    bool showContent_ = true;
    bool showBounds_ = false;
    bool showNames_ = false;
    std::uint32_t selected_ = GameUi::kNoNode;
    std::uint32_t hovered_ = GameUi::kNoNode;
    std::optional<GameUi::FrameRect> focus_;
    bool draggable_ = false;
    bool resizable_ = false;
    std::uint32_t dragNode_ = GameUi::kNoNode;  // 按下时可拖动的节点
    DragMode dragMode_ = DragMode::None;
    int dragHandle_ = -1;
    QPointF dragStart_;
    QRectF dragOrigin_;   // 按下时节点的像素矩形
    QRectF dragPreview_;  // 拖动中的新矩形（像素，已吸附）
    bool dragging_ = false;  // 超过阈值后才算拖动
    std::vector<double> snapXs_;
    std::vector<double> snapYs_;
    std::vector<QLineF> guides_;
    std::vector<bool> pickable_;
    mutable std::vector<HitBox> hits_;
    bool measurePixels_ = false;
    GameUi::UiResolution resolution_;
    bool showGrid_ = false;
    QImage reference_;
    double referenceOpacity_ = 0.5;
    double zoom_ = 1.0;
    QPointF pan_;  // 相对适应窗口的平移（像素）
    bool spaceHeld_ = false;
    bool panning_ = false;
    QPointF panStart_;
    QPointF panOrigin_;
    std::uint32_t nudgeNode_ = GameUi::kNoNode;
    QPointF nudge_;  // 累计的方向键位移（屏幕单位，上为正）；停下一会儿再一次提交
    QTimer* nudgeTimer_ = nullptr;
};

} // namespace War3Map::Modules
