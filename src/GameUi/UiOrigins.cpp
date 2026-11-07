#include "War3MapCore/GameUi/UiOrigins.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/Text/TextUtil.h"
#include "UiDescriptionText.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>

namespace War3Map::GameUi {
const std::vector<UiOriginPart>& UiOriginParts() {
    static const auto parts = [] {
        std::vector<UiOriginPart> result{{"consoleui", "控制台", false},
            {"resourcebarframe", "资源栏", false}, {"upperbuttonbarframe", "顶部按钮栏", false}};
        for (const auto& area : GameCodeAreas()) result.push_back({std::string(area.id), std::string(area.label), true});
        const std::initializer_list<std::string_view> qualified{
            "resourcebarframe", "upperbuttonbarframe", "Portrait", "CommandBar", "InfoBar", "HeroBar",
            "InventoryButton1", "InventoryButton2", "InventoryButton3", "InventoryButton4", "InventoryButton5", "InventoryButton6",
            "MinimapButton1", "MinimapButton2", "MinimapButton3", "MinimapButton4", "MinimapButton5"};
        // 能力按部件与属性独立验收；查到对象或底层返回成功并不代表完整布局可用。
        for (auto& part : result) {
            if (std::find(qualified.begin(), qualified.end(), part.name) != qualified.end()) {
                part.visibility = part.position = part.scale = part.alpha = true;
                part.scaleRequiresPosition = true;
                part.reason = "1.27a：显示/隐藏、移动、缩放、透明度；缩放需同时设置位置";
            } else if (part.name == "Minimap") {
                part.visibility = part.position = true;
                part.reason = "1.27a：显示/隐藏、移动；实际渲染视口的缩放、透明度不支持";
            } else if (part.name == "TimeOfDay") {
                part.visibility = true;
                part.reason = "1.27a：仅显示/隐藏；移动、缩放、透明度未通过渲染与输入核对，不支持";
            } else if (part.name == "consoleui") {
                part.reason = "不支持整组调整：含独立渲染部件，完整布局与输入未通过；请分别调整已支持的部件";
            } else if (part.name == "ChatMessage") {
                part.reason = "不支持：消息框布局未通过验收，运行库在调用前拒绝";
            }
        }
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
        const auto pivot = it->anchors.empty() ? static_cast<unsigned>(FramePoint::BottomLeft) : static_cast<unsigned>(it->anchors.front().point);
        double left = original.left + original.Width() * x[pivot] * (1 - scale);
        double bottom = original.bottom + original.Height() * y[pivot] * (1 - scale);
        if (change.position) {
            const auto& a = *change.position;
            const auto p = static_cast<unsigned>(a.point), r = static_cast<unsigned>(a.targetPoint);
            const auto screen = tree.Nodes().front().rect;
            left = screen.left + screen.Width() * x[r] + a.x - original.Width() * scale * x[p];
            bottom = screen.bottom + screen.Height() * y[r] + a.y - original.Height() * scale * y[p];
        }
        std::set<std::uint32_t> transformed;
        const auto transform = [&](auto&& self, std::uint32_t id) -> void {
            if (!transformed.insert(id).second) return;
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
        // CInfoBar的物品面板拥有六个按钮与标题；代码区域表按屏幕平铺，仍需反映其运行时父变换。
        if (change.name == "InfoBar") {
            for (const auto& node : tree.Nodes()) {
                const bool inventory = node.name.size() == 16 && node.name.starts_with("InventoryButton") && node.name.back() >= '1' && node.name.back() <= '6';
                if (!node.codeLabel.empty() && (inventory || node.name == "InventoryTitle")) {
                    transform(transform, node.id);
                    if (change.visible && !*change.visible) tree.MutableNode(node.id).hidden = true;
                }
            }
        }
    }
    return diagnostics;
}

std::vector<Diagnostic> CheckUiOriginSupport(const UiDescription& description) {
    std::vector<Diagnostic> result;
    for (const auto& change : description.origins) {
        const auto* part = FindUiOriginPart(change.name);
        std::string fields;
        const auto unsupported = [&](bool requested, bool supported, const char* field) {
            if (requested && !supported) { if (!fields.empty()) fields += "、"; fields += field; }
        };
        unsupported(change.visible.has_value(), part && part->visibility, "显示/隐藏");
        unsupported(change.position.has_value(), part && part->position, "位置");
        unsupported(change.scale.has_value(), part && part->scale, "缩放");
        unsupported(change.alpha.has_value(), part && part->alpha, "透明度");
        if (part && part->scaleRequiresPosition && change.scale && !change.position) {
            if (!fields.empty()) fields += "、";
            fields += "缺少显式位置的缩放（默认锚点偏移也会被原版缩放）";
        }
        if (!part || !fields.empty()) {
            result.push_back(detail::MakeUiError("AUI_ORIGIN_UNSUPPORTED", "原版节点 " + change.name + " 的" + fields + "不支持：" + (part ? part->reason : "没有该节点")));
        }
    }
    return result;
}
} // namespace War3Map::GameUi
