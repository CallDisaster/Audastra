#pragma once
#include "WorldTerrainAssets.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include <QPointer>
#include <QWidget>
#include <functional>

class QLabel;
class QHideEvent;
class QComboBox;
class QPushButton;
class QThread;
namespace War3Map::Modules {
class WorldViewport;
class WorldAnimationPlayer;
// 单实例属性卡；与世界视图使用同一个 Qt-free 动画服务，不逐帧重建模型。
class WorldModelPreview final : public QWidget {
    Q_OBJECT
public:
    explicit WorldModelPreview(QWidget* parent,const War3RenderPluginApiV2* backend);
    ~WorldModelPreview() override;
    [[nodiscard]] bool Matches(const QString& mapKey,std::uint64_t generation,std::uint64_t editRevision,
        const MapData::PlacementId& placement,const Render::ModelInstanceRequest& request) const;
    void ShowSelection(QString mapKey,std::uint64_t generation,std::uint64_t editRevision,
        MapData::PlacementId placement,std::string rowKey,Render::ModelInstanceRequest request,
        Result<WorldTerrainSources> sources,std::function<bool()> isCurrent);
    void ShowUnavailable(QString mapKey,std::uint64_t generation,std::uint64_t editRevision,
        MapData::PlacementId placement,Render::ModelInstanceRequest request,QString reason);
    void ClearSelection();
protected:
    void hideEvent(QHideEvent* event) override;
private:
    struct Identity {
        QString mapKey;
        std::uint64_t generation=0,editRevision=0;
        MapData::PlacementId placement;
        std::string rowKey,modelPath,replaceableTexture,animationProperties;
        std::uint32_t teamColor=0;
        std::array<float,3> tint{1,1,1};
        std::function<bool()> isCurrent;
    };
    struct Pending { Identity identity; Render::ModelInstanceRequest request; WorldTerrainSources sources; };
    struct Built;
    void BeginNext();
    void Present(const Built& built);
    void SetMessage(QString text);
    QLabel* title_=nullptr;
    QLabel* message_=nullptr;
    QComboBox* action_=nullptr;
    QPushButton* playPause_=nullptr;
    QPushButton* step_=nullptr;
    WorldViewport* viewport_=nullptr;
    WorldAnimationPlayer* player_=nullptr;
    QPointer<QThread> worker_;
    std::optional<Identity> identity_;
    std::optional<Pending> pending_;
    std::uint64_t serial_=0,viewportRevision_=0,displayedContentVersion_=0;
};
} // namespace War3Map::Modules
