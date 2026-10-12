#include "UiLibraryCommand.h"

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/UiLibrary/UiLibrary.h"
#include "War3MapCore/IO/FileBytes.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

// ui-library-inspect：只读列出数据根中的函数库包、按 config 载入后的冲突，
// 以及（给定 WTG/WCT 时）本图用到的包与比对结果。不修改 config，不执行包内程序。
namespace {

std::vector<std::byte> ReadAll(const std::filesystem::path& path) {
    auto result = War3Map::IO::ReadFileBytes(path, std::numeric_limits<std::uint64_t>::max());
    return result.Succeeded() ? std::move(*result.value) : std::vector<std::byte>{};
}

void Print(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
}

} // namespace

int RunUiLibraryInspect(const std::filesystem::path& toolRoot,
    const std::filesystem::path* wtgPath, const std::filesystem::path* wctPath) {
    using namespace War3Map;
    auto discovered = UiLibrary::DiscoverPackages(toolRoot);
    Print(discovered.diagnostics);
    if (!discovered.value) return 1;
    const auto& packages = *discovered.value;
    for (const auto& package : packages) {
        std::cout << "package " << package.id << (package.listedInConfig ? " listed" : " unlisted")
                  << " files=" << package.fileCount << " bytes=" << package.byteCount
                  << " fingerprint=" << package.fingerprint.substr(0, 12) << '\n';
    }
    const auto global = UiLibrary::ReconcileGlobal({}, packages);
    const auto layers = UiLibrary::EffectiveLayers(global, {}, packages);
    auto catalog = Trigger::UiCatalog::LoadLayers(toolRoot, layers);
    Print(catalog.diagnostics);
    if (!catalog.Succeeded()) return 1;
    const auto conflicts = UiLibrary::FindConflicts(*catalog.value);
    std::cout << "effectiveLayers=" << layers.size() << " conflicts=" << conflicts.size() << '\n';
    for (std::size_t index = 0; index < conflicts.size() && index < 20; ++index) {
        const auto& conflict = conflicts[index];
        std::cout << "  conflict " << Trigger::ToString(conflict.kind) << ' ' << conflict.name << " active=" << conflict.activeLayer
                  << " providers=" << conflict.layers.size() << '\n';
    }
    // 回调块动作：UI 定义少末尾 code 参数，块内动作生成为独立函数传入。
    const auto& callbacks = catalog.value->CallbackActions();
    std::vector<std::string> callbackNames;
    for (const auto& [name, action] : callbacks.actions) callbackNames.push_back(name);
    std::sort(callbackNames.begin(), callbackNames.end());
    std::cout << "callbackActions=" << callbackNames.size() << (callbacks.complete ? "" : " incomplete") << '\n';
    for (const auto& name : callbackNames) {
        const auto& action = callbacks.actions.at(name);
        const auto source = action.source.u8string();
        std::cout << "  callback " << name << " codeArgument=" << action.codeArgument
                  << (action.sharesLocals ? " locals=shared" : " locals=own") << " source="
                  << std::string(source.begin(), source.end()) << ':' << action.line << '\n';
    }
    if (wtgPath == nullptr || wctPath == nullptr) return 0;
    auto parsed = Trigger::WtgWctCodec{}.Parse(ReadAll(*wtgPath), ReadAll(*wctPath), *catalog.value);
    Print(parsed.diagnostics);
    if (!parsed.Succeeded()) return 1;
    const auto usage = UiLibrary::ComputeMapUsage(*parsed.value, packages);
    for (const auto& check : UiLibrary::CheckMapUsage(usage, global, packages)) {
        const auto entry = std::find_if(usage.packages.begin(), usage.packages.end(),
            [&check](const auto& value) { return value.id == check.id; });
        std::cout << "mapUses " << check.id << " functions=" << entry->functionCount
                  << " status=" << UiLibrary::ToString(check.status) << '\n';
    }
    return 0;
}
