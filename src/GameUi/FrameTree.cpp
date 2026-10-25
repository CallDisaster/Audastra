#include "War3MapCore/GameUi/FrameTree.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <utility>

namespace War3Map::GameUi {
namespace {

constexpr const char* kSubsystem = "GameUi";
// 单棵树的节点上限与嵌套上限：防止模板互相 WITHCHILDREN 展开时失控。
constexpr std::size_t kMaxNodes = 20000;
constexpr int kMaxDepth = 64;

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, kSubsystem, code, std::move(message), std::nullopt};
}

int PointColumn(FramePoint point) {
    return static_cast<int>(point) % 3;  // 0 左，1 中，2 右
}

int PointRow(FramePoint point) {
    return static_cast<int>(point) / 3;  // 0 上，1 中，2 下
}

void UpsertAnchor(std::vector<FrameAnchor>& anchors, FrameAnchor anchor) {
    for (auto& existing : anchors) {
        if (existing.point == anchor.point) {
            existing = std::move(anchor);
            return;
        }
    }
    anchors.push_back(std::move(anchor));
}

// 一条轴上的约束求解：min/max/center 是两端与中点，size 是定义的尺寸。
struct AxisResult {
    bool constrained = false;
    bool degenerate = false;  // 只有一个点、没有尺寸
    float low = 0.0f;
    float high = 0.0f;
};

AxisResult SolveAxis(std::optional<float> low, std::optional<float> high, std::optional<float> center,
    std::optional<float> size) {
    AxisResult result;
    result.constrained = true;
    if (low && high) {
        result.low = *low;
        result.high = *high;
    } else if (low && size) {
        result.low = *low;
        result.high = *low + *size;
    } else if (high && size) {
        result.low = *high - *size;
        result.high = *high;
    } else if (center && size) {
        result.low = *center - *size * 0.5f;
        result.high = *center + *size * 0.5f;
    } else if (low && center) {
        result.low = *low;
        result.high = 2.0f * *center - *low;
    } else if (high && center) {
        result.low = 2.0f * *center - *high;
        result.high = *high;
    } else if (low || high || center) {
        const float at = low ? *low : (high ? *high : *center);
        result.low = at;
        result.high = at;
        result.degenerate = true;
    } else {
        result.constrained = false;
    }
    return result;
}

} // namespace

float FramePointX(const FrameRect& rect, FramePoint point) noexcept {
    switch (PointColumn(point)) {
    case 0: return rect.left;
    case 1: return (rect.left + rect.right) * 0.5f;
    default: return rect.right;
    }
}

float FramePointY(const FrameRect& rect, FramePoint point) noexcept {
    switch (PointRow(point)) {
    case 0: return rect.top;
    case 1: return (rect.top + rect.bottom) * 0.5f;
    default: return rect.bottom;
    }
}

bool IsTextNode(const FrameNode& node) {
    if (node.kind == DefinitionKind::String) return true;
    return node.kind == DefinitionKind::Frame && Text::LowerAscii(node.frameType) == "text";
}

std::string EnsureTextureExtension(std::string path) {
    const std::size_t slash = path.find_last_of("\\/");
    const std::size_t dot = path.find_last_of('.');
    if (!path.empty() && (dot == std::string::npos || (slash != std::string::npos && dot < slash))) path += ".blp";
    return path;
}

struct FrameTree::BuildContext {
    const FrameLibrary& library;
    const UiSkin* skin = nullptr;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> templateStack;
    bool limitReported = false;
};

