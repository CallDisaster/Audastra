#pragma once

#include "WorldSelectionPresentation.h"
#include "WorldTerrainAssets.h"
#include "War3MapCore/MapData/PlacementEditing.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QThread>

namespace War3Map::Modules {
class WorldViewport;

// 一个附加层承载全部单位选取圈。主世界roster事务不适用于此层；选择布局或父身份
// 改变时重交这个小层，普通位置变化只更新矩阵，不重复读取BLP或主场景几何。
class WorldSelectionPresenter final : public QObject {
    Q_OBJECT
public:
    explicit WorldSelectionPresenter(WorldViewport& viewport, QObject* parent = nullptr);
    ~WorldSelectionPresenter() override;
    void SetSource(WorldTerrainSources sources, Render::AnimationSession session,
        std::function<bool()> sourceCurrent);
    // selectedIds若提供，须与requests平行；只用于拒绝迟到的旧选中对象，不猜rawcode/位置。
    // waterHeight只可捕获拥有的只读值，不能在后台访问QObjects或共享可变地图。
    void Show(std::vector<Render::ModelInstanceRequest> requests,
        std::shared_ptr<const MapData::TerrainGridSnapshot> terrain,
        std::function<std::optional<float>(double, double)> waterHeight = {},
        std::vector<MapData::PlacementId> selectedIds = {});
    void Hide(); // 清除当前选择，保留来源/真实圈素材。
    void Clear(); // 来源失效，取消接收；不阻塞在途worker。
    void RebindParent(); // 主世界模型或roster安装后，由宿主显式调用。
    // 主roster排队前同步调用；CPU素材仍可接收，但终态前禁止Set/Update/Remove附加层。
    void SetSubmissionDeferred(bool deferred);
    [[nodiscard]] bool SubmissionDeferred() const noexcept { return submissionDeferred_; }
    [[nodiscard]] bool Preparing() const noexcept { return worker_ != nullptr; }
    [[nodiscard]] std::uint64_t LayerId() const noexcept { return layerId_; }
signals:
    void Message(const QString& text);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Work;
    struct Signature {
        std::uint64_t source = 0, roster = 0, request = 0, binding = 0, parent = 0;
        bool operator==(const Signature&) const = default;
    };
    void BeginNext();
    void SubmitPrepared(const Signature& signature);
    void RemoveLayer();
    void CheckParent();
    void UpdateTimer();
    [[nodiscard]] Signature CurrentSignature() const;
    QPointer<WorldViewport> viewport_;
    QPointer<QThread> worker_;
    QTimer parentTimer_;
    std::optional<WorldTerrainSources> sources_;
    Render::AnimationSession session_;
    std::function<bool()> sourceCurrent_;
    std::shared_ptr<WorldResourceReader> reader_;
    std::optional<WorldSelectionCircleAssets> assets_;
    std::optional<WorldSelectionCircleFrame> prepared_;
    std::optional<WorldSelectionCircleFrame> installed_;
    std::vector<Render::ModelInstanceRequest> requests_;
    std::vector<MapData::PlacementId> selectedIds_;
    std::shared_ptr<const MapData::TerrainGridSnapshot> terrain_;
    std::function<std::optional<float>(double, double)> waterHeight_;
    std::optional<Signature> failed_;
    QString lastMessage_;
    std::uint64_t sourceSerial_ = 0, rosterSerial_ = 0, requestSerial_ = 0, bindingSerial_ = 0;
    std::uint64_t preparedRequestSerial_ = 0, observedParent_ = 0;
    bool assetFailed_ = false, needsSubmit_ = false, submitted_ = false;
    bool submissionDeferred_ = false, removalPending_ = false;
    const std::uint64_t layerId_;
};
} // namespace War3Map::Modules
