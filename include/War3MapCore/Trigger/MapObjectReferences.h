#pragma once

#include "War3MapCore/Types.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string_view>

namespace War3Map::Services { class MapObjectReferenceCapture; }

namespace War3Map::Trigger {

// 真实地图成员衍生的只读标量；不授予全局赋值或数组资格。
struct MapObjectReference {
    std::string symbol;
    std::string type; // 仅正向证明的 unit/item；未知项不进入此表。
    std::string rawcode;
    std::int32_t creationId = 0;
    std::size_t sourceIndex = 0;
    std::string memberName;
    std::string memberSha256;
    std::string classificationSource;
};

// 地图内数据可随显式保存迁移；外部游戏与战役仍绑定实际来源。
enum class MapObjectReferenceSourceScope { External, Map, Campaign };

struct MapObjectReferenceInput {
    std::string role;
    std::string source;
    bool present = false;
    std::vector<std::byte> bytes;
    std::string sha256;
    MapObjectReferenceSourceScope scope = MapObjectReferenceSourceScope::External;
};

// 只能由Services用真实成员/分类表捕获。协议端不能传入symbol/type名单取得资格。
class MapObjectReferenceSnapshot final {
public:
    [[nodiscard]] const std::string& Identity() const noexcept { return identity_; }
    [[nodiscard]] std::span<const MapObjectReference> References() const noexcept { return references_; }
    [[nodiscard]] std::span<const MapObjectReferenceInput> Inputs() const noexcept { return inputs_; }
    [[nodiscard]] const std::vector<Diagnostic>& Diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] const MapObjectReference* Find(std::string_view symbol) const noexcept {
        const auto found = std::lower_bound(references_.begin(), references_.end(), symbol,
            [](const auto& entry, auto key) { return entry.symbol < key; });
        return found != references_.end() && found->symbol == symbol ? &*found : nullptr;
    }

private:
    friend class Services::MapObjectReferenceCapture;
    MapObjectReferenceSnapshot() = default;
    std::string identity_;
    std::vector<MapObjectReference> references_;
    std::vector<MapObjectReferenceInput> inputs_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace War3Map::Trigger
