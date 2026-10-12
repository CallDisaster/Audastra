#pragma once
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Render/TerrainCliffs.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace War3Map::Modules {
class WorldViewport;

// 悬崖网格单独成一个附加模型层。地形笔刷改变悬崖时只重建这一层，
// 不再重建整张世界的动画场景（单位、装饰物等全部模型）。
struct WorldCliffLayerFrame {
    std::shared_ptr<const Render::TerrainCliffs> cliffs; // 本层对应的悬崖网格身份
    std::shared_ptr<const Render::AnimatedModelScene> scene;
    std::shared_ptr<const Render::AnimationFrame> frame;
};

// worker 内调用：只读取悬崖贴图，不读模型、不涉及实例。cliffs 为空或没有网格时返回空帧。
[[nodiscard]] Result<WorldCliffLayerFrame> PrepareWorldCliffLayer(std::shared_ptr<const Render::TerrainCliffs> cliffs,
    const Render::AnimationResourceReader& read, Render::AnimationSession session);

class WorldCliffLayerPresenter final : public QObject {
public:
    explicit WorldCliffLayerPresenter(WorldViewport* viewport, QObject* parent = nullptr);
    ~WorldCliffLayerPresenter() override;
    // 显示一份已准备的悬崖层；frame 为空帧时移除层。主场景换代后由定时检查重新提交。
    void Show(WorldCliffLayerFrame frame);
    // 主场景重新安装后立即重提交，避免悬崖消失一帧。
    void RebindParent();
    void Clear();
    [[nodiscard]] bool Showing(const std::shared_ptr<const Render::TerrainCliffs>& cliffs) const noexcept {
        return shown_.cliffs == cliffs && (submitted_ || !shown_.scene); // 无悬崖时层已移除，也算已显示
    }
    [[nodiscard]] const Status& LastStatus() const noexcept { return status_; }
private:
    void Submit();
    static constexpr std::uint64_t LayerId = 0x434c4946464c4159ull;
    QPointer<WorldViewport> viewport_;
    QTimer timer_;
    WorldCliffLayerFrame shown_;
    std::uint64_t parentEpoch_ = 0, failedEpoch_ = 0;
    bool submitted_ = false;
    Status status_;
};
} // namespace War3Map::Modules
