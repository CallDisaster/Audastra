#pragma once

#include "War3MapCore/GameUi/FrameTree.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

// 把已布局的界面树转成按绘制顺序排列的贴图与文字命令。坐标仍是屏幕单位（0.8×0.6，原点左下），
// 由显示层换算到像素；编辑器预览与将来的游戏内运行库共用这一份绘制规则。
namespace War3Map::GameUi {

enum class DrawBlend : std::uint8_t { Blend, Add, AlphaKey, Modulate };
enum class JustifyH : std::uint8_t { Left, Center, Right };
enum class JustifyV : std::uint8_t { Top, Middle, Bottom };

struct TexturePoint {
    float u = 0.0f;
    float v = 0.0f;
};

struct ImageDraw {
    std::string path;
    std::string source;  // 皮肤来源，见 TextureInfo::source
    FrameRect rect;
    // 目标矩形左上、右上、左下、右下四角对应的贴图坐标；可超出 0..1（此时 repeat 为真，按贴图平铺）。
    std::array<TexturePoint, 4> uv{};
    DrawBlend blend = DrawBlend::Blend;
    bool repeat = false;
};

struct TextDraw {
    std::string text;  // 原文，含 |c 颜色码与 |n
    std::string fontPath;
    float fontHeight = 0.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> shadowColor{0.0f, 0.0f, 0.0f, 0.0f};  // 透明度为 0 时不画
    std::array<float, 2> shadowOffset{0.0f, 0.0f};             // 屏幕单位，y 向上
    FrameRect rect;
    // 只有定位点时（SizeFromContent），文字以 anchor 指定的位置对齐到 rect 的同名点。
    bool sizeFromContent = false;
    FramePoint anchor = FramePoint::Center;
    JustifyH justifyH = JustifyH::Center;
    JustifyV justifyV = JustifyV::Middle;
};

// CodeArea：游戏代码自行绘制的区域（小地图、头像等），没有界面文件可画；显示层画占位框，游戏内跳过。
enum class DrawKind : std::uint8_t { Image, Text, CodeArea };

struct FrameDrawCommand {
    std::uint32_t node = kNoNode;
    DrawKind kind = DrawKind::Image;
    ImageDraw image;  // CodeArea 只用其中的 rect
    TextDraw text;
};

struct FrameDrawOptions {
    // 隐藏的节点连同其子树都不绘制。
    std::unordered_set<std::uint32_t> hidden;
    // 也画脚本或描述设为隐藏的节点（FrameNode::hidden），便于编辑时查看。
    bool drawScriptHidden = false;
};

struct FrameDrawList {
    std::vector<FrameDrawCommand> commands;
    // 只在按下、禁用、鼠标悬停、获得焦点或勾选时显示的节点；常态预览不绘制。
    std::vector<std::uint32_t> stateOnlyNodes;
};

// 绘制顺序：父框架先于子框架；SIMPLEFRAME 内的贴图与文字按层 BACKGROUND、BORDER、ARTWORK、OVERLAY
// 排列（不在层内的视为 ARTWORK），之后才是子框架。未定位的节点不产生命令，其子节点照常处理。
[[nodiscard]] FrameDrawList BuildDrawList(const FrameTree& tree, const FrameDrawOptions& options = {});

[[nodiscard]] DrawBlend ParseBlend(std::string_view alphaMode);

} // namespace War3Map::GameUi
