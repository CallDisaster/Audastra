#pragma once

#include "War3MapCore/Services/MapTestService.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace War3Map::Services { class LuaTestMailboxSession; }

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
    // 结束本次 Job 里的全部进程（只作用于本次启动的进程树，不枚举其他游戏）。已结束时直接成功；
    // 返回只代表已发出终止，终态仍由 IsActive/Cleanup 确认。
    [[nodiscard]] Status Stop();
    // 本次 Job 里仍在运行的进程编号（找本次游戏窗口用）；已结束或无法查询时为空。
    [[nodiscard]] std::vector<std::uint32_t> ProcessIds() const;
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
    // 只有自有 Lua 且原 Job/游戏句柄归属得到验证时才提供；UI/MCP 不接触共享内存。
    std::shared_ptr<Services::LuaTestMailboxSession> luaMailbox;
    // 作者手写 Lua 的入口模块；非空且有 luaMailbox 时，编辑器按文件把修改送进游戏。
    std::vector<std::string> luaEntryModules;
    bool luaHotReload = false;
};

// 只发起已验证的独立计划；返回不代表游戏进图。挂起主线程在Job归属成立后才恢复。
// Job不设置KILL_ON_JOB_CLOSE，编辑器退出不结束用户游戏；宿主只在IsActive=false后清理地图。
[[nodiscard]] Result<MapTestLaunchReceipt> LaunchMapTest(const Services::MapTestArtifact& artifact,
    const Services::MapTestProgress& progress = {});

} // namespace War3Map::Script
