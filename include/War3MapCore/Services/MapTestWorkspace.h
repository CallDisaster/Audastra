#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Types.h"

#include <memory>
#include <span>
#include <string_view>

namespace War3Map::Services {

class MapTestWorkspaceLease final {
public:
    ~MapTestWorkspaceLease();
    MapTestWorkspaceLease(const MapTestWorkspaceLease&) = delete;
    MapTestWorkspaceLease& operator=(const MapTestWorkspaceLease&) = delete;
    [[nodiscard]] const std::filesystem::path& Root() const noexcept;
    // 初次冻结为空目录（仅持有root）时，可在准备完成后刷新一次成员清单。
    // 始终复用原root句柄；已有文件/子目录的初始租约、重复刷新或失败后重试均拒绝。
    // 刷新失败保留原root租约且禁止清理，不把部分清单当成完整所有权。
    [[nodiscard]] Status Refresh();
    // 仅宿主已确认游戏全树终态后调用。保持原目录句柄；从只读文件租约切到删除句柄时
    // 再核对同一文件身份/SHA，只删除冻结时已拥有的成员，
    // 不递归删除路径；新增、身份变化或不可删除项保留并返回诊断。
    [[nodiscard]] Status Cleanup(bool gameTerminalConfirmed);
private:
    struct Impl;
    explicit MapTestWorkspaceLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend Result<std::shared_ptr<MapTestWorkspaceLease>> FreezeMapTestWorkspace(const std::filesystem::path& ownedRoot);
};

// 可信宿主刚创建的独占任务目录；不接受协议/Agent自报任意可清理目录。
// 开启root及所有子目录的原句柄租约（仅共享读取），冻结普通文件只读身份/SHA；
// 宿主可在刚创建空root时冻结，在准备完成后调用同一租约的Refresh。
// 上限16目录/512文件/4GiB。重解析、硬链接或未知身份拒绝，析构只关句柄不删除。
[[nodiscard]] Result<std::shared_ptr<MapTestWorkspaceLease>> FreezeMapTestWorkspace(
    const std::filesystem::path& ownedRoot);

// 测试目录由上面的租约持有带删除权限的原句柄；归档写出又要锁住目标的每一级上级目录，且不允许别人持有删除权限。
// 两者在同一测试目录上冲突（Windows 共享冲突）。所以测试副本（SLK 优化、Lua 四字码、Lua 物编资料等）一律
// 先在测试目录外的独占临时目录保存并回读核对，再复制到 target（必须尚不存在）并按 SHA 核对；临时目录随后按租约清理。
// 返回结果的 path 是 target。
[[nodiscard]] Result<ArchiveSaveResult> SaveMapTestCopyOutside(const MapArchiveSession& source,
    std::span<const MemberReplacement> members, const std::filesystem::path& target, std::uint64_t maxMapBytes,
    std::string_view subsystem, std::string_view code);

} // namespace War3Map::Services
