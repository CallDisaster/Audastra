#pragma once

#include "War3MapCore/Render/SurfaceMaterialTable.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {
// 地图材质表（WarVK\materials.ini）的原地编辑，界面、MCP 与保存前检查共用。
// 只改动涉及的行：注释、未知键、坏行、空行、BOM 与每行原来的换行符都保持原样，新加的行用原文件的换行风格
// （新表为 CRLF）。读法与 Render::ParseSurfaceMaterialTable 相同（同一份键表与行读法）：同名节合并，
// 坏值不覆盖之前的好值，超过 256 种类型之后的节被忽略。
inline constexpr std::string_view kSurfaceMaterialSubsystem = "Services.SurfaceMaterials";

struct SurfaceMaterialSection {
    std::string rawcode;
    std::size_t headers = 0; // 同名节头个数；大于 1 时读表已合并
    std::size_t line = 0;    // 第一个节头的行号（从 1 起）
    // 与 Render::kSurfaceMaterialKeys 对齐。values：生效的原文值（坏值不覆盖之前的好值；没写或只写了坏值时为空）。
    std::array<std::optional<std::string>, Render::kSurfaceMaterialKeys.size()> values;
    // 最后一次写的值无效：读表保留之前的值或默认值。
    std::array<bool, Render::kSurfaceMaterialKeys.size()> invalid{};
    std::vector<std::string> unknownKeys;
};

// 检查并规整一个键的值：图片路径不能为空、不能有首尾空白、控制字符或“;”“#”（会被读成注释）；
// 数值须在键的范围内，按最短十进制写出；选项须是两个取值之一。
[[nodiscard]] Result<std::string> NormalizeSurfaceMaterialValue(std::string_view key, std::string_view value);

class SurfaceMaterialTableDocument {
public:
    // 不会失败：坏行按读表规则给出警告并原样保留。
    [[nodiscard]] static SurfaceMaterialTableDocument Parse(std::string_view text);
    [[nodiscard]] std::string Text() const;
    // 等同 Render::ParseSurfaceMaterialTable(Text())。
    [[nodiscard]] Render::SurfaceMaterialTable Table() const;
    // 读表接受的类型，按第一次出现的顺序。
    [[nodiscard]] std::vector<SurfaceMaterialSection> Sections() const;
    [[nodiscard]] std::optional<SurfaceMaterialSection> Section(std::string_view rawcode) const;

    // 改一个键。已写过时只改最后一次出现的那一行的值（行内注释保留）；没写过时加在本类型最后一节的最后一个
    // 已知键之后（没有已知键时紧跟节头）；类型不存在时在表尾新加一节。值非法、类型已满 256 种时拒绝，原文不变。
    [[nodiscard]] Status Set(std::string_view rawcode, std::string_view key, std::string_view value);
    // 删去这个键在本类型各节里的所有行；行内有注释时注释留作单独一行。没写过时不变。
    [[nodiscard]] Status Unset(std::string_view rawcode, std::string_view key);
    // 删去本类型的每一节：从节头到下一节头之前（紧挨下一节头的注释行属于下一节，保留）。类型不存在时报错。
    [[nodiscard]] Status RemoveSection(std::string_view rawcode);

private:
    struct Line {
        std::string text; // 不含行尾换行
        std::string end;  // "\r\n"、"\n" 或空（最后一行没有换行）
    };
    struct ScannedLine {
        Render::SurfaceMaterialLine read;
        int section = -1; // 所在的已接受类型序号；节外、坏节名或超出 256 种之后的节为 -1
    };
    struct Scan {
        std::vector<std::string> order; // 已接受类型，按第一次出现
        std::vector<ScannedLine> lines;
        [[nodiscard]] int Find(std::string_view rawcode) const;
    };
    [[nodiscard]] Scan Read() const;
    void Insert(std::size_t at, std::string text);
    void Erase(const std::vector<bool>& remove);

    std::string bom_;
    std::string eol_ = "\r\n";
    std::vector<Line> lines_;
};
}
