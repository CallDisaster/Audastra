#pragma once

#include "War3MapCore/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace War3Map {

// 兼容配置仅描述魔兽版本和语言环境。
// 不存在"原版编辑器"与"YDWE"的模式切换——YDWE 是原版的完全兼容超集，
// 第三方函数库由 UI 数据根目录内容自动发现并加载。
struct CompatibilityProfile {
    std::uint32_t schemaVersion = 0;
    std::string id;
    std::string displayName;
    std::string warcraftVersion;
    std::string locale;
    std::string unknownDataPolicy;
    std::string definitionSource;
    std::string definitionSha256;

    [[nodiscard]] Status Validate() const;
    [[nodiscard]] static Result<CompatibilityProfile> FromJson(
        std::string_view definition);
    [[nodiscard]] static CompatibilityProfile Default();
};

} // namespace War3Map
