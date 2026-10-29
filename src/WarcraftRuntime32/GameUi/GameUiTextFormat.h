#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Audastra::Runtime32::GameUi {

struct TextFormatArgument {
    bool present = false;
    bool integer = false;
    float real = 0;
    std::int32_t value = 0;
};

// 编译结果独占原文；片段只存偏移，复制、移动或小字符串优化都不会留下悬空视图。
class CompiledTextFormat {
public:
    [[nodiscard]] static std::optional<CompiledTextFormat> Parse(std::string_view format);
    [[nodiscard]] std::optional<std::string> Render(const std::array<TextFormatArgument, 8>& arguments) const;
    [[nodiscard]] std::uint8_t RequiredMask() const noexcept { return required_; }

private:
    struct Piece {
        std::size_t offset = 0;
        std::size_t size = 0;
        int argument = -1;
        int decimals = -1;
    };
    std::string source_;
    std::vector<Piece> pieces_;
    std::uint8_t required_ = 0;
};

} // namespace Audastra::Runtime32::GameUi
