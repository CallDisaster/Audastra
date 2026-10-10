#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map::ObjectData {

enum class SkinMemberState : std::uint8_t { Missing, OpaqueUnknown, Ready, Partial, Invalid, Ambiguous };

struct SkinMemberSummary {
    ObjectType type = ObjectType::Unit;
    std::string memberName;
    SkinMemberState state = SkinMemberState::Missing;
    std::string memberSha256;
    std::optional<std::int32_t> version;
    std::size_t originalCount = 0;
    std::size_t customCount = 0;
    std::vector<Diagnostic> diagnostics;
};

struct SkinObjectRow {
    std::size_t sourceIndex = 0;
    NativeObjectTable table = NativeObjectTable::Original;
    ObjectRecord record;
    bool writable = false;
};

struct SkinObjectPage {
    std::vector<SkinObjectRow> rows;
    std::optional<std::size_t> nextOffset;
};

// Skin 层与普通物编是两个独立来源；这里只提供只读原值，不推断覆盖优先级。
class SkinObjectCatalog {
public:
    [[nodiscard]] static Result<SkinObjectCatalog> Capture(
        std::span<const MapArchiveMember> members, std::string sourceSha256,
        bool hasOpaqueMembers = false);
    [[nodiscard]] std::span<const SkinMemberSummary> Members() const noexcept { return summaries_; }
    [[nodiscard]] const std::string& SourceSha256() const noexcept { return sourceSha256_; }
    [[nodiscard]] Result<SkinObjectPage> Query(ObjectType type, std::string_view expectedSourceSha256,
        std::size_t offset, std::size_t limit) const;

private:
    std::string sourceSha256_;
    std::array<SkinMemberSummary, 7> summaries_{};
    std::array<std::unique_ptr<NativeObjectDocument>, 7> documents_{};
};

[[nodiscard]] std::string SkinObjectFileName(ObjectType type);

} // namespace War3Map::ObjectData
