#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ProjectSaveControl.h"
#include <memory>

namespace War3Map {

struct ProjectSaveOptions {
    // 仅在调用方已经打开/成功保存该工程时传入。没有此值只允许首次独立保存。
    std::optional<std::string> expectedTargetSha256;
    // 不得覆盖的源文件/源目录；目录的子路径也受保护。不要包含获准更新的工程目标。
    std::vector<std::filesystem::path> protectedSourcePaths;
    // 单次 recovery 使用；为空时沿用完整同步手动保存行为。
    std::shared_ptr<ProjectSaveControl> control;
};

struct ProjectSaveResult {
    std::filesystem::path path;
    std::string sha256;
    std::size_t memberCount = 0;
    std::optional<std::filesystem::path> backupPath;
};

// Qt 无关的完整工程快照存储。只处理 .forge 容器；不生成或覆盖游戏地图。
// 检查清单 JSON、格式版本及来源布局；调用方仍负责内嵌文档语义、文档序列化、
// 源快照检查，以及成功后才提交保存基线。
class ProjectStore {
public:
    // 首次保存不覆盖既有路径；更新要求目标仍存在且 SHA 与上次回执相符。
    // 先完整写入并回读候选，再独占锁定旧文件，将旧文件按句柄改名为独立备份，
    // 最后以不覆盖方式发布候选。失败只尝试不覆盖回滚，绝不删除并发新建的目标。
    // 两次改名之间存在短暂目标缺口；进程崩溃时备份仍是完整工程，可直接打开。
    // 这不是 Windows 原子替换或跨进程 CAS；单次改名的原子性不等于整个事务原子性。
    // 更新成功保留 <目标文件名>.backup-<PID>-<tick>-<sequence>.forge，不自动删除历史。
    [[nodiscard]] static Result<ProjectSaveResult> Save(
        const std::filesystem::path& target,
        std::span<const MapArchiveMember> members,
        const ProjectSaveOptions& options = {});
};

} // namespace War3Map
