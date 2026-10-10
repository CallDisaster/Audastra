#include "JassSymbolCollector.h"

#include <algorithm>

namespace War3Map::Jass::detail {
void FinishSymbolIndex(SymbolIndex& output, SymbolCollectionBudget& shared,
    std::vector<SymbolCollector>& collectors, CheckBudget& budget, bool checkComplete) {
    for (const auto& source : shared.Sources()) output.sources.push_back(source.name);
    for (auto& collector : collectors) {
        budget.Poll();
        for (auto& definition : collector.definitions) output.definitions.push_back(std::move(definition));
        output.references.insert(output.references.end(), collector.references.begin(), collector.references.end());
        output.scopes.insert(output.scopes.end(), collector.scopes.begin(), collector.scopes.end());
    }
    const auto less = [](const auto& a, const auto& b) {
        return a.position.key.file != b.position.key.file ? a.position.key.file < b.position.key.file
            : a.position.key.offset < b.position.key.offset;
    };
    std::stable_sort(output.definitions.begin(), output.definitions.end(), less);
    std::stable_sort(output.references.begin(), output.references.end(), less);
    std::vector<SymbolPosition*> positions;
    positions.reserve(output.definitions.size() + output.references.size());
    for (auto& definition : output.definitions) positions.push_back(&definition.position);
    for (auto& reference : output.references) positions.push_back(&reference.position);
    std::stable_sort(positions.begin(), positions.end(), [](const auto* a, const auto* b) {
        return a->key.file != b->key.file ? a->key.file < b->key.file : a->key.offset < b->key.offset;
    });
    std::uint32_t file = UINT32_MAX, line = 1;
    std::size_t scanned = 0, lineStart = 0;
    for (auto* position : positions) {
        budget.Poll();
        if (file != position->key.file) { file = position->key.file; scanned = lineStart = 0; line = 1; }
        const auto text = shared.Sources()[file].text;
        while (scanned < position->key.offset) {
            budget.PollOccasionally(scanned);
            if (text[scanned] == '\r' || text[scanned] == '\n') {
                if (text[scanned] == '\r' && scanned + 1 < position->key.offset && text[scanned + 1] == '\n') ++scanned;
                ++line; lineStart = scanned + 1;
            }
            ++scanned;
        }
        position->line = line; position->byteColumn = static_cast<std::uint32_t>(position->key.offset - lineStart + 1);
    }
    output.completed = checkComplete && !shared.Partial();
    if (!output.completed) output.reason = "语义引用索引包含未知/无效代码、被忽略错误或已达到收集预算；以下是本次实际解析的部分记录。";
}
} // namespace War3Map::Jass::detail
