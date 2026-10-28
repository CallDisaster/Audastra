#include "War3MapCore/Script/MapTestInput.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

namespace War3Map::Script {
Result<std::vector<std::uint32_t>> ParseMapTestKeys(std::string_view text) {
    const auto fail = [] { return Result<std::vector<std::uint32_t>>::Failure("Map.Test", "MAP_TEST_INPUT_ARGUMENT", "按键须为虚拟键名、十进制或0x十六进制码；组合最多8键且不能重复。"); };
    static const std::map<std::string, std::uint32_t> names{
        {"RETURN",13},{"ENTER",13},{"ESCAPE",27},{"ESC",27},{"SPACE",32},{"TAB",9},{"BACKSPACE",8},
        {"SHIFT",16},{"CTRL",17},{"CONTROL",17},{"ALT",18},{"LEFT",37},{"UP",38},{"RIGHT",39},{"DOWN",40},
        {"HOME",36},{"END",35},{"PAGEUP",33},{"PAGEDOWN",34},{"INSERT",45},{"DELETE",46},
    };
    std::vector<std::uint32_t> result;
    while (!text.empty()) {
        const auto end = text.find('+'); auto part = text.substr(0, end);
        std::string token(part); for (auto& c : token) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        std::uint32_t code = 0;
        if (const auto it = names.find(token); it != names.end()) code = it->second;
        else if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= '0' && token[0] <= '9'))) code = token[0];
        else if (token.size() >= 2 && token[0] == 'F' && token[1] >= '1' && token[1] <= '9') {
            unsigned number = 0; const auto parsed = std::from_chars(token.data() + 1, token.data() + token.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || number > 24) return fail();
            code = 111 + number;
        } else {
            auto value = std::string_view(token); int base = 10;
            if (value.starts_with("0X")) { value.remove_prefix(2); base = 16; }
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), code, base);
            if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return fail();
        }
        // 鼠标只能用Click；不发送系统热键或改变真实键盘锁定状态。
        if (code < 8 || code > 254 || code == 20 || code == 91 || code == 92 || code == 144 || code == 145
            || std::find(result.begin(), result.end(), code) != result.end() || result.size() == 8) return fail();
        result.push_back(code);
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1); if (text.empty()) return fail();
    }
    if (result.empty()) return fail();
    return Result<std::vector<std::uint32_t>>::Success(std::move(result));
}
Status ValidateMapTestInput(const MapTestInput& input) {
    if (input.kind == MapTestInputKind::Click) {
        if (std::isfinite(input.x) && std::isfinite(input.y) && input.x >= 0 && input.x <= .8 && input.y >= 0 && input.y <= .6 && input.keys.empty()) return Status::Ok();
    } else if (input.kind == MapTestInputKind::Key && !input.keys.empty() && input.keys.size() <= 8 && input.x == 0 && input.y == 0) {
        std::vector<std::uint32_t> seen;
        for (auto key : input.keys) {
            if (key < 8 || key > 254 || key == 20 || key == 91 || key == 92 || key == 144 || key == 145 || std::find(seen.begin(), seen.end(), key) != seen.end())
                return Status::Failure("Map.Test", "MAP_TEST_INPUT_ARGUMENT", "测试按键无效或重复。");
            seen.push_back(key);
        }
        return Status::Ok();
    } else if (input.kind == MapTestInputKind::Close && input.keys.empty() && input.x == 0 && input.y == 0) return Status::Ok();
    return Status::Failure("Map.Test", "MAP_TEST_INPUT_ARGUMENT", "测试输入类型、坐标或按键无效。");
}
Result<std::pair<int, int>> MapTestInputPixel(double x, double y, int width, int height) {
    const auto valid = ValidateMapTestInput({MapTestInputKind::Click, x, y, {}});
    if (!valid.Succeeded() || width < 1 || height < 1 || width > 32767 || height > 32767)
        return Result<std::pair<int,int>>::Failure("Map.Test", "MAP_TEST_INPUT_GEOMETRY", "界面坐标或实际客户区不可确认。");
    return Result<std::pair<int,int>>::Success({std::min(width - 1, static_cast<int>(std::lround(x * width / .8))),
        std::min(height - 1, static_cast<int>(std::lround((.6 - y) * height / .6)))});
}
}