namespace {

struct ChildRef {
    const FdfDefinition* definition = nullptr;
    std::string file;
};

// 把 definition 及其继承链展开：模板属性在前、自身在后；includeChildren 为真时收集子定义。
void AppendDefinition(const FrameLibrary& library, std::vector<std::string>& templateStack,
    std::vector<Diagnostic>& diagnostics, const FdfDefinition& definition, const std::string& file,
    std::vector<FdfProperty>& properties, std::vector<ChildRef>& children, bool includeChildren) {
    if (!definition.inherits.empty()) {
        const std::string key = Text::LowerAscii(definition.inherits);
        const auto found = library.FindTemplate(definition.inherits);
        if (!found) {
            diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_TEMPLATE_MISSING",
                file + " 第 " + std::to_string(definition.line) + " 行：找不到模板 " + definition.inherits));
        } else if (std::find(templateStack.begin(), templateStack.end(), key) != templateStack.end()) {
            diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_TEMPLATE_CYCLE",
                file + " 第 " + std::to_string(definition.line) + " 行：模板 " + definition.inherits + " 循环继承"));
        } else {
            if (found->definition->kind != definition.kind) {
                diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_TEMPLATE_KIND_MISMATCH",
                    file + " 第 " + std::to_string(definition.line) + " 行：模板 " + definition.inherits + " 的种类与本定义不同"));
            }
            templateStack.push_back(key);
            const std::string& templateFile = library.Files()[found->fileIndex]->file.path;
            AppendDefinition(library, templateStack, diagnostics, *found->definition, templateFile, properties, children,
                includeChildren && definition.inheritsWithChildren);
            templateStack.pop_back();
        }
    }
    properties.insert(properties.end(), definition.properties.begin(), definition.properties.end());
    if (includeChildren) {
        for (const auto& child : definition.children) children.push_back(ChildRef{&child, file});
    }
}

} // namespace

FrameTree::FrameTree(FrameTreeOptions options) : options_(std::move(options)) {
    FrameNode screen;
    screen.id = kScreenNode;
    screen.frameType = "SCREEN";
    screen.name = "Screen";
    screen.rect = FrameRect{0.0f, 0.0f, options_.screenWidth, options_.screenHeight};
    screen.width = options_.screenWidth;
    screen.height = options_.screenHeight;
    screen.layout = LayoutState::Placed;
    nodes_.push_back(std::move(screen));
}

std::uint32_t FrameTree::AddNode(FrameNode node) {
    node.id = static_cast<std::uint32_t>(nodes_.size());
    const std::uint32_t id = node.id;
    const std::uint32_t parent = node.parent;
    nodes_.push_back(std::move(node));
    if (parent != kNoNode) nodes_[parent].children.push_back(id);
    return id;
}

std::uint32_t FrameTree::AddFrame(FrameNode node) { return AddNode(std::move(node)); }

Result<std::uint32_t> FrameTree::InstantiateUnder(const FrameLibrary& library, const UiSkin* skin,
    std::string_view templateName, std::uint32_t parent) {
    const auto found = library.FindTemplate(templateName);
    if (!found) {
        return Result<std::uint32_t>::Failure(kSubsystem, "GAMEUI_TEMPLATE_MISSING",
            "找不到界面定义 " + std::string(templateName));
    }
    BuildContext context{library, skin, {}, {}, false};
    const auto rootId = static_cast<std::uint32_t>(nodes_.size());
    context.templateStack.push_back(Text::LowerAscii(found->definition->name));
    Build(context, *found->definition, library.Files()[found->fileIndex]->file.path, parent, {}, 0);
    Result<std::uint32_t> result;
    result.diagnostics = std::move(context.diagnostics);
    // 节点已达上限时 Build 已记录诊断，不返回编号。
    if (rootId < nodes_.size()) result.value = rootId;
    return result;
}

