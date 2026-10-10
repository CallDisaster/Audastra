#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace War3Map::ObjectData {
namespace {

[[nodiscard]] bool SameKey(const ObjectKey& left, const ObjectKey& right) noexcept {
    return left == right;
}

[[nodiscard]] const ObjectRecord* FindRecord(
    std::span<const ObjectRecord> records,
    const ObjectKey& key) noexcept {
    const auto iterator = std::find_if(
        records.begin(),
        records.end(),
        [&key](const ObjectRecord& record) {
            return SameKey(record.key, key);
        });
    return iterator == records.end() ? nullptr : &*iterator;
}

[[nodiscard]] const ObjectModification* FindModification(
    const ObjectRecord& record,
    const ModificationKey& key) noexcept {
    const auto iterator = std::find_if(
        record.modifications.begin(),
        record.modifications.end(),
        [&key](const ObjectModification& modification) {
            return modification.key == key;
        });
    return iterator == record.modifications.end() ? nullptr : &*iterator;
}

[[nodiscard]] std::string BytesToHex(const UnknownBytesValue& value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::setfill('0');
    for (const std::byte byte : value.bytes) {
        stream << std::setw(2) << std::to_integer<unsigned int>(byte);
    }
    return stream.str();
}

} // namespace

std::string_view ToString(ObjectType type) noexcept {
    switch (type) {
    case ObjectType::Unit:
        return "unit";
    case ObjectType::Ability:
        return "ability";
    case ObjectType::Item:
        return "item";
    case ObjectType::Buff:
        return "buff";
    case ObjectType::Upgrade:
        return "upgrade";
    case ObjectType::Doodad:
        return "doodad";
    case ObjectType::Destructable:
        return "destructable";
    }
    return "unknown";
}

std::string_view LniFileName(ObjectType type) noexcept {
    switch (type) {
    case ObjectType::Unit:
        return "unit.ini";
    case ObjectType::Ability:
        return "ability.ini";
    case ObjectType::Item:
        return "item.ini";
    case ObjectType::Buff:
        return "buff.ini";
    case ObjectType::Upgrade:
        return "upgrade.ini";
    case ObjectType::Doodad:
        return "doodad.ini";
    case ObjectType::Destructable:
        return "destructable.ini";
    }
    return {};
}

std::optional<ObjectType> ObjectTypeFromLniFileName(
    std::string_view fileName) noexcept {
    std::string lowered(fileName);
    std::transform(
        lowered.begin(),
        lowered.end(),
        lowered.begin(),
        [](unsigned char value) {
            if (value >= 'A' && value <= 'Z') {
                return static_cast<char>(value - 'A' + 'a');
            }
            return static_cast<char>(value);
        });

    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };
    for (const ObjectType type : types) {
        if (lowered == LniFileName(type)) {
            return type;
        }
    }
    return std::nullopt;
}

std::string_view NativeFileName(ObjectType type) noexcept {
    switch (type) {
    case ObjectType::Unit:
        return "war3map.w3u";
    case ObjectType::Ability:
        return "war3map.w3a";
    case ObjectType::Item:
        return "war3map.w3t";
    case ObjectType::Buff:
        return "war3map.w3h";
    case ObjectType::Upgrade:
        return "war3map.w3q";
    case ObjectType::Doodad:
        return "war3map.w3d";
    case ObjectType::Destructable:
        return "war3map.w3b";
    }
    return {};
}

std::optional<ObjectType> ObjectTypeFromNativeFileName(
    std::string_view fileName) noexcept {
    std::string lowered(fileName);
    std::transform(
        lowered.begin(),
        lowered.end(),
        lowered.begin(),
        [](unsigned char value) {
            if (value >= 'A' && value <= 'Z') {
                return static_cast<char>(value - 'A' + 'a');
            }
            return static_cast<char>(value);
        });

    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };
    for (const ObjectType type : types) {
        if (lowered == NativeFileName(type)) {
            return type;
        }
    }
    return std::nullopt;
}

std::string_view ValueKindName(const Value& value) noexcept {
    return std::visit(
        [](const auto& current) -> std::string_view {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, std::int32_t>) {
                return "int32";
            } else if constexpr (std::is_same_v<T, float>) {
                return "float32";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return "string";
            } else if constexpr (std::is_same_v<T, bool>) {
                return "bool";
            } else if constexpr (std::is_same_v<T, RawcodeValue>) {
                return "rawcode";
            } else if constexpr (std::is_same_v<T, RawcodeListValue>) {
                return "rawcode-list";
            } else if constexpr (std::is_same_v<T, EnumValue>) {
                return "enum";
            } else if constexpr (std::is_same_v<T, AssetPathValue>) {
                return "asset-path";
            } else if constexpr (std::is_same_v<T, LocalizedStringValue>) {
                return "localized-string";
            } else {
                return "unknown-bytes";
            }
        },
        value);
}

