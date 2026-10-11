#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Resources {

struct ImportRecord {
    std::uint8_t flags = 13;
    std::string path;

    [[nodiscard]] bool operator==(const ImportRecord&) const = default;
};

using ImportMemberExists = std::function<bool(std::string_view)>;
enum class ImportScope { Map, Campaign };

// 与当前严格 MPQ 写入器一致：相对 ASCII 虚拟路径，统一反斜杠及大小写用于比较。
[[nodiscard]] Result<std::string> CanonicalImportPath(std::string_view path);

// 5/8 为标准路径，10/13 为完整路径。旧工具写出的歧义记录可由真实归档消歧。
// 未知 flags 不猜测语义；有唯一的实际成员证据时允许解析，但不改写原 flags。
[[nodiscard]] Result<std::string> ResolveArchivePath(
    const ImportRecord& record,
    const ImportMemberExists& exists = {}, ImportScope scope = ImportScope::Map);

// classic war3map.imp v1。解析不修改路径、flags、顺序和尾部未知字节。
// 缺失成员由调用方显式 CreateEmpty；损坏或未知版本绝不降级为空清单。
class ImportTable {
public:
    [[nodiscard]] static Result<ImportTable> Parse(std::span<const std::byte> bytes,
        ImportScope scope = ImportScope::Map);
    [[nodiscard]] static ImportTable CreateEmpty(ImportScope scope = ImportScope::Map);

    [[nodiscard]] const std::vector<ImportRecord>& Entries() const noexcept { return entries_; }
    [[nodiscard]] const std::vector<std::byte>& OriginalBytes() const noexcept { return originalBytes_; }
    [[nodiscard]] bool IsModified() const noexcept { return modified_; }

    // 新增使用 flags=13 的完整路径；已有同一归档路径时保持原记录和字节不变。
    // 验证全部已有记录后再修改，歧义/不安全路径失败不影响文档。
    [[nodiscard]] Status Upsert(
        std::string_view archivePath,
        const ImportMemberExists& exists = {});
    // 删除解析到该归档路径的全部记录（成员改名 / 移动 / 删除时调用）；没有记录时不改动。
    [[nodiscard]] Status Remove(
        std::string_view archivePath,
        const ImportMemberExists& exists = {});
    [[nodiscard]] Result<std::vector<std::byte>> Serialize() const;

private:
    ImportTable() = default;
    std::vector<ImportRecord> entries_;
    std::vector<std::byte> originalBytes_;
    std::vector<std::byte> trailing_;
    bool modified_ = false;
    ImportScope scope_ = ImportScope::Map;
};

} // namespace War3Map::Resources