Result<std::uint32_t> FrameTree::Instantiate(const FrameLibrary& library, const UiSkin* skin,
    std::string_view templateName, const std::optional<RootPlacement>& placement) {
    auto built = InstantiateUnder(library, skin, templateName, kScreenNode);
    if (!built.value) return built;
    const std::uint32_t rootId = *built.value;
    const auto known = placement ? std::nullopt : KnownRootPlacement(templateName);
    const auto& effective = placement ? placement : known;
    if (effective) {
        FrameNode& root = nodes_[rootId];
        root.anchors.clear();
        if (effective->fillScreen) {
            root.anchors.push_back(FrameAnchor{FramePoint::TopLeft, {}, FramePoint::TopLeft, 0.0f, 0.0f});
            root.anchors.push_back(FrameAnchor{FramePoint::BottomRight, {}, FramePoint::BottomRight, 0.0f, 0.0f});
        }
        for (const auto& anchor : effective->anchors) UpsertAnchor(root.anchors, anchor);
        if (effective->width) root.width = effective->width;
        if (effective->height) root.height = effective->height;
        root.codePlaced = known.has_value();
    } else if (nodes_[rootId].anchors.empty()) {
        built.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Info, "GAMEUI_ROOT_PLACEMENT_UNKNOWN",
            std::string(templateName) + " 的位置由游戏代码决定，尚未取得，暂不显示"));
    }
    return built;
}

void FrameTree::Build(BuildContext& context, const FdfDefinition& definition, const std::string& file,
    std::uint32_t parent, std::string_view layer, int depth) {
    if (nodes_.size() >= kMaxNodes || depth > kMaxDepth) {
        if (!context.limitReported) {
            context.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error, "GAMEUI_NODE_LIMIT",
                file + " 第 " + std::to_string(definition.line) + " 行：界面节点超过 " + std::to_string(kMaxNodes) +
                    " 个或嵌套超过 " + std::to_string(kMaxDepth) + " 层，已停止展开"));
            context.limitReported = true;
        }
        return;
    }
    FrameNode node;
    node.parent = parent;
    node.kind = definition.kind;
    node.frameType = definition.frameType;
    node.name = definition.name;
    node.templateName = definition.inherits;
    node.layer = std::string(layer);
    node.sourceFile = file;
    node.sourceLine = definition.line;

    if (definition.kind == DefinitionKind::Layer) {
        node.decorate = nodes_[parent].decorate;
        const std::uint32_t id = AddNode(std::move(node));
        for (const auto& child : definition.children) Build(context, child, file, id, definition.frameType, depth + 1);
        return;
    }

    std::vector<ChildRef> children;
    AppendDefinition(context.library, context.templateStack, context.diagnostics, definition, file, node.properties,
        children, true);
    node.decorate = nodes_[parent].decorate ||
        std::any_of(node.properties.begin(), node.properties.end(),
            [](const FdfProperty& property) { return PropertyIs(property, "decoratefilenames"); });

    const bool hasPoints = std::any_of(node.properties.begin(), node.properties.end(),
        [](const FdfProperty& property) { return PropertyIs(property, "setpoint") || PropertyIs(property, "anchor"); });
    for (const auto& property : node.properties) {
        if (PropertyIs(property, "width")) {
            node.width = PropertyNumber(property, 0);
        } else if (PropertyIs(property, "height")) {
            node.height = PropertyNumber(property, 0);
        } else if (PropertyIs(property, "anchor") && property.values.size() >= 1) {
            if (const auto point = ParseFramePoint(property.values[0].text)) {
                UpsertAnchor(node.anchors, FrameAnchor{*point, {}, *point, PropertyNumber(property, 1).value_or(0.0f),
                                               PropertyNumber(property, 2).value_or(0.0f)});
            }
        } else if (PropertyIs(property, "setpoint") && property.values.size() >= 3) {
            const auto point = ParseFramePoint(property.values[0].text);
            const auto relativePoint = ParseFramePoint(property.values[2].text);
            if (point && relativePoint) {
                UpsertAnchor(node.anchors, FrameAnchor{*point, property.values[1].text, *relativePoint,
                                               PropertyNumber(property, 3).value_or(0.0f), PropertyNumber(property, 4).value_or(0.0f)});
            } else {
                context.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_SETPOINT_INVALID",
                    file + " 第 " + std::to_string(property.line) + " 行：SetPoint 的定位点无法识别"));
            }
        } else if (PropertyIs(property, "setallpoints") && !hasPoints) {
            // 游戏只在定义里没有 SetPoint/Anchor 时才执行 SetAllPoints（如多面板标题栏两者都写，以 SetPoint 为准）。
            const std::string target = property.values.empty() ? std::string() : property.values[0].text;
            UpsertAnchor(node.anchors, FrameAnchor{FramePoint::TopLeft, target, FramePoint::TopLeft, 0.0f, 0.0f});
            UpsertAnchor(node.anchors, FrameAnchor{FramePoint::BottomRight, target, FramePoint::BottomRight, 0.0f, 0.0f});
        }
    }
    ResolveResources(context, node);
    const std::uint32_t id = AddNode(std::move(node));
    // WITHCHILDREN 带来的子定义记录模板所在文件。
    for (const auto& child : children) Build(context, *child.definition, child.file, id, {}, depth + 1);
    ApplyControlFill(id);
}

