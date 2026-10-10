#include "JassTypes.h"

#include <array>

namespace War3Map::Jass::detail {

TypeTable::TypeTable() {
    // 顺序与 kErrorType … kHandle 的编号一致。
    constexpr std::array<std::string_view, 9> kBuiltins = {
        "<错误>", "nothing", "null", "integer", "real", "boolean", "string", "code", "handle"};
    for (std::size_t i = 0; i < kBuiltins.size(); ++i) {
        entries_.push_back({std::string(kBuiltins[i]), kUnknownType});
        if (i >= static_cast<std::size_t>(kInteger)) byName_.emplace(kBuiltins[i], static_cast<TypeId>(i));
    }
}

TypeId TypeTable::Declare(std::string_view name, TypeId parent) {
    if (byName_.contains(name) || parent < kFirstUserType - 1 || parent >= static_cast<TypeId>(entries_.size()) || !IsHandle(parent)) {
        return kUnknownType;
    }
    const auto id = static_cast<TypeId>(entries_.size());
    entries_.push_back({std::string(name), parent});
    byName_.emplace(name, id);
    return id;
}

TypeId TypeTable::Find(std::string_view name) const {
    const auto found = byName_.find(name);
    return found == byName_.end() ? kUnknownType : found->second;
}

const std::string& TypeTable::Name(TypeId type) const {
    static const std::string kUnknown = "<未知>";
    return type >= 0 && type < static_cast<TypeId>(entries_.size()) ? entries_[static_cast<std::size_t>(type)].name : kUnknown;
}

TypeId TypeTable::Parent(TypeId type) const {
    return type >= 0 && type < static_cast<TypeId>(entries_.size()) ? entries_[static_cast<std::size_t>(type)].parent : kUnknownType;
}

bool TypeTable::IsHandle(TypeId type) const {
    return Extends(type, kHandle);
}

bool TypeTable::Extends(TypeId from, TypeId to) const {
    for (TypeId type = from; type != kUnknownType; type = Parent(type)) {
        if (type == to) return true;
    }
    return false;
}

bool TypeTable::CanConvert(TypeId from, TypeId to) const {
    if (from == kErrorType || to == kErrorType || from == to) return true;
    if (from == kNull) return to == kString || to == kCode || IsHandle(to);
    if (from == kInteger && to == kReal) return true;
    return IsHandle(from) && Extends(from, to);
}

bool TypeTable::CanCompare(TypeId left, TypeId right) const {
    if (left == kErrorType || right == kErrorType || left == right) return true;
    if (left == kNothing || right == kNothing) return false;
    if (left == kNull || right == kNull) return true;
    if (IsNumeric(left) && IsNumeric(right)) return true;
    return IsHandle(left) && IsHandle(right);
}

} // namespace War3Map::Jass::detail
