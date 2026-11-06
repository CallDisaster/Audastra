#include "War3MapCore/GameUi/UiOrigins.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/Text/TextUtil.h"
#include "UiDescriptionText.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace War3Map::GameUi {
const std::vector<UiOriginPart>& UiOriginParts() {
    static const auto parts = [] {
        std::vector<UiOriginPart> result{{"consoleui", "控制台", false},
            {"resourcebarframe", "资源栏", false}, {"upperbuttonbarframe", "顶部按钮栏", false}};
        for (const auto& area : GameCodeAreas()) result.push_back({std::string(area.id), std::string(area.label), true});
        return result;
    }();
    return parts;
}

const UiOriginPart* FindUiOriginPart(std::string_view name) {
    const auto& parts = UiOriginParts();
    const auto key = Text::LowerAscii(name);
    const auto it = std::find_if(parts.begin(), parts.end(), [&](const auto& part) { return Text::LowerAscii(part.name) == key; });
    return it == parts.end() ? nullptr : &*it;
}

std::vector<Diagnostic> ValidateUiOrigins(const UiDescription& description) {
    std::vector<Diagnostic> diagnostics;
    std::set<std::string> names;
    const auto error = [&](std::size_t i, std::string message) {
        diagnostics.push_back(detail::MakeUiError("AUI_ORIGIN", UiDescriptionMemberPath(description.id) + " /origins/" + std::to_string(i) + "：" + message));
    };
    if (description.origins.size() > UiOriginParts().size()) error(0, "原版调整项过多");
    for (std::size_t i = 0; i < description.origins.size(); ++i) {
        const auto& change = description.origins[i];
        const auto* part = FindUiOriginPart(change.name);
        if (!part || part->name != change.name) error(i, "不是原版节点的规范标识：" + change.name);
        if (!names.insert(change.name).second) error(i, "节点重复");
        if (!change.visible && !change.position && !change.scale && !change.alpha) error(i, "没有调整属性，恢复原样应删除此项");
        if (change.scale && (!std::isfinite(*change.scale) || *change.scale < 0.1 || *change.scale > 4)) error(i, "缩放应为0.1到4");
        if (change.alpha && *change.alpha > 255) error(i, "透明度应为0到255");
        if (change.position) {
            const auto& a = *change.position;
            if (a.target != UiAnchorTarget::Screen || !a.targetId.empty() ||
                static_cast<unsigned>(a.point) > 8 || static_cast<unsigned>(a.targetPoint) > 8 ||
                !std::isfinite(a.x) || !std::isfinite(a.y) || std::abs(a.x) > 2 || std::abs(a.y) > 2) error(i, "位置须为屏幕锚点，偏移不超过2");
        }
    }
    return diagnostics;
}

std::vector<Diagnostic> ApplyUiOriginsPreview(FrameTree& tree, const UiDescription& description) {
    auto diagnostics = ValidateUiOrigins(description);
    if (!diagnostics.empty()) return diagnostics;
    constexpr double x[] = {0, .5, 1, 0, .5, 1, 0, .5, 1};
    constexpr double y[] = {1, 1, 1, .5, .5, .5, 0, 0, 0};
    for (const auto& change : description.origins) {
        const auto it = std::find_if(tree.Nodes().begin(), tree.Nodes().end(), [&](const auto& node) {
            return !node.uiFrame && !node.scriptFrame && Text::LowerAscii(node.name) == Text::LowerAscii(change.name);
        });
        if (it == tree.Nodes().end()) continue;
        const auto nodeId = it->id;
        if (it->layout != LayoutState::Placed) continue;
        const auto original = it->rect;
        const double scale = change.scale.value_or(1);
        double left = original.left, bottom = original.bottom;
        if (change.position) {
            const auto& a = *change.position;
            const auto p = static_cast<unsigned>(a.point), r = static_cast<unsigned>(a.targetPoint);
            const auto screen = tree.Nodes().front().rect;
            left = screen.left + screen.Width() * x[r] + a.x - original.Width() * scale * x[p];
            bottom = screen.bottom + screen.Height() * y[r] + a.y - original.Height() * scale * y[p];
        }
        const auto transform = [&](auto&& self, std::uint32_t id) -> void {
            auto& node = tree.MutableNode(id);
            node.rect = {static_cast<float>(left + (node.rect.left - original.left) * scale),
                static_cast<float>(bottom + (node.rect.bottom - original.bottom) * scale),
                static_cast<float>(left + (node.rect.right - original.left) * scale),
                static_cast<float>(bottom + (node.rect.top - original.bottom) * scale)};
            if (node.text) node.text->fontHeight *= static_cast<float>(scale);
            node.previewOpacity *= change.alpha.value_or(255) / 255.0f;
            for (const auto child : node.children) self(self, child);
        };
        transform(transform, nodeId);
        if (change.visible) tree.MutableNode(nodeId).hidden = !*change.visible;
    }
    return diagnostics;
}

std::vector<Diagnostic> CheckUiOriginSupport(const UiDescription& description) {
    std::vector<Diagnostic> result;
    for (const auto& change : description.origins) {
        const auto* part = FindUiOriginPart(change.name);
        if (!part || (change.visible && !part->visibility) || (change.position && !part->position) ||
            (change.scale && !part->scale) || (change.alpha && !part->alpha)) {
            result.push_back(detail::MakeUiError("AUI_ORIGIN_UNSUPPORTED", "原版节点 " + change.name + " 的调整不支持：" + (part ? part->reason : "没有该节点")));
        }
    }
    return result;
}
} // namespace War3Map::GameUi
