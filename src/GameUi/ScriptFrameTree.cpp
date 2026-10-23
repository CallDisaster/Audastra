#include "War3MapCore/GameUi/ScriptFrames.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"

#include <algorithm>

// 脚本框架挂到界面树上，以及预览未还原的界面调用的问题行。
namespace War3Map::GameUi {
namespace {

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, "GameUi", code, std::move(message), std::nullopt};
}

} // namespace

std::vector<Diagnostic> ScriptCallDiagnostics(const ScriptFramePlan& plan) {
    std::vector<Diagnostic> diagnostics;
    for (const auto& group : plan.calls) {
        const char* code = "GAMEUI_SCRIPT_CALL_NOT_PREVIEWED";
        std::string reason = "预览不还原";
        if (group.reason == ScriptUiCallReason::UntracedTarget) {
            code = "GAMEUI_SCRIPT_CALL_UNTRACED";
            reason = "作用的框架不是脚本创建后保存的变量";
        } else if (group.reason == ScriptUiCallReason::Unknown) {
            code = "GAMEUI_SCRIPT_CALL_UNKNOWN";
            reason = "未识别";
        }
        std::string sites;
        for (const auto& sample : group.samples) {
            if (!sites.empty()) sites += "、";
            sites += sample.scriptFile + ":" + std::to_string(sample.line);
        }
        if (group.count > group.samples.size()) sites += " 等";
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, code,
            group.function + "（" + std::string(UiApiFamilyName(group.family)) + "）" + std::to_string(group.count) +
                " 处，" + reason + "：" + sites));
    }
    return diagnostics;
}

std::uint32_t AddScriptFrames(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin,
    const ScriptFramePlan& plan, std::vector<Diagnostic>& diagnostics) {
    if (plan.frames.empty()) return kNoNode;
    FrameNode root;
    root.parent = kScreenNode;
    root.frameType = "SCRIPT";
    root.name = std::string(kScriptRoot);
    root.anchors.push_back(FrameAnchor{FramePoint::TopLeft, {}, FramePoint::TopLeft, 0.0f, 0.0f});
    root.anchors.push_back(FrameAnchor{FramePoint::BottomRight, {}, FramePoint::BottomRight, 0.0f, 0.0f});
    const std::uint32_t rootId = tree.AddFrame(std::move(root));

    const auto where = [](const ScriptFrame& frame) {
        return frame.created.scriptFile + " 第 " + std::to_string(frame.created.line) + " 行";
    };
    std::vector<std::uint32_t> nodeOf(plan.frames.size(), kNoNode);
    for (std::size_t i = 0; i < plan.frames.size(); ++i) {
        const ScriptFrame& frame = plan.frames[i];
        std::uint32_t parent = rootId;
        if (frame.parent.target == ScriptFrameTarget::Script && frame.parent.frame < i &&
            nodeOf[frame.parent.frame] != kNoNode) {
            parent = nodeOf[frame.parent.frame];
        }
        std::optional<std::uint32_t> id;
        if (!frame.templateName.empty()) {
            auto built = tree.InstantiateUnder(library, skin, frame.templateName, parent);
            if (built.value) {
                id = built.value;
                diagnostics.insert(diagnostics.end(), built.diagnostics.begin(), built.diagnostics.end());
            } else {
                diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SCRIPT_TEMPLATE_MISSING",
                    where(frame) + "创建框架用的模板 " + frame.templateName + " 不在已读入的界面目录里"));
            }
        }
        if (!id) {
            FrameNode node;
            node.parent = parent;
            node.frameType = frame.frameType.empty() ? "FRAME" : frame.frameType;
            id = tree.AddFrame(std::move(node));
        }
        nodeOf[i] = *id;
        FrameNode& node = tree.MutableNode(*id);
        node.name = frame.name;
        if (!frame.frameType.empty()) node.frameType = frame.frameType;
        node.sourceFile = frame.created.scriptFile;
        node.sourceLine = frame.created.line;
        node.scriptFrame = static_cast<std::uint32_t>(i);
        if (frame.width) node.width = frame.width;
        if (frame.height) node.height = frame.height;
        if (frame.texture) {
            TextureInfo texture;
            texture.key = *frame.texture;
            texture.path = EnsureTextureExtension(*frame.texture);
            texture.source = "script";
            node.texture = std::move(texture);
        }
        if (frame.text) {
            // 按钮类框架的文字在它的文字子框架上。
            std::uint32_t textNode = *id;
            if (!node.text && !IsTextNode(node)) {
                for (const std::uint32_t child : node.children) {
                    if (tree.Nodes()[child].text || IsTextNode(tree.Nodes()[child])) {
                        textNode = child;
                        break;
                    }
                }
            }
            FrameNode& target = tree.MutableNode(textNode);
            if (target.text || IsTextNode(target)) {
                if (!target.text) target.text = TextInfo{};
                target.text->key.clear();
                target.text->text = *frame.text;
                target.text->source = "script";
            }
        }
    }
    // 锚点最后处理：可以引用后创建的脚本框架。
    for (std::size_t i = 0; i < plan.frames.size(); ++i) {
        const ScriptFrame& frame = plan.frames[i];
        FrameNode& node = tree.MutableNode(nodeOf[i]);
        if (frame.clearPoints) node.anchors.clear();
        for (const auto& anchor : frame.anchors) {
            FrameAnchor resolved{anchor.point, anchor.relative.name, anchor.relativePoint, anchor.x, anchor.y};
            switch (anchor.relative.target) {
            case ScriptFrameTarget::Screen:
                resolved.targetNode = kScreenNode;
                resolved.relativeName = "Screen";
                break;
            case ScriptFrameTarget::CodeArea:
                if (const auto area = tree.FindCodeArea(anchor.relative.name)) {
                    resolved.targetNode = *area;
                } else if (anchor.relative.name == kGameCodeRoot) {
                    resolved.targetNode = kScreenNode;  // 游戏界面根铺满屏幕
                } else {
                    diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_SCRIPT_ANCHOR_AREA",
                        where(frame) + "的框架锚在游戏界面区域 " + anchor.relative.name + "，显示游戏界面后才能定位"));
                    continue;
                }
                break;
            case ScriptFrameTarget::Script:
                resolved.targetNode = nodeOf[anchor.relative.frame];
                resolved.relativeName = tree.Nodes()[resolved.targetNode].name;
                break;
            case ScriptFrameTarget::Named:
                break;  // 按名字查 fdf 框架
            default:
                continue;
            }
            const auto existing = std::find_if(node.anchors.begin(), node.anchors.end(),
                [&](const FrameAnchor& candidate) { return candidate.point == resolved.point; });
            if (existing != node.anchors.end()) *existing = std::move(resolved);
            else node.anchors.push_back(std::move(resolved));
        }
    }
    return rootId;
}

} // namespace War3Map::GameUi
