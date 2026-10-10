#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace War3Map::Shell {

// 纯视图数据；QtShell 不依赖工程存储，也不读取地图或配置。
struct RecentMapListEntry {
    QString path;
    QDateTime lastOpenedUtc;
    bool pinned = false;
    bool available = false;
};

class RecentMapsList final : public QWidget {
    Q_OBJECT
public:
    explicit RecentMapsList(QWidget* parent = nullptr);
    [[nodiscard]] bool SetEntries(const QVector<RecentMapListEntry>& entries);
    void SetStatus(const QString& text);

signals:
    void OpenRequested(const QString& path);
    void PinRequested(const QString& path, bool pinned);
    void RemoveRequested(const QString& path);

private:
    QVBoxLayout* rows_ = nullptr;
    QLabel* empty_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace War3Map::Shell
