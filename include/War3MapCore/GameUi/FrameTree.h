#pragma once

#include "War3MapCore/GameUi/FrameDefinition.h"
#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/UiSkin.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 界面实例树：把模板展开成具体节点（合并继承、查皮肤、查文字），再按锚点计算每个节点在屏幕上的矩形。
// 屏幕坐标与游戏一致：原点在左下，宽 0.8、高 0.6（4:3）。
namespace War3Map::GameUi {

struct FrameRect {
    float left = 0.0f;
    float bottom = 0.0f;
    float right = 0.0f;
    float top = 0.0f;

    [[nodiscard]] float Width() const noexcept { return right - left; }
    [[nodiscard]] float Height() const noexcept { return top - bottom; }
};

inline constexpr std::uint32_t kScreenNode = 0;
inline constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;

struct FrameAnchor {
    FramePoint point = FramePoint::TopLeft;
    // 为空表示相对布局父节点（fdf 的 Anchor、无参数 SetAllPoints）；否则按名字查找。
    std::string relativeName;
    FramePoint relativePoint = FramePoint::TopLeft;
    float x = 0.0f;
    float y = 0.0f;
    // 布局时解析出的目标节点。
    std::uint32_t relativeNode = kNoNode;
};

enum class LayoutState : std::uint8_t {
    Placed,           // 矩形确定
    SizeFromContent,  // 只有定位点，宽高要由文字内容决定（显示层测量）
    Unplaced,         // 没有可用锚点，游戏中不会显示
    Cycle,            // 锚点互相依赖
};

struct TextureInfo {
    std::string key;     // fdf 原文（DecorateFileNames 时为皮肤键）
    std::string path;    // 实际路径；没有扩展名时补 .blp
    std::string source;  // "game:human"、"map:customskin"、"literal"
    std::array<float, 4> texCoord{0.0f, 1.0f, 0.0f, 1.0f};  // 左、右、上、下
    std::string alphaMode;
};

struct TextInfo {
    std::string key;   // fdf 中的文字键
    std::string text;  // 查表后的显示文字（含 |c 颜色码，原样保留）
    std::string source;  // "map:framedef"、"fdf"、"literal"
    std::string fontKey;
    std::string fontPath;
    float fontHeight = 0.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::string justifyH;
    std::string justifyV;
};

struct FrameNode {
    std::uint32_t id = 0;
    std::uint32_t parent = kNoNode;
    std::vector<std::uint32_t> children;
    DefinitionKind kind = DefinitionKind::Frame;
    std::string frameType;     // Frame 类型；Layer 节点为层名；屏幕节点为 SCREEN
    std::string name;
    std::string templateName;  // INHERITS 的模板
    std::string layer;         // Texture/String 所在的 Layer
    std::string sourceFile;
    std::uint32_t sourceLine = 0;
    std::vector<FdfProperty> properties;  // 合并后：模板在前、自身在后
    bool decorate = false;
    std::optional<float> width;
    std::optional<float> height;
    std::vector<FrameAnchor> anchors;
    // 自身没有锚点、由父控件属性（如 ControlBackdrop、ButtonText）让游戏自动铺满时记录该属性名。
    std::string autoFillBy;
    std::optional<TextureInfo> texture;  // Texture 的 File、BACKDROP 背景、HIGHLIGHT 贴图、SIMPLEBUTTON 常态贴图、状态条贴图
    std::optional<TextureInfo> edge;     // BACKDROP 边框
    std::optional<TextInfo> text;        // String/TEXT 文字、SIMPLEBUTTON 常态文字
    FrameRect rect;
    LayoutState layout = LayoutState::Unplaced;
    FramePoint contentAnchor = FramePoint::Center;  // SizeFromContent 时文字对齐的定位点
};

// String 与 TEXT 框架：没有尺寸时由文字内容决定大小。
[[nodiscard]] bool IsTextNode(const FrameNode& node);

struct FrameTreeOptions {
    std::string race = "Human";
    bool expansion = true;
    float screenWidth = 0.8f;
    float screenHeight = 0.6f;
};

// 自带界面根节点在游戏代码里的摆放（fdf 中没有）；锚点一律相对屏幕。
struct RootPlacement {
    bool fillScreen = false;
    std::vector<FrameAnchor> anchors;
};

// 已确认的自带界面根摆放；未列出的根框架由游戏代码定位，需游戏内取得后补入。
[[nodiscard]] std::optional<RootPlacement> KnownRootPlacement(std::string_view templateName);

class FrameTree {
public:
    explicit FrameTree(FrameTreeOptions options = {});

    // 按顶层模板名实例化一个根框架，挂在屏幕节点下；返回根节点编号。
    // placement 为空时先查 KnownRootPlacement，再用定义自身的锚点（相对屏幕）；都没有时记 Info 诊断。
    [[nodiscard]] Result<std::uint32_t> Instantiate(const FrameLibrary& library, const UiSkin* skin,
        std::string_view templateName, const std::optional<RootPlacement>& placement = std::nullopt);

    // 计算全部节点的矩形；返回本次布局的诊断。
    [[nodiscard]] std::vector<Diagnostic> Layout();

    [[nodiscard]] const std::vector<FrameNode>& Nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::optional<std::uint32_t> FindByName(std::string_view name) const;
    [[nodiscard]] const FrameTreeOptions& Options() const noexcept { return options_; }

private:
    struct BuildContext;

    std::uint32_t AddNode(FrameNode node);
    void Build(BuildContext& context, const FdfDefinition& definition, const std::string& file, std::uint32_t parent,
        std::string_view layer, int depth);
    void ResolveResources(BuildContext& context, FrameNode& node);
    void ApplyControlFill(std::uint32_t id);
    [[nodiscard]] std::uint32_t LayoutParent(std::uint32_t id) const;
    [[nodiscard]] std::optional<std::uint32_t> FindRelative(std::uint32_t from, std::string_view name) const;
    void Solve(std::uint32_t id, std::vector<std::uint8_t>& state, std::vector<Diagnostic>& diagnostics);

    FrameTreeOptions options_;
    std::vector<FrameNode> nodes_;
};

} // namespace War3Map::GameUi