void FrameTree::ApplyControlFill(std::uint32_t id) {
    // 这些属性指向的直接子框架由控件铺满自身（游戏行为，fdf 中不写锚点）。
    static constexpr std::array<std::string_view, 10> kFillKeys{"controlbackdrop", "controlpushedbackdrop",
        "controldisabledbackdrop", "controldisabledpushedbackdrop", "controlmouseoverhighlight", "controlfocushighlight",
        "buttontext", "dialogbackdrop", "checkboxcheckhighlight", "checkboxdisabledcheckhighlight"};
    for (const auto& property : nodes_[id].properties) {
        if (property.values.empty()) continue;
        const std::string key = Text::LowerAscii(property.key);
        if (std::find(kFillKeys.begin(), kFillKeys.end(), key) == kFillKeys.end()) continue;
        const std::string target = Text::LowerAscii(property.values[0].text);
        for (const std::uint32_t childId : nodes_[id].children) {
            FrameNode& child = nodes_[childId];
            if (child.kind != DefinitionKind::Frame || !child.anchors.empty() || Text::LowerAscii(child.name) != target) continue;
            child.anchors.push_back(FrameAnchor{FramePoint::TopLeft, {}, FramePoint::TopLeft, 0.0f, 0.0f});
            child.anchors.push_back(FrameAnchor{FramePoint::BottomRight, {}, FramePoint::BottomRight, 0.0f, 0.0f});
            child.autoFillBy = property.key;
            break;
        }
    }
}

