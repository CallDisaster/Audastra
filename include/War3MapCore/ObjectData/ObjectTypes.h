#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace War3Map::ObjectData {

enum class ObjectType : std::uint8_t {
    Unit,
    Ability,
    Item,
    Buff,
    Upgrade,
    Doodad,
    Destructable,
};

[[nodiscard]] std::string_view ToString(ObjectType type) noexcept;
[[nodiscard]] std::string_view LniFileName(ObjectType type) noexcept;
[[nodiscard]] std::optional<ObjectType> ObjectTypeFromLniFileName(
    std::string_view fileName) noexcept;
[[nodiscard]] std::string_view NativeFileName(ObjectType type) noexcept;
[[nodiscard]] std::optional<ObjectType> ObjectTypeFromNativeFileName(
    std::string_view fileName) noexcept;

struct ObjectKey {
    ObjectType type = ObjectType::Unit;
    std::string rawcode;

    [[nodiscard]] bool operator==(const ObjectKey&) const noexcept = default;
};

struct ModificationKey {
    std::string fieldId;
    std::optional<std::uint32_t> level;
    std::optional<std::uint32_t> dataIndex;
    std::optional<std::uint32_t> variation;

    [[nodiscard]] bool operator==(const ModificationKey&) const noexcept = default;
};

struct RawcodeValue {
    std::string value;
    [[nodiscard]] bool operator==(const RawcodeValue&) const noexcept = default;
};

struct RawcodeListValue {
    std::vector<std::string> values;
    [[nodiscard]] bool operator==(const RawcodeListValue&) const noexcept = default;
};

struct EnumValue {
    std::string value;
    [[nodiscard]] bool operator==(const EnumValue&) const noexcept = default;
};

struct AssetPathValue {
    std::string value;
    [[nodiscard]] bool operator==(const AssetPathValue&) const noexcept = default;
};

struct LocalizedStringValue {
    std::string value;
    [[nodiscard]] bool operator==(const LocalizedStringValue&) const noexcept = default;
};

struct UnknownBytesValue {
    std::vector<std::byte> bytes;
    [[nodiscard]] bool operator==(const UnknownBytesValue&) const noexcept = default;
};

using Value = std::variant<
    std::int32_t,
    float,
    std::string,
    bool,
    RawcodeValue,
    RawcodeListValue,
    EnumValue,
    AssetPathValue,
    LocalizedStringValue,
    UnknownBytesValue>;

[[nodiscard]] std::string_view ValueKindName(const Value& value) noexcept;
[[nodiscard]] std::string ValueToDisplayString(const Value& value);

struct ObjectModification {
    ModificationKey key;
    Value value;
    std::string sourceKey;
    std::string originalValueToken;
    bool valueWasOpaque = false;
    std::size_t sourceLine = 0;
    std::optional<OriginSpan> origin;
};

struct ObjectRecord {
    StableId stableId = 0;
    ObjectKey key;
    std::optional<std::string> parentRawcode;
    std::vector<ObjectModification> modifications;
    std::size_t sectionLine = 0;
};

enum class ObjectChangeKind : std::uint8_t {
    Added,
    Removed,
    Modified,
};

struct ObjectChange {
    ObjectChangeKind kind = ObjectChangeKind::Modified;
    ObjectKey object;
    std::optional<ModificationKey> modification;
    std::optional<Value> before;
    std::optional<Value> after;
};

struct ObjectSemanticDiff {
    std::vector<ObjectChange> changes;

    [[nodiscard]] bool Empty() const noexcept {
        return changes.empty();
    }
};

[[nodiscard]] StableId MakeObjectStableId(const ObjectKey& key) noexcept;
[[nodiscard]] ObjectSemanticDiff DiffObjects(
    std::span<const ObjectRecord> before,
    std::span<const ObjectRecord> after);

} // namespace War3Map::ObjectData
