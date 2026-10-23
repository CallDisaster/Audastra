#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 游戏界面定义（fdf/toc）的解析结果。只做语法层：不查皮肤、不展开继承、不算位置。
// 本模块不依赖 Qt，编辑器显示、预览和将来的游戏内运行时共用同一份模型。
namespace War3Map::GameUi {

// fdf 中的定义块种类。Frame 是框架；Texture/String 是 SIMPLEFRAME 里的贴图与文字；
// Layer 是 SIMPLEFRAME 的绘制层，只用于给其中的 Texture/String 分层。
enum class DefinitionKind : std::uint8_t {
    Frame,
    Texture,
    String,
    Layer,
};

// 属性参数。quoted 表示原文带引号；路径中的反斜杠按字面保留，fdf 没有转义。
struct FdfValue {
    std::string text;
    bool quoted = false;

    [[nodiscard]] bool operator==(const FdfValue&) const = default;
};

struct FdfProperty {
    std::string key;
    std::vector<FdfValue> values;
    std::uint32_t line = 0;
};

struct FdfDefinition {
    DefinitionKind kind = DefinitionKind::Frame;
    // Frame 为框架类型（如 BACKDROP、SIMPLEFRAME，保持原文大小写）；Layer 为层名（如 ARTWORK）。
    std::string frameType;
    // Texture/String 可以匿名；Frame 必须有名字。
    std::string name;
    std::string inherits;
    bool inheritsWithChildren = false;
    std::vector<FdfProperty> properties;
    std::vector<FdfDefinition> children;
    std::uint32_t line = 0;
};

struct FdfFile {
    std::string path;
    // IncludeFile 指令按出现顺序记录，由加载方决定是否读取。
    std::vector<std::string> includes;
    std::vector<FdfDefinition> definitions;
    // StringList 中的键值，按出现顺序；同键以后出现者为准由加载方处理。
    std::vector<std::pair<std::string, std::string>> strings;
};

// 锚点位置，顺序与游戏一致（0..8）。
enum class FramePoint : std::uint8_t {
    TopLeft,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
};

[[nodiscard]] std::optional<FramePoint> ParseFramePoint(std::string_view text);
[[nodiscard]] std::string_view FramePointName(FramePoint point) noexcept;

// 解析一个 fdf 文件。可恢复的问题记为 Warning；结构错误（如括号不配对）记为 Error，
// 但仍返回已解析的部分，供界面显示和定位。诊断消息带行号，origin 记录文件与字节偏移。
// 属性按行读取；同一行里逗号后出现的属性名（非数字、非锚点名、非 JUSTIFY 常量）开始下一个属性。
[[nodiscard]] Result<FdfFile> ParseFdf(std::string_view text, std::string path);

// 解析 toc：每行一个文件路径，忽略空行与 // 注释。
[[nodiscard]] std::vector<std::string> ParseToc(std::string_view text);

} // namespace War3Map::GameUi
