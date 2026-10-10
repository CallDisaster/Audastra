#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// WorldEditStrings 字符串表：WESTRING_* 键 → 显示文本。
//
// 世界编辑器的分类名、类型名、预设名、物编字段名大量写成 WESTRING_* 键，实际文本在
// UI\WorldEditStrings.txt / UI\WorldEditGameStrings.txt 中。YDWE 把 share/<语言>/mpq
// 当作叠加在游戏 MPQ 之上的虚拟层；本表按“先加入者优先”逐键合并多个来源，
// 因此覆盖层缺少的键仍可从游戏 MPQ 或英文原版补上（载入顺序见 WorldEditStringLoader.h）。
// 本类只做解析与查询，不读取文件，也不依赖游戏数据。
namespace War3Map::Game {

class WorldEditStrings {
public:
    // 追加一份 INI 文本（UTF-8，可带 BOM）。已存在的键保持原值：先加入的来源优先。
    // 返回新增的键数。
    std::size_t AddText(std::string_view source, std::string_view text);
    // 追加单个键值（已存在时忽略）。供调用方注入与测试。
    bool AddEntry(std::string_view key, std::string value);

    // 按键查找（不区分大小写）。
    [[nodiscard]] const std::string* Find(std::string_view key) const;
    // text 是 WESTRING_* 键时解析为文本（值本身也可能是键，按链继续，遇到循环停止）；
    // 不是键或找不到时原样返回。
    [[nodiscard]] std::string Resolve(std::string_view text) const;
    // 键能否解析为非键文本。
    [[nodiscard]] bool CanResolve(std::string_view text) const;

    [[nodiscard]] static bool IsKey(std::string_view text) noexcept;
    [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }
    // 已加入的来源名（按优先级从高到低）。
    [[nodiscard]] const std::vector<std::string>& Sources() const noexcept { return sources_; }

private:
    std::unordered_map<std::string, std::string> entries_; // 键统一为 ASCII 大写
    std::vector<std::string> sources_;
};

} // namespace War3Map::Game
