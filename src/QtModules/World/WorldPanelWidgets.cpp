// 世界视图各面板共用的小控件：面板底色容器、分组标题、分段选择。
#include "WorldPanelWidgets.h"
#include "Widgets/Motion.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

namespace War3Map::Modules::WorldUi {

// 面板内的容器用面板底色（全局样式表把普通 QWidget 画成页面底色）。
QWidget* Surface(QWidget* parent) {
    auto* widget = new QWidget(parent);
    widget->setProperty("worldPanel", true);
    return widget;
}

QLabel* SectionLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("worldSectionLabel"));
    return label;
}

void Repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

// 分段选择：一组互斥按钮，底块随选中项滑动。
QFrame* Segment(QWidget* parent, QButtonGroup* group, std::initializer_list<std::pair<QString, int>> items, const char* name) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("worldSegment"));
    auto* row = new QHBoxLayout(frame);
    row->setContentsMargins(2, 2, 2, 2); row->setSpacing(2);
    auto* highlight = new UiKit::SlidingHighlight(frame);
    highlight->setObjectName(QStringLiteral("worldSegmentIndicator"));
    int index = 0;
    for (const auto& [text, id] : items) {
        auto* button = new QPushButton(text, frame);
        button->setObjectName(QLatin1String(name) + QString::number(index++));
        button->setProperty("segmentItem", true);
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        group->addButton(button, id);
        row->addWidget(button, 1);
    }
    QObject::connect(group, &QButtonGroup::idToggled, highlight, [group, highlight](int, bool on) {
        if (on) highlight->Track(group->checkedButton());
    });
    return frame;
}

} // namespace War3Map::Modules::WorldUi
