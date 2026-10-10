#pragma once

#include "WorldTerrainAssets.h"
#include "War3MapCore/Render/AnimationRenderer.h"

#include <QImage>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

namespace War3Map::Modules {

struct WorldAssetBinding {
    QString mapKey;
    std::uint64_t generation = 0, revision = 0;
    [[nodiscard]] bool operator==(const WorldAssetBinding&) const noexcept = default;
};

struct WorldThumbnailResult {
    QImage image;
    QString issue;
    std::string imageSha256;
    std::optional<Render::AnimationCaptureReceipt> receipt;
    std::vector<Render::AnimationPoseLink> poses;
    std::vector<War3RenderCore::AssetSource> sources;
};

// 一个后台所有者、真实完成帧、有界驻留与内存LRU。没有跨renderer资产共享承诺。
class WorldThumbnailCache final : public QObject {
    Q_OBJECT
public:
    explicit WorldThumbnailCache(const War3RenderPluginApiV2* backend, QObject* parent = nullptr, bool loadProductionBackend = false);
    ~WorldThumbnailCache() override;
    void SetContext(WorldAssetBinding binding, Result<WorldTerrainSources> sources, std::function<bool()> current);
    void Clear();
    // 返回内容请求key；失败也有可查询的明确诊断。只接受128/256物理像素，固定Stand/0ms。
    [[nodiscard]] QString Request(Render::ModelInstanceRequest request, int pixels = 128);
    [[nodiscard]] std::optional<WorldThumbnailResult> Lookup(const QString& key) const;
signals:
    void Ready(const QString& key);
private:
    struct State;
    std::unique_ptr<State> state_;
    void StartNext();
};

} // namespace War3Map::Modules