void FrameTree::ResolveResources(BuildContext& context, FrameNode& node) {
    const auto resolveTexture = [&](const std::string& key, bool decorate) {
        TextureInfo texture;
        texture.key = key;
        texture.path = key;
        texture.source = "literal";
        if (decorate && context.skin != nullptr) {
            if (const auto found = context.skin->Lookup(key, options_.race, options_.expansion)) {
                texture.path = found->value;
                texture.source = found->source;
            }
        }
        texture.path = EnsureTextureExtension(texture.path);
        return texture;
    };
    const auto resolveText = [&](const std::string& key) {
        TextInfo text;
        text.key = key;
        text.text = key;
        text.source = "literal";
        if (context.skin != nullptr) {
            if (const auto found = context.skin->MapFrameString(key)) {
                text.text = *found;
                text.source = "map:framedef";
                return text;
            }
        }
        if (const auto found = context.library.FindString(key)) {
            text.text = *found;
            text.source = "fdf";
        }
        return text;
    };
    const auto applyFont = [&](TextInfo& text, const std::vector<FdfProperty>& properties) {
        for (const auto& property : properties) {
            if ((PropertyIs(property, "font") || PropertyIs(property, "framefont")) && !property.values.empty()) {
                text.fontKey = property.values[0].text;
                text.fontPath = text.fontKey;
                if (context.skin != nullptr) {
                    if (const auto found = context.skin->Lookup(text.fontKey, options_.race, options_.expansion)) {
                        text.fontPath = found->value;
                    }
                }
                text.fontHeight = PropertyNumber(property, 1).value_or(text.fontHeight);
            } else if (PropertyIs(property, "fontcolor")) {
                for (std::size_t i = 0; i < 4 && i < property.values.size(); ++i) {
                    if (const auto value = PropertyNumber(property, i)) text.color[i] = *value;
                }
            } else if (PropertyIs(property, "fontshadowcolor")) {
                for (std::size_t i = 0; i < 4 && i < property.values.size(); ++i) {
                    if (const auto value = PropertyNumber(property, i)) text.shadowColor[i] = *value;
                }
            } else if (PropertyIs(property, "fontshadowoffset")) {
                for (std::size_t i = 0; i < 2 && i < property.values.size(); ++i) {
                    if (const auto value = PropertyNumber(property, i)) text.shadowOffset[i] = *value;
                }
            } else if (PropertyIs(property, "fontjustificationh") && !property.values.empty()) {
                text.justifyH = property.values[0].text;
            } else if (PropertyIs(property, "fontjustificationv") && !property.values.empty()) {
                text.justifyV = property.values[0].text;
            }
        }
    };
    const auto applyTextureProperties = [&](TextureInfo& texture, const std::vector<FdfProperty>& properties) {
        if (const auto* coords = FindProperty(properties, "texcoord")) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (const auto value = PropertyNumber(*coords, i)) texture.texCoord[i] = *value;
            }
        }
        if (const auto* alpha = FindProperty(properties, "alphamode"); alpha && !alpha->values.empty()) {
            texture.alphaMode = alpha->values[0].text;
        }
    };
    // 引用另一个顶层 Texture/String 模板的属性（SIMPLEBUTTON 的 NormalTexture/NormalText）。
    const auto templateProperties = [&](const std::string& name) {
        std::vector<FdfProperty> properties;
        if (const auto found = context.library.FindTemplate(name)) {
            std::vector<ChildRef> ignored;
            AppendDefinition(context.library, context.templateStack, context.diagnostics, *found->definition,
                context.library.Files()[found->fileIndex]->file.path, properties, ignored, false);
        } else {
            context.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_TEMPLATE_MISSING",
                node.name + "：找不到引用的模板 " + name));
        }
        return properties;
    };

    const std::string type = Text::LowerAscii(node.frameType);
    if (node.kind == DefinitionKind::Texture) {
        if (const auto* file = FindProperty(node.properties, "file"); file && !file->values.empty()) {
            node.texture = resolveTexture(file->values[0].text, node.decorate);
            applyTextureProperties(*node.texture, node.properties);
        }
        return;
    }
    if (IsTextNode(node)) {
        TextInfo text;
        if (const auto* value = FindProperty(node.properties, "text"); value && !value->values.empty()) {
            text = resolveText(value->values[0].text);
        }
        applyFont(text, node.properties);
        node.text = std::move(text);
        return;
    }
    if (const auto* background = FindProperty(node.properties, "backdropbackground");
        background && !background->values.empty()) {
        node.texture = resolveTexture(background->values[0].text, node.decorate);
    }
    if (const auto* edge = FindProperty(node.properties, "backdropedgefile"); edge && !edge->values.empty()) {
        node.edge = resolveTexture(edge->values[0].text, node.decorate);
    }
    if (type == "simplebutton") {
        if (const auto* normal = FindProperty(node.properties, "normaltexture"); normal && !normal->values.empty()) {
            const auto properties = templateProperties(normal->values[0].text);
            if (const auto* file = FindProperty(properties, "file"); file && !file->values.empty()) {
                node.texture = resolveTexture(file->values[0].text, node.decorate);
                applyTextureProperties(*node.texture, properties);
            }
        }
        if (const auto* normalText = FindProperty(node.properties, "normaltext"); normalText && normalText->values.size() >= 2) {
            TextInfo text = resolveText(normalText->values[1].text);
            applyFont(text, templateProperties(normalText->values[0].text));
            node.text = std::move(text);
        }
    } else if (type == "simplestatusbar") {
        if (const auto* bar = FindProperty(node.properties, "bartexture"); bar && !bar->values.empty()) {
            node.texture = resolveTexture(bar->values[0].text, node.decorate);
        }
    } else if (type == "highlight") {
        if (const auto* file = FindProperty(node.properties, "highlightalphafile"); file && !file->values.empty()) {
            node.texture = resolveTexture(file->values[0].text, node.decorate);
            if (const auto* mode = FindProperty(node.properties, "highlightalphamode"); mode && !mode->values.empty()) {
                node.texture->alphaMode = mode->values[0].text;
            }
        }
    }
}

