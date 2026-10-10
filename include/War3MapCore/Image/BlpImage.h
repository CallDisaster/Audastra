#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace War3Map::Image {

// 解码后的 32 位图像，像素为 RGBA8888（每像素 4 字节，行优先，无填充）。
struct DecodedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> rgba; // size == width * height * 4

    [[nodiscard]] bool Valid() const noexcept {
        return width > 0 && height > 0 &&
            rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

// 解码 Warcraft III 的 BLP 纹理为 RGBA。
// 支持：
//   * BLP1 + JPEG 压缩（content=0）：4 组件按 BGRA 原样映射（BLP 约定，不做 CMYK 变换）。
//   * BLP1 + 调色板（content=1）：256 色 BGRA 调色板 + 索引 + 可选 alpha 平面。
// 不支持的格式（如 BLP2/DXT）返回 Failure，绝不产出损坏像素（fail-closed）。
[[nodiscard]] Result<DecodedImage> DecodeBlp(std::span<const std::byte> data);

// 编码为 BLP1 调色板格式（content=1）并生成完整 mipmap 链（BlpEncode.cpp）。
// 调色板由图像（含各级 mipmap）的实际颜色组成，超过 256 色时失败，不做有损量化；灰度图总能编码。
// alpha 全不透明时不写 alpha 平面。
[[nodiscard]] Result<std::vector<std::byte>> EncodeBlpPaletted(const DecodedImage& image);

// 暂停 / 禁用按钮图标（DISBTN / DISPAS）的外观：去色并压暗，保留 alpha。
[[nodiscard]] DecodedImage MakeDisabledIcon(const DecodedImage& image);

} // namespace War3Map::Image
