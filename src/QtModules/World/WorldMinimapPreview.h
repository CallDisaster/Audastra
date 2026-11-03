#pragma once
#include <QWidget>
#include <QPointer>
#include <QString>
class QLabel;
class QPushButton;
class QThread;
namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::Modules {
// 只显示实际归档底图，独立于3D provider；worker只拥有值路径/SHA及解码结果。
class WorldMinimapPreview final : public QWidget {
public:
    explicit WorldMinimapPreview(QtProject::EditorProject& project, QWidget* parent = nullptr);
    ~WorldMinimapPreview() override;
private:
    void Refresh();
    void UpdateStatus();
    void Clear(const QString& reason);
    QtProject::EditorProject& project_;
    QLabel* image_ = nullptr;
    QLabel* state_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPointer<QThread> worker_;
    QString loadedSha_;
    bool pending_ = false;
    bool present_ = false;
};
} // namespace War3Map::Modules
