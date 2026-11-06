#pragma once

#include "War3MapCore/GameUi/UiDescription.h"

namespace War3Map::GameUi {
// 与原版定义/代码区域共用标识；未列出的节点不推测可用性。
struct UiOriginPart {
    std::string name;
    std::string label;
    bool gameCode = false;
};
[[nodiscard]] const std::vector<UiOriginPart>& UiOriginParts();
[[nodiscard]] const UiOriginPart* FindUiOriginPart(std::string_view name);
[[nodiscard]] std::vector<Diagnostic> ValidateUiOrigins(const UiDescription& description);
// 应在原布局计算完成后调用；整次重建使用新树，因此恢复原样不会累积变换。
[[nodiscard]] std::vector<Diagnostic> ApplyUiOriginsPreview(FrameTree& tree, const UiDescription& description);
} // namespace War3Map::GameUi
