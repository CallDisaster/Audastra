#pragma once

#include "TriggerBlueprintItems.h"

#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Types.h"

#include <QGraphicsView>
#include <QIcon>

#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

class QGraphicsPathItem;
class QGraphicsScene;
class QLabel;

// 蓝图视图（对应原型 TriggerCanvas）。WTG 不保存节点坐标，蓝图按语句结构自动布局：
// 事件 → 触发条件 → 动作链纵向排列；块语句的每个分支（则 / 否则 / 循环 / 参数 / 条件）是节点右侧的输出端口，
// 分支内的语句向右展开。没有事件与触发条件的触发器有一个“开始”节点。
// 端口像虚幻蓝图的执行引脚：从输出端口拉线到语句 = 把它移到这里；拉到空白处或单击 = 在这里新建语句；
// 从语句的输入端口拉到其他输出端口 = 把这条语句移过去。所有编辑经模块的语句操作，可撤销。
class TriggerBlueprint final : public QGraphicsView {
public:
    using PinTarget = TriggerBlueprintDetail::PinTarget;
    struct Callbacks {
        std::function<void(War3Map::StableId ecaId)> selected;
        // 点击节点里的参数记号：参数序号与记号附近的全局矩形。
        std::function<void(War3Map::StableId ecaId, std::size_t position, const QRect& anchor)> argument;
        // Scalar arguments edit inside the node. Returning false keeps the full value panel.
        std::function<bool(War3Map::StableId ecaId, std::size_t position, const QRect& anchor)> editScalar;
        // 双击节点的非记号区域。
        std::function<void(War3Map::StableId ecaId)> activated;
        // 右键：语句节点（ecaId）、端口或空白处（ecaId 为 0）；targets 是可以添加语句的位置。
        std::function<void(War3Map::StableId ecaId, const std::vector<PinTarget>& targets, const QPoint& globalPos)> contextMenu;
        std::function<void()> showConditions;
        // 端口连线：把语句 ecaId 移到 target。
        std::function<void(War3Map::StableId ecaId, const PinTarget& target)> connect;
        // 拉到空白处 / 单击端口：在 target 新建语句。
        std::function<void(const PinTarget& target)> create;
    };

    explicit TriggerBlueprint(QWidget* parent = nullptr);
    ~TriggerBlueprint() override;

    void SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }
    void SetIconResolver(std::function<QIcon(std::string_view)> resolver) { iconResolver_ = std::move(resolver); }
    void SetSource(const War3Map::Trigger::TriggerDocument* document, const War3Map::Trigger::UiCatalog* catalog,
        War3Map::Trigger::PresentationOptions options);
    // 按当前文档重建布局；保持缩放，尽量保持视口中心。
    void ShowTrigger(War3Map::StableId triggerId);
    void Select(War3Map::StableId ecaId, bool center = true);
    [[nodiscard]] War3Map::StableId SelectedEca() const;
    void Fit();
    void ZoomBy(double factor);

    // 测试与探针。
    [[nodiscard]] std::size_t NodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] QRectF NodeRect(War3Map::StableId ecaId) const;
    [[nodiscard]] std::size_t WireCount() const noexcept { return wires_; }
    [[nodiscard]] QString NodeText(War3Map::StableId ecaId) const;
    [[nodiscard]] QPoint ArgumentViewPos(War3Map::StableId ecaId, std::size_t position) const;
    // 触发条件节点的文字（没有触发条件时为空）。
    [[nodiscard]] QString ConditionText() const;
    // 节点的端口：“输入”“下一步”“输出”或分支名。ecaId 为 0 时取开始节点（没有则取触发条件节点）。
    [[nodiscard]] QStringList PinLabels(War3Map::StableId ecaId) const;
    // 端口在视口中的位置（找不到时为 (-1, -1)）。
    [[nodiscard]] QPoint PinViewPos(War3Map::StableId ecaId, const QString& label) const;
    // 右键菜单会提供的添加位置（测试用，与 contextMenuEvent 一致）。
    [[nodiscard]] std::vector<PinTarget> MenuTargets(War3Map::StableId ecaId) const;

protected:
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    using Node = TriggerBlueprintDetail::BlueprintNode;
    using Pin = TriggerBlueprintDetail::BlueprintPin;
    struct Placed {
        double bottom = 0;
        Node* first = nullptr;
        Node* last = nullptr;
    };
    Node* AddNode(std::uint32_t nodeIndex, double x, double y);
    Node* AddConditionNode(const std::vector<std::uint32_t>& conditions, double x, double y);
    Node* AddStartNode(const QString& name, const std::vector<EcaSlot>& roots, War3Map::StableId firstAction);
    // chain 是同一位置（slot）内按顺序的语句；每个节点带输入 / 下一步端口，块语句带分支端口。
    Placed LayoutChain(const std::vector<std::uint32_t>& chain, const EcaSlot& slot, int column, double top, std::size_t depth);
    void AddWire(Pin* from, Pin* to, const QColor& color);
    [[nodiscard]] Node* NodeAt(const QPoint& viewPos) const;
    [[nodiscard]] Pin* PinAt(const QPoint& viewPos) const;
    [[nodiscard]] Node* ProbeNode(War3Map::StableId ecaId) const;
    [[nodiscard]] std::vector<PinTarget> RootTargets() const;
    void FinishPinDrag(const QPoint& viewPos);
    void UpdateZoomLabel();

    QGraphicsScene* scene_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    Node* conditionNode_ = nullptr;
    Node* startNode_ = nullptr;
    const War3Map::Trigger::TriggerDocument* document_ = nullptr;
    const War3Map::Trigger::UiCatalog* catalog_ = nullptr;
    War3Map::Trigger::PresentationOptions options_;
    War3Map::StableId triggerId_ = 0;
    std::unordered_map<War3Map::StableId, Node*> nodes_;
    std::size_t wires_ = 0;
    bool fitted_ = false;
    bool selecting_ = false;
    // 正在从端口拉线。
    Pin* dragPin_ = nullptr;
    QPoint dragStart_;
    QGraphicsPathItem* dragWire_ = nullptr;
    Callbacks callbacks_;
    std::function<QIcon(std::string_view)> iconResolver_;
};
