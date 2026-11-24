#pragma once

#include "War3MapCore/Render/RenderAdapterPlugin.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Render {

// 一块可用于硬件渲染的显卡。key 跨重启稳定（厂商:设备:子系统:修订#同型号序号，十六进制大写）；
// luid 只在本次开机内有效，不能写进设置。
struct RenderAdapter {
    std::string key, name;
    std::uint32_t vendorId = 0, deviceId = 0;
    std::uint64_t luid = 0, dedicatedVideoMemory = 0;
};

// 按系统枚举顺序列出硬件显卡（不含软件渲染器），第一个即系统默认。
[[nodiscard]] Result<std::vector<RenderAdapter>> EnumerateRenderAdapters();

// 设置里的显卡键是否合法（空串表示跟随系统）。纯格式检查，设置提交处不链接显卡枚举也能用。
[[nodiscard]] inline bool ValidRenderAdapterKey(std::string_view key) noexcept {
    const auto hex = [](std::string_view text) {
        for (const char c : text) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
        return true;
    };
    if (key.empty()) return true;
    const auto hash = key.find('#');
    if (hash != 21 || key.size() < 23 || key.size() > 25) return false;
    for (const char c : key.substr(22)) if (c < '0' || c > '9') return false;
    return key[4] == ':' && key[9] == ':' && key[18] == ':' &&
        hex(key.substr(0, 4)) && hex(key.substr(5, 4)) && hex(key.substr(10, 8)) && hex(key.substr(19, 2));
}

// 按文本找显卡：先精确匹配 key；否则按名称不区分大小写包含匹配（命令行用，例如 "Quadro"）。
// 名称匹配到多块卡时取枚举顺序第一块。没有匹配返回空指针。
[[nodiscard]] const RenderAdapter* FindRenderAdapter(const std::vector<RenderAdapter>& adapters, std::string_view text) noexcept;

} // namespace War3Map::Render
