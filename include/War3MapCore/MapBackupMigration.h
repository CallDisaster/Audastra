#pragma once

#include "MapArchiveSession.h"

namespace War3Map {

struct MapBackupMigrationOptions {
    // 源收据与目标保留清理合计的枚举上限；每次内容散列/复制预留完整大小。
    std::size_t maxEntries = 4096;
    std::uint64_t maxReadBytes = 8ull * 1024 * 1024 * 1024;
    std::size_t keepBackups = 10; // 每个源身份；0不清理。
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes{2};
    std::stop_token stop;
};

struct MapBackupMigration {
    std::size_t moved = 0, alreadyPresent = 0, skipped = 0, pruned = 0;
    std::size_t examinedEntries = 0;
    std::uint64_t bytesRead = 0; // 包含失败/取消操作的保守预留，不是成功读取量。
    bool complete = true; // false包括跳过、取消、预算或清理未完成；细节在diagnostics。
};

// 仅迁移owner索引、原生文件身份和完整SHA均能核验的备份，不打开原地图。
// 每份先复制/核验并登记目标新身份，再删除源文件与记录；同名异内容不覆盖。
// 单份失败回滚本次新建物；取消不撤回此前已经完成的份数。不是跨卷崩溃原子事务。
// 调用方在工作线程执行；Win32同步I/O只能在块边界协作取消。
[[nodiscard]] Result<MapBackupMigration> MigrateMapBackups(
    const std::filesystem::path& from, const std::filesystem::path& to,
    const MapBackupMigrationOptions& options = {});

} // namespace War3Map
