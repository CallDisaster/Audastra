#include "War3MapCore/GameUi/UiDescription.h"

#include "UiBindingCheck.h"
#include "War3MapCore/GameUi/GameCodeLayout.h"

#include <algorithm>
#include <charconv>
#include <unordered_map>

// 界面描述挂到界面树：按当前数值与状态算出每个框架的贴图、文字、显示与进度，锚点最后解析。
namespace War3Map::GameUi {
namespace {

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, "GameUi", code, std::move(message), std::nullopt};
}

std::string_view FrameTypeOf(UiFrameType type) {
    switch (type) {
    case UiFrameType::Backdrop: return "BACKDROP";
    case UiFrameType::Text: return "TEXT";
    case UiFrameType::Bar: return "SIMPLESTATUSBAR";
    case UiFrameType::Button: return "GLUETEXTBUTTON";
    default: return "FRAME";
    }
}

std::string_view JustifyName(JustifyH justify) {
    switch (justify) {
    case JustifyH::Left: return "JUSTIFYLEFT";
    case JustifyH::Right: return "JUSTIFYRIGHT";
    default: return "JUSTIFYCENTER";
    }
}

std::string_view JustifyName(JustifyV justify) {
    switch (justify) {
    case JustifyV::Top: return "JUSTIFYTOP";
    case JustifyV::Bottom: return "JUSTIFYBOTTOM";
    default: return "JUSTIFYMIDDLE";
    }
}

class Builder {
public:
    Builder(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin, const UiDescription& description,
        const UiInstanceState& state, std::vector<Diagnostic>& diagnostics)
        : tree_(tree), library_(library), skin_(skin), description_(description), state_(state), diagnostics_(diagnostics),
          source_(UiDescriptionMemberPath(description.id)) {
        if (!state.state.empty()) {
            const auto found = std::find_if(description.states.begin(), description.states.end(),
                [&](const UiState& candidate) { return candidate.id == state.state; });
            if (found != description.states.end()) active_ = &*found;
            else diagnostics_.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "AUI_STATE_UNKNOWN",
                     source_ + "：没有状态 " + state.state + "，按基础状态显示"));
        }
    }

    std::uint32_t Run() {
        FrameNode root;
        root.parent = kScreenNode;
        root.frameType = std::string(kUiDescriptionRootType);
        root.name = description_.id;
        root.sourceFile = source_;
        root.uiFrame = UiFrameLink{description_.id, {}, std::nullopt};
        root.anchors.push_back(FrameAnchor{FramePoint::TopLeft, {}, FramePoint::TopLeft, 0.0f, 0.0f});
        root.anchors.push_back(FrameAnchor{FramePoint::BottomRight, {}, FramePoint::BottomRight, 0.0f, 0.0f});
        const std::uint32_t rootId = tree_.AddFrame(std::move(root));
        for (const auto& frame : description_.frames) Build(frame, rootId, std::nullopt);
        for (const auto& placed : placed_) Anchor(placed);
        return rootId;
    }

