#include "War3MapCore/GameUi/FrameDraw.h"

#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace War3Map::GameUi {
namespace {

// 只在按下、禁用、悬停、焦点或勾选时显示的控件子框架（由父控件的这些属性引用）。
constexpr std::array<std::string_view, 7> kStateOnlyFillKeys{"controlpushedbackdrop", "controldisabledbackdrop",
    "controldisabledpushedbackdrop", "controlmouseoverhighlight", "controlfocushighlight", "checkboxcheckhighlight",
    "checkboxdisabledcheckhighlight"};

// 边框贴图横向 8 格：左、右、上、下、左上、右上、左下、右下；上、下两格竖着存放，绘制时顺时针转 90°。
enum EdgeCell { EdgeLeft, EdgeRight, EdgeTop, EdgeBottom, EdgeUpperLeft, EdgeUpperRight, EdgeLowerLeft, EdgeLowerRight };

bool IsStateOnly(const FrameNode& node) {
    if (Text::LowerAscii(node.frameType) == "highlight") return true;
    if (node.autoFillBy.empty()) return false;
    const std::string key = Text::LowerAscii(node.autoFillBy);
    return std::find(kStateOnlyFillKeys.begin(), kStateOnlyFillKeys.end(), key) != kStateOnlyFillKeys.end();
}

bool IsVisibleLayout(const FrameNode& node) {
    return node.layout == LayoutState::Placed || node.layout == LayoutState::SizeFromContent;
}

int LayerRank(std::string_view layer) {
    const std::string lower = Text::LowerAscii(layer);
    if (lower == "background") return 0;
    if (lower == "border") return 1;
    if (lower == "overlay") return 3;
    return 2;  // ARTWORK 与未写层
}

std::array<TexturePoint, 4> AxisUv(float left, float right, float top, float bottom) {
    return {TexturePoint{left, top}, TexturePoint{right, top}, TexturePoint{left, bottom}, TexturePoint{right, bottom}};
}

ImageDraw MakeImage(const TextureInfo& texture, const FrameRect& rect) {
    ImageDraw image;
    image.path = texture.path;
    image.source = texture.source;
    image.rect = rect;
    image.uv = AxisUv(texture.texCoord[0], texture.texCoord[1], texture.texCoord[2], texture.texCoord[3]);
    image.blend = ParseBlend(texture.alphaMode);
    return image;
}

JustifyH ParseJustifyH(std::string_view text, JustifyH fallback) {
    const std::string lower = Text::LowerAscii(text);
    if (lower == "justifyleft") return JustifyH::Left;
    if (lower == "justifyright") return JustifyH::Right;
    if (lower == "justifycenter") return JustifyH::Center;
    return fallback;
}

JustifyV ParseJustifyV(std::string_view text, JustifyV fallback) {
    const std::string lower = Text::LowerAscii(text);
    if (lower == "justifytop") return JustifyV::Top;
    if (lower == "justifybottom") return JustifyV::Bottom;
    if (lower == "justifymiddle") return JustifyV::Middle;
    return fallback;
}

class Builder {
public:
    Builder(const FrameTree& tree, const FrameDrawOptions& options) : tree_(tree), options_(options) {}

    FrameDrawList Run() {
        for (const std::uint32_t root : tree_.Nodes()[kScreenNode].children) Visit(root);
        return std::move(list_);
    }

private:
    const FrameNode& Node(std::uint32_t id) const { return tree_.Nodes()[id]; }
    bool Hidden(std::uint32_t id) const {
        return options_.hidden.contains(id) || (!options_.drawScriptHidden && Node(id).hidden);
    }

    void Visit(std::uint32_t id) {
        if (Hidden(id)) return;
        const FrameNode& node = Node(id);
        if (node.kind != DefinitionKind::Frame) return;  // 贴图、文字与层由所属框架按层处理
        if (IsStateOnly(node)) {
            list_.stateOnlyNodes.push_back(id);
            return;
        }
        if (IsVisibleLayout(node)) {
            if (!node.codeLabel.empty()) {
                if (node.children.empty()) PushCodeArea(node);
            } else {
                EmitFrame(node);
            }
        }
        EmitLayered(node);
        for (const std::uint32_t child : node.children) {
            if (Node(child).kind == DefinitionKind::Frame) Visit(child);
        }
    }

