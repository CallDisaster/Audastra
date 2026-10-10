#include "ObjectLayerCommand.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/SkinObjectCatalog.h"

#include <bit>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;

void Diagnostics(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& item : diagnostics)
        std::cerr << item.subsystem << ": " << item.code << ": " << item.message << '\n';
}

std::string_view StateName(SkinMemberState state) {
    switch (state) {
    case SkinMemberState::Missing: return "missing";
    case SkinMemberState::OpaqueUnknown: return "opaque-unknown";
    case SkinMemberState::Ready: return "ready-read-only";
    case SkinMemberState::Partial: return "partial-read-only";
    case SkinMemberState::Invalid: return "invalid";
    case SkinMemberState::Ambiguous: return "ambiguous";
    }
    return "invalid";
}

std::string Hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

std::optional<std::string> RawValue(const Value& value) {
    if (const auto* number = std::get_if<std::int32_t>(&value)) return std::to_string(*number);
    if (const auto* number = std::get_if<float>(&value)) {
        std::ostringstream stream;
        stream << "f32:" << std::hex << std::setfill('0') << std::setw(8)
            << std::bit_cast<std::uint32_t>(*number);
        return stream.str();
    }
    if (const auto* bytes = std::get_if<std::string>(&value)) return "hex:" + Hex(*bytes);
    return std::nullopt;
}

std::optional<std::size_t> Number(std::string_view text) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::string Index(const std::optional<std::uint32_t>& value) {
    return value ? std::to_string(*value) : "-";
}

std::optional<ObjectType> Type(std::string_view type) {
    for (const auto current : {ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
            ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable}) {
        if (type == ToString(current)) return current;
    }
    return std::nullopt;
}

Result<SkinObjectCatalog> Open(const std::filesystem::path& map) {
    auto archive = MapArchiveSession::Open(map);
    if (!archive.Succeeded()) {
        Result<SkinObjectCatalog> result;
        result.diagnostics = std::move(archive.diagnostics);
        return result;
    }
    return SkinObjectCatalog::Capture(archive.value->Members(), archive.value->SourceSha256(),
        archive.value->HasOpaqueMembers());
}
} // namespace

int RunObjectLayerInspect(const std::filesystem::path& map) {
    auto catalog = Open(map);
    if (!catalog.Succeeded()) { Diagnostics(catalog.diagnostics); return 2; }
    std::cout << "layer=skin\tsource_sha256=" << catalog.value->SourceSha256() << '\n';
    bool invalid = false;
    for (const auto& member : catalog.value->Members()) {
        std::cout << "member\t" << ToString(member.type) << '\t' << member.memberName << '\t'
            << StateName(member.state) << '\t' << member.version.value_or(0) << '\t'
            << member.originalCount << '\t' << member.customCount << '\t' << member.memberSha256 << '\n';
        Diagnostics(member.diagnostics);
        invalid |= member.state == SkinMemberState::Invalid || member.state == SkinMemberState::Ambiguous;
    }
    return invalid ? 2 : 0;
}

int RunObjectLayerQuery(const std::filesystem::path& map, std::string_view expectedSourceSha256,
    std::string_view type, std::string_view offset, std::string_view limit) {
    const auto objectType = Type(type);
    const auto start = Number(offset);
    const auto count = Number(limit);
    if (!objectType || !start || !count) {
        std::cerr << "ObjectData: SKIN_QUERY_ARGUMENT_INVALID: 类型或页码无效。\n";
        return 2;
    }
    auto catalog = Open(map);
    if (!catalog.Succeeded()) { Diagnostics(catalog.diagnostics); return 2; }
    auto page = catalog.value->Query(*objectType, expectedSourceSha256, *start, *count);
    if (!page.Succeeded()) { Diagnostics(page.diagnostics); return 2; }
    for (const auto& row : page.value->rows) {
        std::cout << "row\t" << row.sourceIndex << '\t'
            << (row.table == NativeObjectTable::Original ? "original" : "custom") << '\t'
            << Hex(row.record.key.rawcode) << '\t'
            << (row.record.parentRawcode ? Hex(*row.record.parentRawcode) : "-") << '\t'
            << row.record.modifications.size() << "\treadonly\n";
        for (std::size_t index = 0; index < row.record.modifications.size(); ++index) {
            const auto& field = row.record.modifications[index];
            const auto raw = RawValue(field.value);
            if (!raw) {
                std::cerr << "ObjectData: SKIN_VALUE_UNSUPPORTED: 未知字段值类型，已停止输出。\n";
                return 2;
            }
            std::cout << "field\t" << row.sourceIndex << '\t' << index << '\t'
                << Hex(field.key.fieldId) << '\t' << Index(field.key.level) << '\t'
                << Index(field.key.dataIndex) << '\t' << Index(field.key.variation)
                << '\t' << ValueKindName(field.value) << '\t' << *raw << '\n';
        }
    }
    std::cout << "next=";
    if (page.value->nextOffset) std::cout << *page.value->nextOffset;
    else std::cout << "end";
    std::cout << '\n';
    return 0;
}
