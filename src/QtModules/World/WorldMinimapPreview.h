#pragma once
#include "ProjectMinimapPreview.h"
#include <QWidget>
#include <QPointF>
#include <QPointer>
#include <QString>
class QLabel;
class QPushButton;
class QThread;
class QTabBar;
class QToolButton;
namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::Modules {
class WorldViewport;
class MinimapCanvas;
class WorldMinimapPreview final : public QWidget {
public:
    explicit WorldMinimapPreview(QtProject::EditorProject& project, QWidget* parent = nullptr, WorldViewport* viewport = nullptr);
private:
    void Refresh(bool force = false);
    void Display();
    void Pan(QPointF normalized);
    void Outline();
    QtProject::EditorProject& project_;
    QtProject::ProjectMinimapPreview preview_;
    QPointer<WorldViewport> viewport_;
    MinimapCanvas* image_ = nullptr;
    QLabel* state_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QTabBar* source_ = nullptr;
    QToolButton* layers_ = nullptr;
    QtProject::MinimapPreviewOptions options_;
    std::uint64_t displayedGeneration_ = 0;
    bool displayedCurrent_ = false;
};
} // namespace War3Map::Modules
