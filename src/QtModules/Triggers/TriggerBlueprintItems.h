#pragma once

#include "TriggerModels.h"

#include "War3MapCore/Types.h"

#include <QColor>
#include <QGraphicsItem>
#include <QPainterPath>
#include <QString>
#include <QImage>
#include <QHash>
#include <QUrl>

#include <vector>

class QGraphicsTextItem;

// 蓝图的节点与端口图元（对应虚幻蓝图的执行引脚）。纵向执行流：
//   输入 ▼（节点顶部中央）— 这条语句从这里开始执行；
//   下一步 ▼（底部中央）— 这条语句之后；
//   分支 ▶（右侧，逐行）— 块语句的“则 / 否则 / 循环 / 参数 …”；
//   输出 ▶（右侧标题行）— 事件与触发条件节点通向动作链。
// 每个端口记着“接到这里的语句放在哪里”（所在分支 + 插在谁之前），拖线与新建都按它执行。
namespace TriggerBlueprintDetail {

enum class PinKind { In, Next, Branch, Out };

struct PinTarget {
    EcaSlot slot;
    War3Map::StableId before = 0; // 0 = 分支末尾
    QString label;                // 菜单文字用的位置名；为空表示“在之后”
};

class BlueprintNode;

class BlueprintPin final : public QGraphicsItem {
public:
    BlueprintPin(BlueprintNode* owner, PinKind kind, QString label, QColor color, PinTarget target);

    [[nodiscard]] QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

    [[nodiscard]] PinKind Kind() const noexcept { return kind_; }
    [[nodiscard]] const QString& Label() const noexcept { return label_; }
    [[nodiscard]] const PinTarget& Target() const noexcept { return target_; }
    [[nodiscard]] BlueprintNode* Owner() const noexcept { return owner_; }
    [[nodiscard]] QColor Color() const noexcept { return color_; }
    [[nodiscard]] bool IsOutput() const noexcept { return kind_ != PinKind::In; }
    // 连线离开 / 进入端口的方向（纵向端口向下，右侧端口向右）。
    [[nodiscard]] QPointF Direction() const;
    void SetConnected(bool connected);

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    BlueprintNode* owner_ = nullptr;
    PinKind kind_ = PinKind::In;
    QString label_;
    QColor color_;
    PinTarget target_;
    bool connected_ = false;
    bool hovered_ = false;
};

// 一个语句节点：色带标题 + 自然语言正文（参数为链接）+ 端口。位置由结构自动布局。
class BlueprintNode final : public QGraphicsItem {
public:
    // id 为 0 的节点是触发条件等非语句节点。
    BlueprintNode(War3Map::StableId id, std::int32_t ecaType, QString title, QString kind, QColor color, bool enabled);

    void SetBody(const QString& html, const QColor& text);
    void AddImage(const QUrl& url, const QImage& image);
    void SetArgumentTooltip(std::size_t position, QString tooltip);
    BlueprintPin* AddPin(PinKind kind, const QString& label, const QColor& color, const PinTarget& target);
    [[nodiscard]] QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

    // 场景坐标处的链接（arg:N / cond）。
    [[nodiscard]] QString AnchorAt(const QPointF& scenePos) const;
    [[nodiscard]] War3Map::StableId Id() const noexcept { return id_; }
    [[nodiscard]] std::int32_t EcaType() const noexcept { return ecaType_; }
    [[nodiscard]] QString Text() const;
    [[nodiscard]] QRectF ArgumentRect(std::size_t position) const;
    [[nodiscard]] const std::vector<BlueprintPin*>& Pins() const noexcept { return pins_; }
    [[nodiscard]] BlueprintPin* FindPin(PinKind kind, const QString& label = {}) const;
    // 右键菜单里“添加语句到…”的位置（在之后 / 各分支末尾）。
    void AddMenuTarget(PinTarget target) { menuTargets_.push_back(std::move(target)); }
    [[nodiscard]] const std::vector<PinTarget>& MenuTargets() const noexcept { return menuTargets_; }

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    void LayoutPins();

    War3Map::StableId id_ = 0;
    std::int32_t ecaType_ = 2;
    QString title_;
    QString kind_;
    QColor color_;
    bool enabled_ = true;
    double bodyHeight_ = 24;
    QGraphicsTextItem* body_ = nullptr;
    std::vector<BlueprintPin*> pins_;
    std::vector<PinTarget> menuTargets_;
    QHash<QString, QString> argumentTooltips_;
};

// 执行线：从端口沿其方向出发，沿 arriveDirection 进入终点（贝塞尔曲线）。
[[nodiscard]] QPainterPath ExecWire(const QPointF& from, const QPointF& leaveDirection, const QPointF& to, const QPointF& arriveDirection);

constexpr double kNodeWidth = 300;
constexpr double kNodeHeader = 26;
constexpr double kBranchRow = 22;

} // namespace TriggerBlueprintDetail
