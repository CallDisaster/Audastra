#pragma once

#include "War3MapCore/Jass/JassSymbols.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "JassCheckBudget.h"

#include <atomic>
#include <algorithm>
#include <span>
#include <string_view>

namespace War3Map::Jass::detail {

// 索引的显示预算独立于语法/语义预算；耗尽只停止索引收集，不改变保存校验结论。
class SymbolCollectionBudget {
public:
    SymbolCollectionBudget(std::span<const JassSource> sources, std::size_t definitions,
        std::size_t references, std::size_t bytes)
        : sources_(sources), maxDefinitions_((std::min)(definitions, std::size_t{65536})),
          maxReferences_((std::min)(references, std::size_t{524288})),
          maxBytes_((std::min)(bytes, std::size_t{32 * 1024 * 1024})) {}
    [[nodiscard]] bool ClaimDefinition(std::size_t bytes) {
        if (definitions_.fetch_add(1) >= maxDefinitions_) { partial_ = true; return false; }
        return ClaimBytes(bytes);
    }
    [[nodiscard]] bool ClaimReference() {
        if (references_.fetch_add(1) >= maxReferences_) { partial_ = true; return false; }
        return ClaimBytes(sizeof(SymbolReference));
    }
    [[nodiscard]] bool ClaimMetadata(std::size_t bytes) { return ClaimBytes(bytes); }
    [[nodiscard]] std::optional<SymbolPosition> Position(std::uint32_t file, std::string_view name) {
        if (file >= sources_.size() || name.empty() || name.size() > 1024) { partial_ = true; return {}; }
        const auto& source = sources_[file].text;
        const auto begin = reinterpret_cast<std::uintptr_t>(source.data());
        const auto at = reinterpret_cast<std::uintptr_t>(name.data());
        if (at < begin || at - begin > source.size() || name.size() > source.size() - (at - begin)) {
            partial_ = true; return {};
        }
        return SymbolPosition{{file, static_cast<std::uint32_t>(at - begin)}, static_cast<std::uint32_t>(name.size())};
    }
    void MarkPartial() noexcept { partial_ = true; }
    [[nodiscard]] bool Partial() const noexcept { return partial_; }
    [[nodiscard]] std::span<const JassSource> Sources() const noexcept { return sources_; }
private:
    bool ClaimBytes(std::size_t size) {
        const auto previous = bytes_.fetch_add(size);
        if (previous > maxBytes_ || size > maxBytes_ - previous) { partial_ = true; return false; }
        return true;
    }
    std::span<const JassSource> sources_;
    std::size_t maxDefinitions_, maxReferences_, maxBytes_;
    std::atomic<std::size_t> definitions_{0}, references_{0}, bytes_{0};
    std::atomic<bool> partial_{false};
};

// 每个函数独占一个sink；顶层与全局初值sink只由顺序声明阶段写入。
struct SymbolCollector {
    SymbolCollectionBudget* shared = nullptr;
    std::uint32_t file = 0;
    std::optional<SymbolKey> scope;
    std::vector<SymbolDefinition> definitions;
    std::vector<SymbolReference> references;
    std::vector<SymbolScope> scopes;

    [[nodiscard]] std::optional<SymbolKey> Define(std::string_view name, std::string_view type,
        SymbolKind kind, bool array = false, bool constant = false) {
        if (!shared) return {};
        const auto position = shared->Position(file, name);
        if (!position || type.size() > 1024) { shared->MarkPartial(); return {}; }
        if (!shared->ClaimDefinition(sizeof(SymbolDefinition) + name.size() + type.size())) return {};
        definitions.push_back({*position, kind, std::string(name), std::string(type), scope, array, constant});
        return position->key;
    }
    void Reference(std::string_view name, std::optional<SymbolKey> target, SymbolUse use) {
        if (!shared) return;
        const auto position = shared->Position(file, name);
        if (!position || !shared->ClaimReference()) return;
        references.push_back({*position, use, target});
        if (!target) shared->MarkPartial();
    }
    void Parameter(SymbolKey function, std::string_view type, std::string_view name) {
        if (!shared || definitions.empty() || definitions.back().position.key != function) return;
        if (type.size() > 1024 || name.size() > 1024) { shared->MarkPartial(); return; }
        if (!shared->ClaimMetadata(sizeof(SymbolParameter) + type.size() + name.size())) return;
        definitions.back().parameters.push_back({std::string(type), std::string(name)});
    }
    void Scope(SymbolKey function, std::uint32_t firstLine, std::uint32_t lastLine) {
        if (shared && shared->ClaimMetadata(sizeof(SymbolScope))) scopes.push_back({function, firstLine, lastLine});
    }
};

// 主线程稳定合并后按来源顺序一次扫描行边界，不为每条引用从头重扫源文。
void FinishSymbolIndex(SymbolIndex& output, SymbolCollectionBudget& shared,
    std::vector<SymbolCollector>& collectors, CheckBudget& budget, bool checkComplete);

} // namespace War3Map::Jass::detail