std::string ValueToDisplayString(const Value& value) {
    return std::visit(
        [](const auto& current) -> std::string {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, std::int32_t>) {
                return std::to_string(current);
            } else if constexpr (std::is_same_v<T, float>) {
                std::array<char, 64> buffer{};
                const auto [end, error] = std::to_chars(
                    buffer.data(),
                    buffer.data() + buffer.size(),
                    current,
                    std::chars_format::general);
                if (error != std::errc{}) {
                    return "<unprintable-float>";
                }
                return std::string(buffer.data(), end);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return current;
            } else if constexpr (std::is_same_v<T, bool>) {
                return current ? "true" : "false";
            } else if constexpr (
                std::is_same_v<T, RawcodeValue>
                || std::is_same_v<T, EnumValue>
                || std::is_same_v<T, AssetPathValue>
                || std::is_same_v<T, LocalizedStringValue>) {
                return current.value;
            } else if constexpr (std::is_same_v<T, RawcodeListValue>) {
                std::string result;
                for (std::size_t index = 0; index < current.values.size(); ++index) {
                    if (index != 0) {
                        result += ',';
                    }
                    result += current.values[index];
                }
                return result;
            } else {
                return BytesToHex(current);
            }
        },
        value);
}

StableId MakeObjectStableId(const ObjectKey& key) noexcept {
    constexpr StableId offsetBasis = 14695981039346656037ull;
    constexpr StableId prime = 1099511628211ull;
    StableId hash = offsetBasis;
    hash ^= static_cast<std::uint8_t>(key.type);
    hash *= prime;
    for (const unsigned char character : key.rawcode) {
        hash ^= character;
        hash *= prime;
    }
    return hash;
}

ObjectSemanticDiff DiffObjects(
    std::span<const ObjectRecord> before,
    std::span<const ObjectRecord> after) {
    ObjectSemanticDiff result;

    for (const ObjectRecord& previous : before) {
        const ObjectRecord* current = FindRecord(after, previous.key);
        if (current == nullptr) {
            result.changes.push_back(ObjectChange{
                ObjectChangeKind::Removed,
                previous.key,
                std::nullopt,
                std::nullopt,
                std::nullopt,
            });
            continue;
        }

        if (previous.parentRawcode != current->parentRawcode) {
            result.changes.push_back(ObjectChange{
                ObjectChangeKind::Modified,
                previous.key,
                ModificationKey{"_parent", std::nullopt, std::nullopt, std::nullopt},
                previous.parentRawcode.has_value()
                    ? std::optional<Value>{Value{*previous.parentRawcode}}
                    : std::nullopt,
                current->parentRawcode.has_value()
                    ? std::optional<Value>{Value{*current->parentRawcode}}
                    : std::nullopt,
            });
        }

        for (const ObjectModification& oldModification : previous.modifications) {
            const ObjectModification* newModification = FindModification(
                *current,
                oldModification.key);
            if (newModification == nullptr) {
                result.changes.push_back(ObjectChange{
                    ObjectChangeKind::Removed,
                    previous.key,
                    oldModification.key,
                    oldModification.value,
                    std::nullopt,
                });
            } else if (oldModification.value != newModification->value) {
                result.changes.push_back(ObjectChange{
                    ObjectChangeKind::Modified,
                    previous.key,
                    oldModification.key,
                    oldModification.value,
                    newModification->value,
                });
            }
        }

        for (const ObjectModification& newModification : current->modifications) {
            if (FindModification(previous, newModification.key) == nullptr) {
                result.changes.push_back(ObjectChange{
                    ObjectChangeKind::Added,
                    current->key,
                    newModification.key,
                    std::nullopt,
                    newModification.value,
                });
            }
        }
    }

    for (const ObjectRecord& current : after) {
        if (FindRecord(before, current.key) == nullptr) {
            result.changes.push_back(ObjectChange{
                ObjectChangeKind::Added,
                current.key,
                std::nullopt,
                std::nullopt,
                std::nullopt,
            });
        }
    }
    return result;
}

} // namespace War3Map::ObjectData
