#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// 脚本各阶段（预处理、模板、vJass）之间的行号映射：输出第 N 行来自哪个源文件的哪一行。
// 以“段”存储：从 outputLine 开始，连续的输出行对应同一文件中连续的源行。
namespace War3Map::Jass {

struct SourcePosition {
    std::uint32_t file = 0; // SourceMap::files 的下标
    std::uint32_t line = 0; // 从 1 开始
};

class SourceMap {
public:
    // 登记文件并返回下标；同名文件复用同一下标。
    std::uint32_t AddFile(const std::string& name);
    // 声明输出第 outputLine 行（从 1 开始）来自 position；与现有段的推算一致时不新增段。
    void Map(std::uint32_t outputLine, SourcePosition position);

    // 输出行对应的来源；没有任何段覆盖时为空。
    [[nodiscard]] std::optional<SourcePosition> Find(std::uint32_t outputLine) const;
    // “文件:行”；没有映射时为空。
    [[nodiscard]] std::string Describe(std::uint32_t outputLine) const;

    [[nodiscard]] const std::vector<std::string>& Files() const noexcept { return files_; }
    [[nodiscard]] std::size_t SegmentCount() const noexcept { return segments_.size(); }
    [[nodiscard]] bool Empty() const noexcept { return segments_.empty(); }

    // 把本映射作为“输出 → 中间文本”（outputLines 为输出总行数），中间文本再经 inner 映射到源：得到“输出 → 源”。
    [[nodiscard]] SourceMap ComposeWith(const SourceMap& inner, std::uint32_t outputLines) const;

private:
    struct Segment {
        std::uint32_t outputLine;
        SourcePosition source;
    };
    std::vector<std::string> files_;
    std::vector<Segment> segments_; // 按 outputLine 递增
};

} // namespace War3Map::Jass
