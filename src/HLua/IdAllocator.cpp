#include "War3MapCore/HLua/IdAllocator.h"
#include <algorithm>

namespace War3Map::HLua {
namespace {
bool Rawcode(std::string_view id) {
    return id.size() == 4 && !(id.front() >= '0' && id.front() <= '9')
        && std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e && c!='[' && c!=']'; });
}
char Prefix(std::string_view type) {
    if (type == "ability") return 'K';
    if (type == "item") return 'I';
    if (type == "unit") return 'V';
    if (type == "buff") return 'B';
    if (type == "upgrade") return 'U';
    if (type == "destructable") return 'D';
    if (type == "doodad") return 'O';
    return 'X';
}
}
IdAllocator::IdAllocator(std::span<const std::string> occupied) : occupied_(occupied.begin(), occupied.end()) {}
Result<std::string> IdAllocator::Allocate(std::string_view type, std::string_view parent, std::string_view forced) {
    if (!Rawcode(parent)) return Result<std::string>::Failure("HLua", "HLUA_PARENT_INVALID", "父对象必须是四字节 rawcode。");
    std::string id(forced);
    if (!id.empty()) {
        if (!Rawcode(id)) return Result<std::string>::Failure("HLua", "HLUA_FORCED_ID_INVALID", "指定 ID 必须是首字节非数字的四字节 rawcode。");
        if (occupied_.contains(id)) return Result<std::string>::Failure("HLua", "HLUA_ID_CONFLICT", "指定 ID 已被占用：" + id);
    } else {
        const auto prefix = Prefix(type);
        auto& counter = counters_[prefix];
        constexpr std::string_view digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        for (;;) {
            if (counter >= 36u * 36u * 36u)
                return Result<std::string>::Failure("HLua", "HLUA_ID_EXHAUSTED", "该类别的三位 base36 ID 已耗尽。");
            auto value = counter++;
            id.assign(4, '0'); id[0] = prefix;
            for (int at = 3; at >= 1; --at) { id[at] = digits[value % 36]; value /= 36; }
            if (type == "unit" && parent.front() >= 'a' && parent.front() <= 'z')
                for (auto& c : id) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (!occupied_.contains(id)) break;
        }
    }
    occupied_.insert(id); allocated_.push_back(id);
    return Result<std::string>::Success(std::move(id));
}
} // namespace War3Map::HLua
