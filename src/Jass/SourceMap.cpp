#include "War3MapCore/Jass/SourceMap.h"

#include <algorithm>

namespace War3Map::Jass {

std::uint32_t SourceMap::AddFile(const std::string& name) {
    for (std::size_t i = 0; i < files_.size(); ++i) {
        if (files_[i] == name) return static_cast<std::uint32_t>(i);
    }
    files_.push_back(name);
    return static_cast<std::uint32_t>(files_.size() - 1);
}

void SourceMap::Map(std::uint32_t outputLine, SourcePosition position) {
    if (!segments_.empty()) {
        const auto& last = segments_.back();
        if (outputLine < last.outputLine) return; // 只按输出顺序追加
        if (last.source.file == position.file && last.source.line + (outputLine - last.outputLine) == position.line) return;
        if (last.outputLine == outputLine) {
            segments_.back().source = position;
            return;
        }
    }
    segments_.push_back({outputLine, position});
}

std::optional<SourcePosition> SourceMap::Find(std::uint32_t outputLine) const {
    const auto next = std::upper_bound(segments_.begin(), segments_.end(), outputLine,
        [](std::uint32_t line, const Segment& segment) { return line < segment.outputLine; });
    if (next == segments_.begin()) return std::nullopt;
    const auto& segment = *(next - 1);
    return SourcePosition{segment.source.file, segment.source.line + (outputLine - segment.outputLine)};
}

std::string SourceMap::Describe(std::uint32_t outputLine) const {
    const auto position = Find(outputLine);
    if (!position || position->file >= files_.size()) return {};
    return files_[position->file] + ":" + std::to_string(position->line);
}

SourceMap SourceMap::ComposeWith(const SourceMap& inner, std::uint32_t outputLines) const {
    SourceMap result;
    result.files_ = inner.files_;
    // 本映射的每个段内逐行查询内层映射；段数与行数同阶，足够快。
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const auto& segment = segments_[i];
        const std::uint32_t end = i + 1 < segments_.size() ? segments_[i + 1].outputLine : std::max(outputLines + 1, segment.outputLine + 1);
        for (std::uint32_t line = segment.outputLine; line < end; ++line) {
            const auto middle = segment.source.line + (line - segment.outputLine);
            if (const auto source = inner.Find(middle)) result.Map(line, *source);
        }
    }
    return result;
}

} // namespace War3Map::Jass
