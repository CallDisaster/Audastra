#pragma once

#include "War3MapCore/Services/MapTestService.h"

#include <memory>

namespace War3Map::Script {

// 原生测试 SLK 优化：从游戏目录读取原版物编表，调用 ObjectData::OptimizeObjectDataToSlk，
// 只向本次独占目录的新地图写出。有效物编逐字段回读、替换成员回读和源图未变均通过才返回成功；
// 成功结果附带转换摘要与保留原因（Info）。不调用任何外部工具。
[[nodiscard]] Status OptimizeMapTestSlk(const Services::MapTestSlkRequest& request);

// 根据所选游戏脚本与地图实际依赖预检原生/扩展启动；不读取YDWE或注册表。
[[nodiscard]] Result<Services::MapTestLaunchPlan> BuildIndependentMapTestLaunchPlan(
    const Services::MapTestArtifact& artifact, const Services::MapTestProgress& progress = {});

struct MapTestLaunchReceipt;
namespace detail { struct MapTestLaunchAccess; }
class MapTestLaunchLease final {
public:
    ~MapTestLaunchLease();
    MapTestLaunchLease(const MapTestLaunchLease&) = delete;
    MapTestLaunchLease& operator=(const MapTestLaunchLease&) = delete;
    // Job仍有进程或无法确认已空时返回true。
    // 子进程捕获不完整但Job已空时允许重试；Cleanup仍保留副本并说明缺失证据。
    // 宿主存活时观察Job新进程；宿主退出后仅已捕获进程持有Job，不继续观察新后代。
    [[nodiscard]] bool IsActive() const noexcept;
    // 仅原进程树终态后调用；在后台核对并清理本次短路径副本，返回启动终态诊断。
    [[nodiscard]] Status Cleanup();
private:
    struct Impl;
    explicit MapTestLaunchLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend Result<MapTestLaunchReceipt> LaunchMapTest(const Services::MapTestArtifact& artifact,
        const Services::MapTestProgress& progress);
    friend struct detail::MapTestLaunchAccess;
};

struct MapTestLaunchReceipt {
    std::uint32_t launcherProcessId = 0;
    std::shared_ptr<MapTestLaunchLease> lease;
};

// 只发起已验证的独立计划；返回不代表游戏进图。挂起主线程在Job归属成立后才恢复。
// Job不设置KILL_ON_JOB_CLOSE，编辑器退出不结束用户游戏；宿主只在IsActive=false后清理地图。
[[nodiscard]] Result<MapTestLaunchReceipt> LaunchMapTest(const Services::MapTestArtifact& artifact,
    const Services::MapTestProgress& progress = {});

} // namespace War3Map::Script
