#include "War3MapCore/Render/RenderAdapters.h"

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <map>
#include <tuple>

namespace War3Map::Render {
namespace {
std::string Utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string output(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, output.data(), size, nullptr, nullptr);
    return output;
}
std::string Lower(std::string_view text) {
    std::string output(text);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return output;
}
} // namespace

Result<std::vector<RenderAdapter>> EnumerateRenderAdapters() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return Result<std::vector<RenderAdapter>>::Failure("Render.Adapters", "RENDER_ADAPTER_ENUMERATION_FAILED", "无法列出本机显卡。");
    std::vector<RenderAdapter> adapters;
    std::map<std::tuple<UINT, UINT, UINT, UINT>, std::uint32_t> ordinals;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        // 系统自带的软件渲染器（Microsoft Basic Render Driver）不算硬件显卡。
        if (desc.VendorId == 0x1414 && desc.DeviceId == 0x8C) continue;
        const auto identity = std::make_tuple(desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision);
        const auto ordinal = ordinals[identity]++;
        std::array<char, 64> key{};
        std::snprintf(key.data(), key.size(), "%04X:%04X:%08X:%02X#%u", desc.VendorId & 0xFFFFu, desc.DeviceId & 0xFFFFu,
            desc.SubSysId, desc.Revision & 0xFFu, ordinal);
        RenderAdapter output;
        output.key = key.data(); output.name = Utf8(desc.Description);
        output.vendorId = desc.VendorId; output.deviceId = desc.DeviceId;
        output.luid = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(desc.AdapterLuid.HighPart)) << 32) | desc.AdapterLuid.LowPart;
        output.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        adapters.push_back(std::move(output));
    }
    return Result<std::vector<RenderAdapter>>::Success(std::move(adapters));
}

const RenderAdapter* FindRenderAdapter(const std::vector<RenderAdapter>& adapters, std::string_view text) noexcept try {
    if (text.empty()) return nullptr;
    for (const auto& adapter : adapters) if (adapter.key == text) return &adapter;
    const auto wanted = Lower(text);
    for (const auto& adapter : adapters) if (Lower(adapter.name).find(wanted) != std::string::npos) return &adapter;
    return nullptr;
} catch (...) { return nullptr; }

} // namespace War3Map::Render
