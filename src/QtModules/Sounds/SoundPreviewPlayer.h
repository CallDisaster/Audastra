#pragma once
#include <QObject>
#include <span>
#include <cstddef>

class QMediaPlayer;
class QAudioOutput;
class QBuffer;

namespace War3Map::Modules {
// 声音模块与资源选择器共用同一试听实现；调用者显式点击后才播放。
class SoundPreviewPlayer final : public QObject {
    Q_OBJECT
public:
    explicit SoundPreviewPlayer(QObject* parent = nullptr);
    ~SoundPreviewPlayer() override;
    bool PlayBytes(std::span<const std::byte> bytes, const QString& path);
    void PlayFile(const QString& path);
    void Stop();
signals:
    void Error(const QString& message);
private:
    QMediaPlayer* player_;
    QAudioOutput* output_;
    QBuffer* buffer_;
};
}
