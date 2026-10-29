#pragma once
#include "War3MapCore/Types.h"

namespace War3Map::Services {
enum class ImportPayloadRole { OwnedResource, OwnedLuaDraft, DiskResource };
struct ImportPayloadPolicy {
    static constexpr std::uint64_t ownedFileBytes = 64ull * 1024 * 1024;
    static constexpr std::uint64_t ownedBatchBytes = 256ull * 1024 * 1024;
    static constexpr std::uint64_t luaDraftBytes = 2ull * 1024 * 1024;
    static constexpr std::uint64_t diskFileBytes = 256ull * 1024 * 1024;
    [[nodiscard]] static constexpr std::uint64_t FileLimit(ImportPayloadRole role) noexcept {
        return role == ImportPayloadRole::OwnedLuaDraft ? luaDraftBytes
            : role == ImportPayloadRole::OwnedResource ? ownedFileBytes : diskFileBytes;
    }
    [[nodiscard]] static Status Check(ImportPayloadRole role, std::uint64_t bytes) {
        if (bytes > FileLimit(role)) return Status::Failure("Import", "IMPORT_PAYLOAD_BUDGET", "导入内容超过当前用途的单文件预算。");
        return Status::Ok();
    }
};
}