private:
    struct Placed {
        const UiFrame* frame = nullptr;
        std::uint32_t node = kNoNode;
        std::optional<std::uint32_t> instance;
        bool repeatRoot = false;
    };

    static std::string Key(std::string_view id, std::optional<std::uint32_t> instance) {
        return instance ? std::string(id) + "#" + std::to_string(*instance) : std::string(id);
    }

    const UiStateOverride* Override(const std::string& frame) const {
        if (!active_) return nullptr;
        for (const auto& entry : active_->overrides) {
            if (entry.frame == frame) return &entry;
        }
        return nullptr;
    }

    std::optional<std::string> BoundText(const UiBinding& binding, std::optional<std::uint32_t> instance) const {
        const std::optional<std::uint32_t> index = binding.perInstance ? instance : binding.index;
        if (binding.perInstance && !instance) return std::nullopt;
        return UiValueText(description_, state_, binding.value, index);
    }

    double Number(const UiNumber& number, std::optional<std::uint32_t> instance) const {
        if (!number.binding) return number.literal;
        const auto text = BoundText(*number.binding, instance);
        double value = 0.0;
        if (text) {
            if (*text == "true") value = 1.0;
            else if (*text != "false") std::from_chars(text->data(), text->data() + text->size(), value);
        }
        return value * number.binding->scale + number.binding->offset;
    }

    bool Flag(const UiFlag& flag, std::optional<std::uint32_t> instance) const {
        if (!flag.binding) return flag.literal;
        const auto text = BoundText(*flag.binding, instance);
        const UiValue* spec = detail::FindValue(description_, flag.binding->value);
        bool on = false;
        if (text && spec && spec->type == UiValueType::String) {
            on = !text->empty();
        } else if (text) {
            if (*text == "true" || *text == "false") {
                on = *text == "true";
            } else {
                double value = 0.0;
                std::from_chars(text->data(), text->data() + text->size(), value);
                on = value != 0.0;
            }
        }
        return on != flag.binding->invert;
    }

    TextureInfo Texture(const std::string& path) const {
        TextureInfo texture;
        texture.key = path;
        texture.path = EnsureTextureExtension(path);
        texture.source = "description";
        return texture;
    }

    void Build(const UiFrame& frame, std::uint32_t parent, std::optional<std::uint32_t> instance) {
        if (frame.repeat && !instance) {
            for (std::uint32_t i = 0; i < frame.repeat->count; ++i) BuildOne(frame, parent, i, true);
        } else {
            BuildOne(frame, parent, instance, false);
        }
    }

    void BuildOne(const UiFrame& frame, std::uint32_t parent, std::optional<std::uint32_t> instance, bool repeatRoot) {
        std::optional<std::uint32_t> id;
        if (frame.type == UiFrameType::Template) {
            auto built = tree_.InstantiateUnder(library_, skin_, frame.templateName, parent);
            if (built.value) {
                id = built.value;
                diagnostics_.insert(diagnostics_.end(), built.diagnostics.begin(), built.diagnostics.end());
            } else {
                diagnostics_.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "AUI_TEMPLATE_MISSING",
                    source_ + "：框架 " + frame.id + " 用的模板 " + frame.templateName + " 不在已读入的界面目录里"));
            }
        }
        if (!id) {
            FrameNode node;
            node.parent = parent;
            node.frameType = std::string(FrameTypeOf(frame.type));
            id = tree_.AddFrame(std::move(node));
        }
        FrameNode& node = tree_.MutableNode(*id);
        node.name = Key(frame.id, instance);
        node.sourceFile = source_;
        node.uiFrame = UiFrameLink{description_.id, frame.id, instance};
        if (frame.width) node.width = static_cast<float>(*frame.width);
        if (frame.height) node.height = static_cast<float>(*frame.height);
        const UiStateOverride* change = Override(frame.id);
        node.hidden = !(change && change->visible ? *change->visible : Flag(frame.visible, instance));
        const std::string texture = change && change->texture ? *change->texture : FormatUiText(description_, state_, frame.texture, instance);
        if (!texture.empty()) node.texture = Texture(texture);
        if (!frame.background.empty()) node.background = Texture(frame.background);
        if (frame.type == UiFrameType::Bar) {
            node.fill = static_cast<float>(change && change->value ? *change->value : Number(frame.value, instance));
        }
        if (frame.type == UiFrameType::Text || frame.type == UiFrameType::Button) {
            TextInfo text;
            text.text = FormatUiText(description_, state_, change && change->text ? *change->text : frame.text, instance);
            text.source = "description";
            text.fontPath = frame.font;
            text.fontHeight = static_cast<float>(frame.fontHeight);
            for (std::size_t i = 0; i < 4; ++i) text.color[i] = static_cast<float>(frame.color[i]);
            text.justifyH = std::string(JustifyName(frame.justifyH));
            text.justifyV = std::string(JustifyName(frame.justifyV));
            node.text = std::move(text);
        }
        nodes_.emplace(Key(frame.id, instance), *id);
        placed_.push_back(Placed{&frame, *id, instance, repeatRoot});
        for (const auto& child : frame.children) Build(child, *id, instance);
    }

    // 同一重复实例内的框架优先，其次第一个实例，最后不在重复范围内的框架。
    std::optional<std::uint32_t> Resolve(const std::string& frame, std::optional<std::uint32_t> instance) const {
        for (const auto& key : {instance ? Key(frame, instance) : std::string(), Key(frame, 0u), frame}) {
            if (key.empty()) continue;
            if (const auto found = nodes_.find(key); found != nodes_.end()) return found->second;
        }
        return std::nullopt;
    }

    void Anchor(const Placed& placed) {
        const UiFrame& frame = *placed.frame;
        if (frame.anchors.empty()) return;
        // 这一步不再添加节点，引用一直有效。
        FrameNode& node = tree_.MutableNode(placed.node);
        node.anchors.clear();  // 描述的锚点取代模板自带的锚点
        const float shiftX = placed.repeatRoot ? static_cast<float>(frame.repeat->dx * *placed.instance) : 0.0f;
        const float shiftY = placed.repeatRoot ? static_cast<float>(frame.repeat->dy * *placed.instance) : 0.0f;
        for (const auto& anchor : frame.anchors) {
            FrameAnchor resolved{anchor.point, {}, anchor.targetPoint, static_cast<float>(anchor.x) + shiftX,
                static_cast<float>(anchor.y) + shiftY};
            switch (anchor.target) {
            case UiAnchorTarget::Parent:
                break;
            case UiAnchorTarget::Screen:
                resolved.targetNode = kScreenNode;
                resolved.relativeName = "Screen";
                break;
            case UiAnchorTarget::Frame: {
                const auto target = Resolve(anchor.targetId, placed.instance);
                if (!target) continue;  // 检查时已报告
                resolved.targetNode = *target;
                resolved.relativeName = tree_.Nodes()[*target].name;
                break;
            }
            case UiAnchorTarget::GameArea:
                if (const auto area = tree_.FindCodeArea(anchor.targetId)) {
                    resolved.targetNode = *area;
                } else if (anchor.targetId == kGameCodeRoot) {
                    resolved.targetNode = kScreenNode;  // 游戏界面根铺满屏幕
                } else {
                    diagnostics_.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "AUI_ANCHOR_AREA",
                        source_ + "：框架 " + frame.id + " 锚在游戏界面区域 " + anchor.targetId + "，显示游戏界面后才能定位"));
                    continue;
                }
                resolved.relativeName = anchor.targetId;
                break;
            }
            node.anchors.push_back(std::move(resolved));
        }
    }

    FrameTree& tree_;
    const FrameLibrary& library_;
    const UiSkin* skin_;
    const UiDescription& description_;
    const UiInstanceState& state_;
    std::vector<Diagnostic>& diagnostics_;
    std::string source_;
    const UiState* active_ = nullptr;
    std::unordered_map<std::string, std::uint32_t> nodes_;
    std::vector<Placed> placed_;
};

} // namespace

std::uint32_t AddUiDescription(FrameTree& tree, const FrameLibrary& library, const UiSkin* skin,
    const UiDescription& description, const UiInstanceState& state, std::vector<Diagnostic>& diagnostics) {
    return Builder(tree, library, skin, description, state, diagnostics).Run();
}

} // namespace War3Map::GameUi
