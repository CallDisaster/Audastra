#pragma once

#include <QWidget>

class QAction;

namespace War3Map::Shell {

// 无项目时的轻量入口；动作及业务状态由宿主持有，不读取配置或地图。
class StartupPage final : public QWidget {
    Q_OBJECT

public:
    explicit StartupPage(QAction* openMapAction, QAction* createMapAction,
                         QWidget* parent = nullptr);
};

} // namespace War3Map::Shell
