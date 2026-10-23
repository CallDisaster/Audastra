#pragma once

#include "War3MapCore/GameUi/FrameDraw.h"

#include <QHash>
#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

namespace War3Map::Modules {

// 游戏界面画布：0.8×0.6 的界面坐标铺满所选宽高比的游戏画面（经典版在宽屏下横向拉伸），
// 按绘制清单画贴图与文字。只显示，不修改界面树。
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

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;

private:
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
    void PaintOverlays(QPainter& painter, const QRectF& screen) const;
    [[nodiscard]] QRectF NodePixelRect(const QRectF& screen, std::uint32_t node) const;

    const GameUi::FrameTree* tree_ = nullptr;
    GameUi::FrameDrawList list_;
    const QHash<QString, QImage>* textures_ = nullptr;
    const QHash<QString, QString>* families_ = nullptr;
    double aspect_ = 16.0 / 9.0;
    bool showContent_ = true;
    bool showBounds_ = false;
    bool showNames_ = false;
    std::uint32_t selected_ = GameUi::kNoNode;
    mutable std::vector<HitBox> hits_;
};

} // namespace War3Map::Modules
