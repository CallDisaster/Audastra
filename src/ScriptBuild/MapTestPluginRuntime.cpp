#include "MapTestPluginRuntime.h"

#include "RuntimePeInspection.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>

namespace War3Map::Script {
namespace {

namespace fs = std::filesystem;
using Plan = PluginConfigRuntimePlan;

// 与启动组件的 kRuntimeModuleLimit 相同。
constexpr std::size_t kModuleLimit = 32;
constexpr std::uint64_t kPeBudget = 32ull * 1024 * 1024;
constexpr std::uint64_t kConfigBudget = 64ull * 1024;
constexpr std::size_t kBinListingLimit = 4096;

Result<Plan> Fail(std::string code, std::string message) {
    return Result<Plan>::Failure("Map.Test.Runtime", std::move(code), std::move(message));
}

std::string Lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool Regular(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

// API set（api-ms-*/ext-ms-*）由系统解析，不能从 bin 里装。
bool ApiSet(std::string_view name) {
    return name.starts_with("api-ms-") || name.starts_with("ext-ms-");
}

// config.cfg 里的插件名只能是 plugin/warcraft3 下的 DLL 文件名。
bool PluginFileName(std::string_view name) {
    if (name.size() < 5 || name.size() > 128 || name.find_first_of("/\\:") != std::string_view::npos) return false;
    if (name.front() == '.' || name.back() == ' ') return false;
    if (std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e; }))
        return false;
    return Lower(std::string(name.substr(name.size() - 4))) == ".dll";
}

// config.cfg 的 [Enable] 段，按文件顺序；值为 0 或空的不装。
Result<std::vector<std::string>> EnabledPlugins(const fs::path& config) {
    const auto bytes = IO::ReadFileBytes(config, kConfigBudget);
    if (!bytes.Succeeded())
        return Result<std::vector<std::string>>::Failure("Map.Test.Runtime", "MAP_TEST_PLUGIN_CONFIG",
            "无法读取 " + IO::PathUtf8(config) + "（不存在或超过 64 KiB）。");
    std::string_view text(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::vector<std::string> enabled;
    std::set<std::string> seen;
    std::string section;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = Text::TrimCWhitespace(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            section = Lower(Text::TrimCWhitespaceCopy(line.substr(1, close == std::string_view::npos ? line.size() - 1 : close - 1)));
            continue;
        }
        if (section != "enable") continue;
        const auto equal = line.find('=');
        if (equal == std::string_view::npos) continue;
        const auto name = Text::TrimCWhitespaceCopy(line.substr(0, equal));
        const auto value = Text::TrimCWhitespaceCopy(line.substr(equal + 1));
        if (value.empty() || value == "0") continue;
        if (!PluginFileName(name))
            return Result<std::vector<std::string>>::Failure("Map.Test.Runtime", "MAP_TEST_PLUGIN_CONFIG",
                IO::PathUtf8(config) + " 里的“" + name + "”不是 plugin/warcraft3 下的 DLL 文件名。");
        if (seen.insert(Lower(name)).second) enabled.push_back(name);
    }
    return Result<std::vector<std::string>>::Success(std::move(enabled));
}

// 在文件里找地图要的接口名（以非标识符字符隔开的完整名字）。
void MarkNatives(std::span<const std::byte> bytes, const std::set<std::string>& wanted, std::set<std::string>& found) {
    if (wanted.empty() || found.size() == wanted.size()) return;
    const auto identifier = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && !identifier(text[start])) ++start;
        auto end = start;
        while (end < text.size() && identifier(text[end])) ++end;
        if (end - start >= 2 && end - start <= 128) {
            const std::string name(text.substr(start, end - start));
            if (wanted.contains(name)) found.insert(name);
        }
        start = end;
    }
}

} // namespace

bool IsPluginConfigRuntime(const fs::path& root) {
    return !root.empty() && Regular(root / "bin" / "ydbase.dll") && Regular(root / "plugin" / "warcraft3" / "config.cfg");
}

