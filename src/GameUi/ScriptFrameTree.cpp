#include "War3MapCore/GameUi/ScriptFrames.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"

#include <algorithm>
#include <charconv>
#include <cmath>

// 脚本框架挂到界面树上，以及预览未还原的界面调用的问题行。
namespace War3Map::GameUi {
namespace {

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, "GameUi", code, std::move(message), std::nullopt};
}

// 整段为 TRIGSTR_编号 的文字换成地图字符串表里的内容；表里没有时原样显示。
std::string DisplayText(const ScriptFramePlan& plan, const std::string& text) {
    constexpr std::string_view prefix = "TRIGSTR_";
    if (text.size() <= prefix.size() || !text.starts_with(prefix)) return text;
    std::uint32_t id = 0;
    const char* end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data() + prefix.size(), end, id);
    if (error != std::errc() || stop != end) return text;
    const auto found = plan.strings.find(id);
    return found == plan.strings.end() ? text : found->second;
}

std::optional<std::uint32_t> FindDescendant(const FrameTree& tree, std::uint32_t node, std::string_view name) {
    for (const std::uint32_t child : tree.Nodes()[node].children) {
        if (tree.Nodes()[child].name == name) return child;
        if (const auto found = FindDescendant(tree, child, name)) return found;
    }
    return std::nullopt;
}

void SetText(FrameTree& tree, std::uint32_t node, std::string text) {
    FrameNode& target = tree.MutableNode(node);
    if (!target.text && !IsTextNode(target)) return;
    if (!target.text) target.text = TextInfo{};
    target.text->key.clear();
    target.text->text = std::move(text);
    target.text->source = "script";
}

// 原版界面对象标题所在的文字节点。
std::string_view TitleNode(ScriptObjectKind object) {
    switch (object) {
    case ScriptObjectKind::TimerDialog: return "TimerDialogTitle";
    case ScriptObjectKind::Dialog: return "ScriptDialogText";
    case ScriptObjectKind::Leaderboard: return "LeaderboardTitle";
    case ScriptObjectKind::Multiboard: return "MultiboardTitle";
    default: return {};
    }
}

FrameAnchor TopTo(std::uint32_t target, std::string name, float y) {
    FrameAnchor anchor{FramePoint::Top, std::move(name), FramePoint::Top, 0.0f, y};
    anchor.targetNode = target;
    return anchor;
}

// 对话框按游戏规则排布（1.27a 添加按钮与重排对话框的代码）：消息在上，按钮间隔 0.003 自上而下；
// 高度 = 上下边距各 0.03 + 消息（文字高 + 0.011）+ 按钮，不足 0.096 取 0.096，否则向上取到 0.048 的整数倍，
// 多出的高度上下平分。消息只按 |n 与换行计行数，不按宽度折行。
void LayoutDialog(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin, std::uint32_t dialog,
    const ScriptFrame& frame, const ScriptFramePlan& plan, std::vector<Diagnostic>& diagnostics) {
    const std::string name = tree.Nodes()[dialog].name;
    const std::string message = frame.text ? DisplayText(plan, *frame.text) : std::string();
    const auto textNode = FindDescendant(tree, dialog, "ScriptDialogText");
    float textHeight = 0.0f;
    if (!message.empty()) {
        float font = textNode && tree.Nodes()[*textNode].text ? tree.Nodes()[*textNode].text->fontHeight : 0.0f;
        if (font <= 0.0f) font = 0.013f;
        std::size_t lines = 1 + static_cast<std::size_t>(std::count(message.begin(), message.end(), '\n'));
        for (std::size_t at = message.find('|'); at != std::string::npos; at = message.find('|', at + 2)) {
            if (at + 1 < message.size() && (message[at + 1] == 'n' || message[at + 1] == 'N')) ++lines;
        }
        textHeight = font * static_cast<float>(lines);
    }
    std::vector<std::uint32_t> buttons;
    for (const auto& text : frame.buttons) {
        auto built = tree.InstantiateUnder(library, skin, "ScriptDialogButton", dialog);
        if (!built.value) {
            diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SCRIPT_TEMPLATE_MISSING",
                name + " 的按钮模板 ScriptDialogButton 不在已读入的界面目录里"));
            break;
        }
        buttons.push_back(*built.value);
        if (const auto label = FindDescendant(tree, *built.value, "ScriptDialogButtonText")) {
            SetText(tree, *label, DisplayText(plan, text));
        }
    }
    const float buttonHeight = buttons.empty() ? 0.0f : tree.Nodes()[buttons.front()].height.value_or(0.035f);
    const float messagePart = message.empty() ? 0.03f : textHeight + 0.008f + 0.003f + 0.03f;
    const float buttonPart = buttons.empty()
        ? 0.03f
        : (buttonHeight + 0.003f) * static_cast<float>(buttons.size()) - 0.003f + 0.03f;
    const float total = messagePart + buttonPart;
    float height = 0.096f;
    float messageY = -0.03f;
    if (total >= 0.096f) {
        const float rest = std::fmod(total, 0.048f);
        height = rest == 0.0f ? total : total - rest + 0.048f;
        messageY = -(height - (total - 0.06f)) * 0.5f;
    }
    tree.MutableNode(dialog).height = height;
    if (textNode) {
        FrameNode& text = tree.MutableNode(*textNode);
        text.anchors.clear();
        text.anchors.push_back(TopTo(dialog, name, messageY));
    }
    float top = message.empty() ? -0.027f : messageY - textHeight - 0.011f;
    for (const std::uint32_t button : buttons) {
        FrameNode& node = tree.MutableNode(button);
        node.anchors.clear();
        node.anchors.push_back(TopTo(dialog, name, top));
        top -= buttonHeight + 0.003f;
    }
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
        node.hidden = frame.visible && !*frame.visible;
        if (frame.width) node.width = frame.width;
        if (frame.height) node.height = frame.height;
        if (frame.texture) {
            TextureInfo texture;
            texture.key = *frame.texture;
            texture.path = EnsureTextureExtension(*frame.texture);
            texture.source = "script";
            node.texture = std::move(texture);
        }
        if (frame.object != ScriptObjectKind::Frame) {
            // 原版界面对象：游戏按固定位置摆放（多个同时显示时的排开规则见 GameCodeLayout），
            // 预览按各自单独显示时的位置画。
            node.anchors.clear();
            if (const auto placement = KnownRootPlacement(frame.templateName)) {
                for (FrameAnchor anchor : placement->anchors) {
                    anchor.targetNode = kScreenNode;
                    anchor.relativeName = "Screen";
                    node.anchors.push_back(std::move(anchor));
                }
            }
            if (frame.text) {
                if (const auto title = FindDescendant(tree, *id, TitleNode(frame.object))) SetText(tree, *title, DisplayText(plan, *frame.text));
            }
            if (frame.object == ScriptObjectKind::Dialog) LayoutDialog(tree, library, skin, *id, frame, plan, diagnostics);
        } else if (frame.text) {
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
            SetText(tree, textNode, DisplayText(plan, *frame.text));
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