std::uint32_t FrameTree::LayoutParent(std::uint32_t id) const {
    std::uint32_t parent = nodes_[id].parent;
    while (parent != kNoNode && parent != kScreenNode && nodes_[parent].kind == DefinitionKind::Layer) {
        parent = nodes_[parent].parent;
    }
    return parent == kNoNode ? kScreenNode : parent;
}

std::optional<std::uint32_t> FrameTree::FindRelative(std::uint32_t from, std::string_view name) const {
    const std::string key = Text::LowerAscii(name);
    // 先在同一棵根框架内找，再全局找；与游戏按名字查已创建框架的结果一致（同名时近者优先）。
    std::uint32_t root = from;
    while (root != kNoNode && nodes_[root].parent != kScreenNode && nodes_[root].parent != kNoNode) root = nodes_[root].parent;
    std::vector<std::uint32_t> stack{root};
    while (!stack.empty()) {
        const std::uint32_t current = stack.back();
        stack.pop_back();
        if (current != from && Text::LowerAscii(nodes_[current].name) == key) return current;
        for (auto it = nodes_[current].children.rbegin(); it != nodes_[current].children.rend(); ++it) stack.push_back(*it);
    }
    // 代码区域在游戏里没有名字，fdf 按名字引用时找不到它们。
    const bool fromCode = !nodes_[from].codeLabel.empty();
    for (const auto& node : nodes_) {
        if (node.id != from && (fromCode || node.codeLabel.empty()) && Text::LowerAscii(node.name) == key) return node.id;
    }
    return std::nullopt;
}

std::uint32_t FrameTree::AddGameCodeAreas() {
    for (const std::uint32_t child : nodes_[kScreenNode].children) {
        if (!nodes_[child].codeLabel.empty() && nodes_[child].name == kGameCodeRoot) return child;
    }
    FrameNode root;
    root.parent = kScreenNode;
    root.frameType = "CGameUI";
    root.name = std::string(kGameCodeRoot);
    root.codeLabel = "游戏界面";
    root.codePlaced = true;
    root.anchors.push_back(FrameAnchor{FramePoint::TopLeft, {}, FramePoint::TopLeft, 0.0f, 0.0f});
    root.anchors.push_back(FrameAnchor{FramePoint::BottomRight, {}, FramePoint::BottomRight, 0.0f, 0.0f});
    const std::uint32_t rootId = AddNode(std::move(root));
    for (const auto& area : GameCodeAreas()) {
        FrameNode node;
        node.parent = rootId;
        node.frameType = std::string(area.className);
        node.name = std::string(area.id);
        node.codeLabel = std::string(area.label);
        node.codePlaced = true;
        node.anchors = area.anchors;
        node.width = area.width;
        node.height = area.height;
        AddNode(std::move(node));
    }
    return rootId;
}

