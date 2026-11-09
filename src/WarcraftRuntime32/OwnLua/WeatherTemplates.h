#pragma once

#include "LuaHost.h"
#include "NativeProvider.h"

#include <filesystem>
#include <set>

namespace Audastra::Runtime32::OwnLua {

class WeatherTemplates final {
public:
    [[nodiscard]] static WeatherTemplates Read(
        const LuaModuleReader& map, const std::filesystem::path& gameRoot);
    [[nodiscard]] std::string CheckCall(std::string_view name, const NativeSignature& signature,
        std::span<const NativeValue> supplied) const;

private:
    [[nodiscard]] static WeatherTemplates Parse(std::string_view source, std::string origin);
    std::set<std::uint32_t> ids_;
    std::string origin_;
    std::string error_ = "当前测试会话没有有效天气表。";
};

} // namespace Audastra::Runtime32::OwnLua
