#pragma once

#include "War3MapCore/Types.h"
#include <map>
#include <set>
#include <span>

namespace War3Map::HLua {

class IdAllocator final {
public:
    explicit IdAllocator(std::span<const std::string> occupied);
    [[nodiscard]] Result<std::string> Allocate(std::string_view objectClass,
        std::string_view parent, std::string_view forced = {});
    [[nodiscard]] std::span<const std::string> Allocated() const noexcept { return allocated_; }
private:
    std::set<std::string> occupied_;
    std::map<char, std::uint32_t> counters_;
    std::vector<std::string> allocated_;
};

} // namespace War3Map::HLua
