#pragma once
#include "War3MapCore/Render/WorldModelScene.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace War3Map::Render::ModelPreview {
// 与预览资源reader共用词法范围；实际文件读取及来源租约仍由调用者管理。
inline std::optional<std::filesystem::path> RelativeAssetPath(std::string_view name) {
    if (name.empty() || name.find(':') != name.npos) return std::nullopt;
    std::string normalized(name);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    const auto relative = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(normalized.data()), normalized.size()));
    if (relative.is_absolute() || relative.has_root_path()) return std::nullopt;
    for (const auto& component : relative) if (component == L"..") return std::nullopt;
    return relative;
}

inline void SetReplacementTexture(std::optional<std::string>& target, std::optional<std::string_view> value) {
    if (target) throw std::runtime_error("duplicate --replaceable-texture");
    if (!value || value->empty() || std::any_of(value->begin(), value->end(), [](unsigned char c) { return c < 32; }))
        throw std::runtime_error("--replaceable-texture requires a relative resource file path");
    const auto relative = RelativeAssetPath(*value);
    if (!relative || !relative->has_filename() || relative->filename() == L".")
        throw std::runtime_error("--replaceable-texture must be a relative resource file path without parent traversal");
    const auto normalized = relative->lexically_normal().generic_u8string();
    target.emplace(reinterpret_cast<const char*>(normalized.data()), normalized.size());
}

inline ModelInstanceRequest MakeRequest(std::string modelKey, const std::optional<std::string>&) {
    ModelInstanceRequest request;
    request.modelPath = std::move(modelKey);
    return request;
}
} // namespace War3Map::Render::ModelPreview