std::optional<std::uint32_t> FrameTree::FindCodeArea(std::string_view id) const {
    for (const auto& node : nodes_) {
        if (!node.codeLabel.empty() && node.name == id) return node.id;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> FrameTree::FindByName(std::string_view name) const {
    const std::string key = Text::LowerAscii(name);
    for (const auto& node : nodes_) {
        if (node.id != kScreenNode && Text::LowerAscii(node.name) == key) return node.id;
    }
    return std::nullopt;
}

std::vector<Diagnostic> FrameTree::Layout() {
    std::vector<Diagnostic> diagnostics;
    std::vector<std::uint8_t> state(nodes_.size(), 0);
    state[kScreenNode] = 2;
    for (std::uint32_t id = 1; id < nodes_.size(); ++id) Solve(id, state, diagnostics);
    return diagnostics;
}

void FrameTree::Solve(std::uint32_t id, std::vector<std::uint8_t>& state, std::vector<Diagnostic>& diagnostics) {
    if (state[id] == 2) return;
    if (state[id] == 1) {
        nodes_[id].layout = LayoutState::Cycle;
        diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_LAYOUT_CYCLE",
            "框架 " + nodes_[id].name + " 的锚点互相依赖"));
        return;
    }
    state[id] = 1;
    const std::uint32_t parent = LayoutParent(id);
    if (nodes_[id].kind == DefinitionKind::Layer) {
        Solve(parent, state, diagnostics);
        nodes_[id].rect = nodes_[parent].rect;
        nodes_[id].layout = nodes_[parent].layout;
        state[id] = 2;
        return;
    }
    std::vector<FrameAnchor> anchors = nodes_[id].anchors;
    if (anchors.empty() && (nodes_[id].kind == DefinitionKind::Texture || nodes_[id].kind == DefinitionKind::String)) {
        // SIMPLEFRAME 中没有锚点的贴图/文字铺满所在框架。
        anchors.push_back(FrameAnchor{FramePoint::TopLeft, {}, FramePoint::TopLeft, 0.0f, 0.0f});
        anchors.push_back(FrameAnchor{FramePoint::BottomRight, {}, FramePoint::BottomRight, 0.0f, 0.0f});
    }
    std::optional<float> left, right, centerX, top, bottom, centerY;
    std::optional<FramePoint> firstPoint;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        FrameAnchor& anchor = anchors[i];
        std::optional<std::uint32_t> target;
        if (anchor.targetNode != kNoNode && anchor.targetNode < nodes_.size()) {
            target = anchor.targetNode;
        } else if (anchor.relativeName.empty()) {
            target = parent;
        } else {
            target = FindRelative(id, anchor.relativeName);
            if (!target) {
                diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Warning, "GAMEUI_ANCHOR_TARGET_MISSING",
                    "框架 " + nodes_[id].name + " 的锚点目标 " + anchor.relativeName + " 不存在"));
                continue;
            }
        }
        Solve(*target, state, diagnostics);
        const FrameNode& relative = nodes_[*target];
        if (relative.layout == LayoutState::Unplaced || relative.layout == LayoutState::Cycle) continue;
        anchor.relativeNode = *target;
        if (i < nodes_[id].anchors.size()) nodes_[id].anchors[i].relativeNode = *target;
        const float x = FramePointX(relative.rect, anchor.relativePoint) + anchor.x;
        const float y = FramePointY(relative.rect, anchor.relativePoint) + anchor.y;
        if (!firstPoint) firstPoint = anchor.point;
        switch (PointColumn(anchor.point)) {
        case 0: left = x; break;
        case 1: centerX = x; break;
        default: right = x; break;
        }
        switch (PointRow(anchor.point)) {
        case 0: top = y; break;
        case 1: centerY = y; break;
        default: bottom = y; break;
        }
    }
    FrameNode& node = nodes_[id];
    const AxisResult horizontal = SolveAxis(left, right, centerX, node.width);
    const AxisResult vertical = SolveAxis(bottom, top, centerY, node.height);
    if (!horizontal.constrained || !vertical.constrained) {
        node.layout = LayoutState::Unplaced;
        node.rect = {};
        state[id] = 2;
        return;
    }
    node.rect = FrameRect{horizontal.low, vertical.low, horizontal.high, vertical.high};
    node.layout = (horizontal.degenerate || vertical.degenerate) && IsTextNode(node) ? LayoutState::SizeFromContent
                                                                                       : LayoutState::Placed;
    if (node.layout == LayoutState::SizeFromContent && firstPoint) node.contentAnchor = *firstPoint;
    state[id] = 2;
}

} // namespace War3Map::GameUi
