#include "SoundPreviewPlayer.h"
#include <QAudioOutput>
#include <QBuffer>
#include <QMediaPlayer>
#include <QUrl>

namespace War3Map::Modules {
SoundPreviewPlayer::SoundPreviewPlayer(QObject* parent) : QObject(parent) {
    output_ = new QAudioOutput(this); buffer_ = new QBuffer(this); player_ = new QMediaPlayer(this);
    player_->setAudioOutput(output_);
    connect(player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& message) {
        if (!message.isEmpty()) emit Error(message);
    });
}
SoundPreviewPlayer::~SoundPreviewPlayer() {
    QObject::disconnect(player_, nullptr, this, nullptr);
    Stop(); delete player_; player_ = nullptr; // QMediaPlayer 必须先释放对 buffer 的引用。
}
bool SoundPreviewPlayer::PlayBytes(std::span<const std::byte> bytes, const QString& path) {
    Stop();
    if (bytes.size() > 64u * 1024u * 1024u) { emit Error(tr("声音超过64MiB试听上限")); return false; }
    buffer_->setData(QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
    if (!buffer_->open(QIODevice::ReadOnly)) { emit Error(tr("无法打开试听数据")); return false; }
    player_->setSourceDevice(buffer_, QUrl(QStringLiteral("file:///preview.%1").arg(path.section('.', -1).toLower())));
    player_->play(); return true;
}
void SoundPreviewPlayer::PlayFile(const QString& path) { Stop(); player_->setSource(QUrl::fromLocalFile(path)); player_->play(); }
void SoundPreviewPlayer::Stop() { if (player_) { player_->stop(); player_->setSource({}); } if (buffer_) buffer_->close(); }
}
