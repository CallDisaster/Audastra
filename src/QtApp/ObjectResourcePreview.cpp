#include "ObjectResourcePreview.h"
#include "SoundPreviewPlayer.h"
#include "WorldModelPreview.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/Render/MdxModel.h"
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <mutex>
#include <exception>

namespace War3Map::QtApp {
namespace {
QString U8(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QString Notes(const std::vector<Diagnostic>& notes) {
    QStringList lines; for (const auto& note : notes) { if (lines.size() >= 4) break; lines.push_back(U8(note.message)); }
    return lines.join('\n');
}
struct Reader {
    Result<Modules::WorldTerrainSources> sources;
    std::unique_ptr<Modules::WorldResourceReader> reader;
};
struct PreviewData {
    QString source, path, error;
    QImage image;
    std::vector<std::byte> sound;
};
PreviewData ReadPreview(Reader& source, const QString& value, Services::ObjectResourceKind kind) {
    PreviewData result;
    if (!source.sources.Succeeded()) { result.error = Notes(source.sources.diagnostics); return result; }
    if (!source.reader) {
        auto opened = Modules::WorldResourceReader::Open(*source.sources.value);
        if (!opened.Succeeded()) { result.error = Notes(opened.diagnostics); return result; }
        source.reader = std::move(*opened.value);
    }
    const auto before = source.reader->VerifyCurrentSource();
    if (!before.Succeeded()) { result.error = Notes(before.diagnostics); return result; }
    auto& resolver = source.reader->Resolver();
    std::string path = value.toStdString();
    if (kind == Services::ObjectResourceKind::UnitSoundSet) {
        result.error = QObject::tr("单位声音组含多个事件；保留组名，暂不选择其中一个事件试听。"); return result;
    }
    if (kind == Services::ObjectResourceKind::Model) {
        path = Render::MdxModelFile(path, 0, 1);
        auto located = resolver.Locate(path);
        if (!located.Succeeded()) { result.error = Notes(located.diagnostics); return result; }
        result.source = U8(located.value->sourceId); result.path = U8(located.value->sourcePath);
    } else {
        if (kind == Services::ObjectResourceKind::SoundLabel) {
            auto info = Services::InspectObjectResource(resolver, path, kind);
            if (!info.Succeeded()) { result.error = Notes(info.diagnostics); return result; }
            path = info.value->resourcePath;
        }
        auto resource = resolver.Read(path);
        if (!resource.Succeeded()) { result.error = Notes(resource.diagnostics); return result; }
        result.source = U8(resource.value->location.sourceId); result.path = U8(resource.value->location.sourcePath);
        if (kind == Services::ObjectResourceKind::Icon || kind == Services::ObjectResourceKind::Texture) {
            auto image = Image::DecodeTexture(path, resource.value->bytes);
            if (!image.Succeeded()) { result.error = Notes(image.diagnostics); return result; }
            const auto& pixels = *image.value;
            result.image = QImage(reinterpret_cast<const uchar*>(pixels.rgba.data()), static_cast<int>(pixels.width),
                static_cast<int>(pixels.height), static_cast<int>(pixels.width * 4), QImage::Format_RGBA8888).copy();
        } else result.sound = std::move(resource.value->bytes);
    }
    const auto after = source.reader->VerifyCurrentSource();
    if (!after.Succeeded()) { result = {}; result.error = Notes(after.diagnostics); }
    return result;
}

class ResourcePreview final : public QWidget {
public:
    ResourcePreview(QWidget* parent, Result<Modules::WorldTerrainSources> sources, std::string revision,
        std::uint64_t generation, std::uint64_t editRevision, std::function<bool()> current)
        : QWidget(parent), reader_(std::make_shared<Reader>()), sources_(std::move(sources)), revision_(std::move(revision)),
          generation_(generation), editRevision_(editRevision), current_(std::move(current)) {
        reader_->sources = sources_;
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
        picture_ = new QLabel(this); picture_->setAlignment(Qt::AlignCenter); picture_->setMinimumSize(160, 128);
        picture_->setObjectName(QStringLiteral("resourceTexturePreview")); layout->addWidget(picture_);
        source_ = new QLabel(this); source_->setTextFormat(Qt::PlainText); source_->setWordWrap(true);
        source_->setObjectName(QStringLiteral("resourcePreviewSource")); layout->addWidget(source_);
        auto* controls = new QHBoxLayout;
        play_ = new QPushButton(tr("试听"), this); play_->setObjectName(QStringLiteral("resourceSoundPlay")); controls->addWidget(play_);
        stop_ = new QPushButton(tr("停止"), this); controls->addWidget(stop_); layout->addLayout(controls);
        play_->hide(); stop_->hide(); layout->addStretch();
        connect(play_, &QPushButton::clicked, this, [this] {
            if (!Current() || sound_.empty()) return;
            if (!audio_) {
                audio_ = new Modules::SoundPreviewPlayer(this);
                connect(audio_, &Modules::SoundPreviewPlayer::Error, this, [this](const QString& error) { source_->setText(error); });
            }
            audio_->PlayBytes(sound_, soundPath_);
        });
        connect(stop_, &QPushButton::clicked, this, [this] { if (audio_) audio_->Stop(); });
    }
    ~ResourcePreview() override { ++serial_; delete audio_; audio_ = nullptr; }
    void Show(const QString& value, Services::ObjectResourceKind kind) {
        ++serial_; pending_ = std::pair{value, kind}; sound_.clear(); play_->setEnabled(false);
        if (audio_) audio_->Stop();
        if (!Current()) { source_->setText(tr("来源已变化，请重新打开资源选择器。")); return; }
        const bool model = kind == Services::ObjectResourceKind::Model;
        picture_->setVisible(!model); picture_->setPixmap({});
        const bool sound = kind == Services::ObjectResourceKind::SoundFile || kind == Services::ObjectResourceKind::SoundLabel;
        play_->setVisible(sound); stop_->setVisible(sound);
        if (model) {
            if (!model_) {
                model_ = new Modules::WorldModelPreview(this, nullptr);
                static_cast<QVBoxLayout*>(layout())->insertWidget(0, model_);
            }
            Render::ModelInstanceRequest request; request.modelPath = Render::MdxModelFile(value.toStdString(), 0, 1);
            model_->ShowSelection(U8(revision_), generation_, editRevision_, {MapData::PlacementFile::Units, 0, "", 0},
                "resource-picker", std::move(request), sources_, current_);
        } else if (model_) model_->ClearSelection();
        source_->setText(tr("正在读取资源…")); Begin();
    }
private:
    bool Current() const { return !current_ || current_(); }
    void Begin() {
        if (worker_ || !pending_ || !Current()) return;
        const auto request = *pending_; pending_.reset();
        const auto serial = serial_; auto result = std::make_shared<PreviewData>();
        auto* worker = QThread::create([reader = reader_, request, result] {
            try { *result = ReadPreview(*reader, request.first, request.second); }
            catch (const std::exception& error) { result->error = QString::fromUtf8(error.what()); }
        });
        worker_ = worker;
        connect(worker, &QThread::finished, this, [this, serial, result] {
            worker_ = nullptr;
            if (serial == serial_ && Current()) {
                if (!result->error.isEmpty()) source_->setText(tr("资源不可用：%1\n保留原值仍可保存。").arg(result->error));
                else {
                    QString text = result->source + QStringLiteral("\n") + result->path;
                    if (!result->image.isNull()) {
                        text += tr("\n%1 × %2").arg(result->image.width()).arg(result->image.height());
                        picture_->setPixmap(QPixmap::fromImage(result->image).scaled(240, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    }
                    source_->setText(text); source_->setToolTip(text);
                    sound_ = std::move(result->sound); soundPath_ = result->path; play_->setEnabled(!sound_.empty());
                }
            }
            Begin();
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start(QThread::LowPriority);
    }
    std::shared_ptr<Reader> reader_;
    Result<Modules::WorldTerrainSources> sources_;
    std::string revision_;
    std::uint64_t generation_, editRevision_, serial_ = 0;
    std::function<bool()> current_;
    std::optional<std::pair<QString, Services::ObjectResourceKind>> pending_;
    QPointer<QThread> worker_;
    Modules::WorldModelPreview* model_ = nullptr;
    Modules::SoundPreviewPlayer* audio_ = nullptr;
    QLabel* picture_;
    QLabel* source_;
    QPushButton* play_;
    QPushButton* stop_;
    std::vector<std::byte> sound_;
    QString soundPath_;
};
}
Modules::ObjectResourcePreviewHandle MakeObjectResourcePreview(QWidget* parent, Result<Modules::WorldTerrainSources> sources,
    std::string revision, std::uint64_t generation, std::uint64_t editRevision, std::function<bool()> current) {
    auto* widget = new ResourcePreview(parent, std::move(sources), std::move(revision), generation, editRevision, std::move(current));
    const QPointer<ResourcePreview> guard(widget);
    return {widget, [guard](const QString& value, auto kind) { if (guard) guard->Show(value, kind); }};
}
}