Result<Plan> PlanPluginConfigRuntime(const fs::path& root, const std::vector<std::string>& natives) {
    if (!IsPluginConfigRuntime(root))
        return Fail("MAP_TEST_PLUGIN_RUNTIME_MISSING", "所选目录不是 KKWE 运行库（缺少 bin/ydbase.dll 或 plugin/warcraft3/config.cfg）："
            + IO::PathUtf8(root));
    const auto pluginDirectory = root / "plugin" / "warcraft3";
    auto enabled = EnabledPlugins(pluginDirectory / "config.cfg");
    if (!enabled.Succeeded()) return {std::nullopt, std::move(enabled.diagnostics)};

    // bin 里的文件：小写名 → 实际文件名（导入表里的大小写与磁盘不一定一致）。
    std::map<std::string, fs::path> bin;
    std::error_code error;
    for (fs::directory_iterator it(root / "bin", error), end; !error && it != end; it.increment(error)) {
        if (bin.size() >= kBinListingLimit) break;
        std::error_code typeError;
        if (!it->is_regular_file(typeError) || typeError) continue;
        const auto name = it->path().filename();
        bin.emplace(Lower(IO::PathUtf8(name)), name);
    }
    if (error) return Fail("MAP_TEST_PLUGIN_RUNTIME_MISSING", "无法列出 " + IO::PathUtf8(root / "bin") + "。");

    Plan plan;
    const std::set<std::string> wanted(natives.begin(), natives.end());
    std::set<std::string> found;
    std::set<std::string> added;
    std::optional<Result<Plan>> failure;
    const auto inspect = [&](const fs::path& path) -> std::optional<RuntimeModuleImage> {
        const auto bytes = IO::ReadFileBytes(path, kPeBudget);
        if (!bytes.Succeeded()) {
            failure = Fail("MAP_TEST_RUNTIME_PE", "无法读取 " + IO::PathUtf8(path) + "（不存在或超过 32 MiB）。");
            return std::nullopt;
        }
        auto image = InspectRuntimeModulePe(*bytes.value);
        if (!image.Succeeded()) {
            failure = Fail("MAP_TEST_RUNTIME_PE", IO::PathUtf8(path) + "：" + image.diagnostics.front().message);
            return std::nullopt;
        }
        MarkNatives(*bytes.value, wanted, found);
        return std::move(*image.value);
    };
    const auto push = [&](Services::MapTestRuntimeModule module) {
        if (plan.modules.size() >= kModuleLimit) {
            failure = Fail("MAP_TEST_PLUGIN_RUNTIME_LIMIT", "KKWE 运行库要装入的文件超过 "
                + std::to_string(kModuleLimit) + " 个，请在 plugin/warcraft3/config.cfg 里关掉不用的插件。");
            return false;
        }
        plan.modules.push_back(std::move(module));
        return true;
    };
    // 依赖在前：先递归放好它在 bin 里的依赖，再放它自己。
    std::function<bool(const std::string&, int)> addBin = [&](const std::string& lower, int depth) {
        if (ApiSet(lower) || added.contains(lower)) return true;
        const auto file = bin.find(lower);
        if (file == bin.end()) return true; // 系统或游戏目录里的 DLL
        if (depth > 16) {
            failure = Fail("MAP_TEST_RUNTIME_PE", "bin 里的 DLL 依赖层级过深：" + IO::PathUtf8(file->second));
            return false;
        }
        added.insert(lower);
        const auto image = inspect(root / "bin" / file->second);
        if (!image) return false;
        for (const auto& name : image->imports)
            if (!addBin(name, depth + 1)) return false;
        const bool lua = lower == "luacore.dll" || lower == "lua53.dll";
        return push({fs::path("bin") / file->second, false, lua});
    };
    for (const auto& plugin : *enabled.value) {
        const auto lower = Lower(plugin);
        if (lower == "yd_loader.dll") continue;
        const auto path = pluginDirectory / plugin;
        if (!Regular(path)) {
            plan.missing.push_back(plugin);
            continue;
        }
        if (added.contains(lower)) continue;
        added.insert(lower);
        const auto image = inspect(path);
        if (!image) return std::move(*failure);
        for (const auto& name : image->imports)
            if (!addBin(name, 0)) return std::move(*failure);
        if (!push({fs::path("plugin") / "warcraft3" / plugin, image->initialize, false})) return std::move(*failure);
        plan.plugins.push_back(plugin);
    }
    if (plan.plugins.empty())
        return Fail("MAP_TEST_PLUGIN_CONFIG", IO::PathUtf8(pluginDirectory / "config.cfg") + " 没有启用任何可装入的插件。");
    for (const auto& name : wanted)
        if (!found.contains(name)) plan.unmatchedNatives.push_back(name);
    return Result<Plan>::Success(std::move(plan));
}

} // namespace War3Map::Script
