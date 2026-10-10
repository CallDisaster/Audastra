#pragma once

#include <QStyledItemDelegate>

#include <cstddef>
#include <functional>
#include <optional>

// 传统视图的语句行：参数画成可点击的记号（已设值为强调色，空值为红色波浪线，函数提供的值带下划线），
// 点击记号打开取值面板。对应原型 TraditionalView 的 ValueToken。
class TriggerSentenceDelegate final : public QStyledItemDelegate {
public:
    // index 为语句行，position 为参数在 EcaNode::arguments 中的序号，anchor 为记号的全局矩形。
    using Activate = std::function<void(const QModelIndex& index, std::size_t position, const QRect& anchor)>;

    explicit TriggerSentenceDelegate(QObject* parent = nullptr);
    void SetActivate(Activate activate) { activate_ = std::move(activate); }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
        const QModelIndex& index) override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
        const QModelIndex& index) override;

    struct Token {
        std::size_t position = 0;
        QRect rect; // 视口坐标
    };
    // 视口坐标中 point 处的参数记号。
    [[nodiscard]] std::optional<Token> TokenAt(const QStyleOptionViewItem& option, const QModelIndex& index,
        const QPoint& point) const;
    // 第 ordinal 个记号（按显示顺序），用于键盘 Enter 编辑首个参数。
    [[nodiscard]] std::optional<Token> TokenByOrder(const QStyleOptionViewItem& option, const QModelIndex& index,
        std::size_t ordinal) const;

private:
    struct Layout;
    [[nodiscard]] Layout Measure(const QStyleOptionViewItem& option, const QModelIndex& index) const;

    Activate activate_;
};
