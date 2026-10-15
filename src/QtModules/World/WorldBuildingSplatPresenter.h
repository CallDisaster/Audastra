#pragma once

#include "WorldPlacementModels.h"
#include "WorldTerrainAssets.h"
#include "War3MapCore/Render/BuildingSplatPreview.h"
#include "War3MapCore/Services/WorldInstanceRegistry.h"

#include <QObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QJsonObject>

#include <map>

namespace War3Map::Modules {
class WorldViewport;

// 一个聚合辅助层；普通镜头/动画tick不触发准备。目录、BLP、查询和网格在
// 单一低优先级worker中处理，UI接收只验证冻结身份并提交既有generic层事务。
class WorldBuildingSplatPresenter final : public QObject {
    Q_OBJECT
public:
    explicit WorldBuildingSplatPresenter(WorldViewport& viewport, QObject* parent = nullptr);
    ~WorldBuildingSplatPresenter() override;
    void SetSource(WorldTerrainSources sources, Render::AnimationSession session,
        std::function<bool()> sourceCurrent);
    void Show(WorldBuildingSplatCapture capture, std::vector<std::uint8_t> visible,
        std::shared_ptr<const MapDocument> document,
        std::shared_ptr<const Services::WorldInstanceRegistry> registry,
        Services::WorldInstanceBinding fallbackBinding,
        std::shared_ptr<const MapData::TerrainGridSnapshot> displayedTerrain,
        std::string terrainIdentity, std::uint64_t terrainRevision, std::function<bool()> current);
    void Clear();
    void RebindParent();
    void SetSubmissionDeferred(bool deferred);
    [[nodiscard]] std::uint64_t LayerId() const noexcept { return layerId_; }
    [[nodiscard]] bool DiagnosticPreparationReady() const {
        return !worker_ && capture_ && prepared_ && preparedRequestSerial_ == requestSerial_
            && current_ && current_() && sourceCurrent_ && sourceCurrent_();
    }
    [[nodiscard]] QJsonObject DiagnosticSnapshot() const;
    // 进度查询沿服务输入游标取一页；总数是已捕获输入数，不伪称全量故障列表。
    [[nodiscard]] QJsonObject DiagnosticSnapshot(std::uint32_t offset, std::uint32_t limit) const;
    [[nodiscard]] std::vector<Services::BuildingSplatRow> DiagnosticRows() const;
signals:
    void Message(const QString& text);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Work;
    struct Cache;
    struct Signature {
        std::uint64_t source = 0, request = 0, binding = 0, parent = 0;
        bool operator==(const Signature&) const = default;
    };
    [[nodiscard]] Signature CurrentSignature() const;
    void BeginNext();
    void SubmitPrepared(const Signature& signature);
    void RemoveLayer();
    void CheckParent();
    void UpdateTimer();
    void Report(QString text);
    [[nodiscard]] bool DiagnosticRowUnavailable(Services::BuildingSplatRow& row) const;
    [[nodiscard]] QJsonObject DiagnosticSnapshotImpl(std::optional<std::uint32_t> offset, std::uint32_t limit) const;
    QPointer<WorldViewport> viewport_;
    QPointer<QThread> worker_;
    QTimer parentTimer_;
    std::optional<WorldTerrainSources> sources_;
    Render::AnimationSession session_;
    std::function<bool()> sourceCurrent_, current_;
    std::shared_ptr<Cache> cache_;
    std::shared_ptr<const WorldBuildingSplatCapture> capture_;
    std::vector<std::uint8_t> visible_;
    std::shared_ptr<const MapDocument> document_;
    std::shared_ptr<const Services::WorldInstanceRegistry> registry_;
    Services::WorldInstanceBinding fallbackBinding_;
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain_;
    std::string terrainIdentity_, desiredIdentity_, geometryIdentity_, preparedGeometryIdentity_;
    std::size_t hiddenPrepared_ = 0;
    std::size_t notApplicableCount_ = 0;
    std::uint64_t terrainRevision_ = 0;
    std::optional<Render::BuildingSplatPreviewFrame> prepared_, installed_;
    std::optional<Signature> failed_;
    QString lastMessage_;
    std::uint64_t sourceSerial_ = 0, requestSerial_ = 0, bindingSerial_ = 0;
    std::uint64_t preparedRequestSerial_ = 0, observedParent_ = 0;
    bool needsSubmit_ = false, submitted_ = false, submissionDeferred_ = false, removalPending_ = false;
    const std::uint64_t layerId_;
};
} // namespace War3Map::Modules