    // SIMPLEFRAME 的贴图与文字：直接子节点与 Layer 内的子节点一起按层排序，同层保持定义顺序。
    void EmitLayered(const FrameNode& frame) {
        std::vector<std::pair<int, std::uint32_t>> items;
        for (const std::uint32_t child : frame.children) {
            if (Hidden(child)) continue;
            const FrameNode& node = Node(child);
            if (node.kind == DefinitionKind::Layer) {
                for (const std::uint32_t inner : node.children) {
                    if (!Hidden(inner)) items.emplace_back(LayerRank(node.frameType), inner);
                }
            } else if (node.kind == DefinitionKind::Texture || node.kind == DefinitionKind::String) {
                items.emplace_back(LayerRank(node.layer), child);
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [rank, id] : items) {
            (void)rank;
            const FrameNode& node = Node(id);
            if (!IsVisibleLayout(node)) continue;
            if (node.kind == DefinitionKind::Texture && node.texture) PushImage(id, MakeImage(*node.texture, node.rect));
            if (node.kind == DefinitionKind::String) PushText(node);
        }
    }

    void EmitFrame(const FrameNode& node) {
        const std::string type = Text::LowerAscii(node.frameType);
        if (type == "backdrop") {
            EmitBackdrop(node);
        } else if (type == "text") {
            PushText(node);
        } else if (type == "simplebutton") {
            if (node.texture) PushImage(node.id, MakeImage(*node.texture, node.rect));
            PushText(node);
        } else if (type == "simplestatusbar") {
            if (node.background) PushImage(node.id, MakeImage(*node.background, node.rect));
            if (node.texture) PushFill(node);
        } else if (node.uiFrame && type == "gluetextbutton") {
            // 界面描述的按钮只有常态贴图与文字；游戏界面文件里的按钮由子框架绘制。
            if (node.texture) PushImage(node.id, MakeImage(*node.texture, node.rect));
            PushText(node);
        }
    }

    // 状态条填充：矩形与贴图横向按同一比例裁剪，贴图不被压缩。
    void PushFill(const FrameNode& node) {
        const float fill = std::clamp(node.fill.value_or(1.0f), 0.0f, 1.0f);
        if (fill <= 0.0f) return;
        TextureInfo texture = *node.texture;
        texture.texCoord[1] = texture.texCoord[0] + (texture.texCoord[1] - texture.texCoord[0]) * fill;
        FrameRect rect = node.rect;
        rect.right = rect.left + rect.Width() * fill;
        PushImage(node.id, MakeImage(texture, rect));
    }

    void EmitBackdrop(const FrameNode& node) {
        const auto& properties = node.properties;
        const auto number = [&](std::string_view key, std::size_t index, float fallback) {
            const FdfProperty* property = FindProperty(properties, key);
            return property ? PropertyNumber(*property, index).value_or(fallback) : fallback;
        };
        if (node.texture) {
            // 背景内缩顺序按 左、右、上、下 读取（游戏文件四个值通常相同）。
            FrameRect inner = node.rect;
            inner.left += number("backdropbackgroundinsets", 0, 0.0f);
            inner.right -= number("backdropbackgroundinsets", 1, 0.0f);
            inner.top -= number("backdropbackgroundinsets", 2, 0.0f);
            inner.bottom += number("backdropbackgroundinsets", 3, 0.0f);
            if (inner.Width() > 0.0f && inner.Height() > 0.0f) {
                ImageDraw image = MakeImage(*node.texture, inner);
                const float tile = number("backdropbackgroundsize", 0, 0.0f);
                if (FindProperty(properties, "backdroptilebackground") && tile > 0.0f) {
                    image.uv = AxisUv(0.0f, inner.Width() / tile, 0.0f, inner.Height() / tile);
                    image.repeat = true;
                }
                PushImage(node.id, std::move(image));
            }
        }
        if (!node.edge) return;
        const float corner = number("backdropcornersize", 0, 0.0f);
        if (corner <= 0.0f) return;
        std::string flags = "UL|UR|BL|BR|T|L|B|R";
        if (const FdfProperty* property = FindProperty(properties, "backdropcornerflags"); property && !property->values.empty()) {
            flags = property->values[0].text;
        }
        const std::string upperFlags = Text::UpperAscii(flags);
        const auto pieces = Text::SplitPlain(upperFlags, '|');
        const auto has = [&](std::string_view piece) {
            return std::any_of(pieces.begin(), pieces.end(), [&](std::string_view item) { return Text::TrimHorizontal(item) == piece; });
        };
        const FrameRect& r = node.rect;
        const float verticalRepeat = std::max(0.0f, (r.Height() - 2.0f * corner) / corner);
        const float horizontalRepeat = std::max(0.0f, (r.Width() - 2.0f * corner) / corner);
        const auto cell = [](int index) { return std::pair{index / 8.0f, (index + 1) / 8.0f}; };
        const auto push = [&](FrameRect rect, std::array<TexturePoint, 4> uv, bool repeat) {
            if (rect.Width() <= 0.0f || rect.Height() <= 0.0f) return;
            ImageDraw image;
            image.path = node.edge->path;
            image.source = node.edge->source;
            image.rect = rect;
            image.uv = uv;
            image.repeat = repeat;
            PushImage(node.id, std::move(image));
        };
        const auto upright = [&](int index, FrameRect rect, float repeat) {
            const auto [u0, u1] = cell(index);
            push(rect, AxisUv(u0, u1, 0.0f, repeat), true);
        };
        // 顺时针转 90°：格子的左列落到目标上边，格子的 v 方向沿目标水平方向重复。
        const auto rotated = [&](int index, FrameRect rect, float repeat) {
            const auto [u0, u1] = cell(index);
            push(rect, {TexturePoint{u0, repeat}, TexturePoint{u0, 0.0f}, TexturePoint{u1, repeat}, TexturePoint{u1, 0.0f}}, true);
        };
        const auto cornerPiece = [&](int index, FrameRect rect) {
            const auto [u0, u1] = cell(index);
            push(rect, AxisUv(u0, u1, 0.0f, 1.0f), false);
        };
        if (has("L")) upright(EdgeLeft, {r.left, r.bottom + corner, r.left + corner, r.top - corner}, verticalRepeat);
        if (has("R")) upright(EdgeRight, {r.right - corner, r.bottom + corner, r.right, r.top - corner}, verticalRepeat);
        if (has("T")) rotated(EdgeTop, {r.left + corner, r.top - corner, r.right - corner, r.top}, horizontalRepeat);
        if (has("B")) rotated(EdgeBottom, {r.left + corner, r.bottom, r.right - corner, r.bottom + corner}, horizontalRepeat);
        if (has("UL")) cornerPiece(EdgeUpperLeft, {r.left, r.top - corner, r.left + corner, r.top});
        if (has("UR")) cornerPiece(EdgeUpperRight, {r.right - corner, r.top - corner, r.right, r.top});
        if (has("BL")) cornerPiece(EdgeLowerLeft, {r.left, r.bottom, r.left + corner, r.bottom + corner});
        if (has("BR")) cornerPiece(EdgeLowerRight, {r.right - corner, r.bottom, r.right, r.bottom + corner});
    }

    void PushImage(std::uint32_t node, ImageDraw image) {
        if (image.path.empty()) return;
        FrameDrawCommand command;
        command.node = node;
        command.kind = DrawKind::Image;
        command.image = std::move(image);
        list_.commands.push_back(std::move(command));
    }

    void PushCodeArea(const FrameNode& node) {
        FrameDrawCommand command;
        command.node = node.id;
        command.kind = DrawKind::CodeArea;
        command.image.rect = node.rect;
        list_.commands.push_back(std::move(command));
    }

    void PushText(const FrameNode& node) {
        if (!node.text || node.text->text.empty()) return;
        FrameDrawCommand command;
        command.node = node.id;
        command.kind = DrawKind::Text;
        TextDraw& text = command.text;
        text.text = node.text->text;
        text.fontPath = node.text->fontPath;
        text.fontHeight = node.text->fontHeight;
        text.color = node.text->color;
        text.shadowColor = node.text->shadowColor;
        text.shadowOffset = node.text->shadowOffset;
        text.rect = node.rect;
        text.sizeFromContent = node.layout == LayoutState::SizeFromContent;
        text.anchor = node.contentAnchor;
        text.justifyH = ParseJustifyH(node.text->justifyH, JustifyH::Center);
        text.justifyV = ParseJustifyV(node.text->justifyV, JustifyV::Middle);
        list_.commands.push_back(std::move(command));
    }

    const FrameTree& tree_;
    const FrameDrawOptions& options_;
    FrameDrawList list_;
};

} // namespace

DrawBlend ParseBlend(std::string_view alphaMode) {
    const std::string lower = Text::LowerAscii(alphaMode);
    if (lower == "add") return DrawBlend::Add;
    if (lower == "alphakey") return DrawBlend::AlphaKey;
    if (lower == "mod" || lower == "modulate") return DrawBlend::Modulate;
    return DrawBlend::Blend;
}

FrameDrawList BuildDrawList(const FrameTree& tree, const FrameDrawOptions& options) {
    return Builder(tree, options).Run();
}

} // namespace War3Map::GameUi
