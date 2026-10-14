#include "War3MapCore/Services/LuaRuntimeCatalog.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Jass/JassDeclarationIndex.h"

#include <array>

namespace War3Map::Services {
namespace {
constexpr std::size_t kScriptLimit = 8 * 1024 * 1024;

std::vector<std::string_view> Lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    for (std::size_t at = 0; at <= text.size(); ++at) {
        if (at == text.size() || text[at] == '\n') {
            auto line = text.substr(begin, at - begin);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            lines.push_back(line);
            begin = at + 1;
        }
    }
    return lines;
}
std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}
// 声明上方紧贴的 // 注释。
std::string Comment(const std::vector<std::string_view>& lines, std::uint32_t line) {
    std::vector<std::string_view> found;
    for (auto index = static_cast<std::ptrdiff_t>(line) - 2; index >= 0 && found.size() < 12; --index) {
        const auto text = Trim(lines[static_cast<std::size_t>(index)]);
        if (!text.starts_with("//")) break;
        auto body = Trim(text.substr(2));
        while (!body.empty() && body.front() == '/') body.remove_prefix(1);
        found.push_back(Trim(body));
    }
    std::string result;
    for (auto at = found.rbegin(); at != found.rend(); ++at) {
        if (at->empty() && result.empty()) continue;
        if (!result.empty()) result += '\n';
        result += *at;
    }
    return result;
}
std::string ConstantValue(const std::vector<std::string_view>& lines, std::uint32_t line, std::string_view name) {
    if (line == 0 || line > lines.size()) return {};
    auto text = lines[line - 1];
    const auto at = text.find(name);
    if (at == std::string_view::npos) return {};
    const auto equal = text.find('=', at + name.size());
    if (equal == std::string_view::npos) return {};
    auto value = text.substr(equal + 1);
    if (const auto comment = value.find("//"); comment != std::string_view::npos) value = value.substr(0, comment);
    value = Trim(value);
    return value.size() > 80 ? std::string{} : std::string(value);
}
std::string Text(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
} // namespace

Result<LuaRuntimeScripts> ReadGameLuaRuntimeScripts(const std::filesystem::path& gameRoot) {
    Result<LuaRuntimeScripts> result;
    auto game = Game::GameDataSource::MountStrict(gameRoot);
    if (!game.Succeeded() || !game.value) { result.diagnostics = std::move(game.diagnostics); return result; }
    const auto read = [&](const char* file) {
        const std::string scripts = std::string("scripts\\") + file;
        return game.value->ReadFileBounded(game.value->HasFile(scripts) ? scripts : file, kScriptLimit);
    };
    auto common = read("common.j");
    if (!common.Succeeded() || !common.value) { result.diagnostics = std::move(common.diagnostics); return result; }
    auto blizzard = read("blizzard.j");
    if (!blizzard.Succeeded() || !blizzard.value) { result.diagnostics = std::move(blizzard.diagnostics); return result; }
    result.value = LuaRuntimeScripts{Text(*common.value), Text(*blizzard.value), "游戏 common.j"};
    return result;
}

std::shared_ptr<const Lua::ApiCatalog> BuildLuaRuntimeCatalog(const LuaRuntimeScripts* scripts,
    const std::vector<std::pair<std::string, std::string>>& mapGlobals, std::stop_token cancel) {
    Lua::OwnedRuntimeInput input;
    input.source = scripts ? scripts->source : std::string("未加载 common.j");
    if (scripts) {
        const auto hash = Sha256Text(scripts->common);
        input.binding = "owned-lua:" + (hash.Succeeded() && hash.value ? hash.value->substr(0, 16) : std::string("unhashed"));
        const std::array<Jass::JassSource, 2> sources{{{"common.j", scripts->common}, {"blizzard.j", scripts->blizzard}}};
        Jass::JassCheckOptions options;
        options.cancel = cancel;
        const auto index = Jass::IndexJassDeclarations(sources, options);
        if (index.completed) {
            const auto commonLines = Lines(scripts->common);
            const auto blizzardLines = Lines(scripts->blizzard);
            for (const auto& symbol : index.symbols) {
                const bool common = symbol.source == "common.j";
                const auto& lines = common ? commonLines : blizzardLines;
                switch (symbol.kind) {
                case Jass::DeclarationKind::Type:
                    input.types.emplace_back(symbol.name, symbol.type);
                    break;
                case Jass::DeclarationKind::Native:
                    if (!common) break;
                    {
                        Lua::JassNativeInput native{symbol.name, {}, symbol.type, Comment(lines, symbol.line)};
                        for (const auto& parameter : symbol.parameters) native.parameters.push_back({parameter.name, parameter.type});
                        input.natives.push_back(std::move(native));
                    }
                    break;
                case Jass::DeclarationKind::Global: {
                    Lua::JassGlobalInput global{symbol.name, symbol.array ? symbol.type + " array" : symbol.type,
                        symbol.constant ? ConstantValue(lines, symbol.line, symbol.name) : std::string{},
                        Comment(lines, symbol.line), symbol.constant};
                    (common ? input.commonGlobals : input.otherGlobals).push_back(std::move(global));
                    break;
                }
                default:
                    break;
                }
            }
        } else {
            input.source += "（解析未完成：" + index.reason + "）";
        }
    }
    for (const auto& [name, type] : mapGlobals) input.otherGlobals.push_back({name, type, {}, "地图变量", false});
    return Lua::BuildOwnedRuntimeCatalog(std::move(input));
}

Lua::SemanticRuntime OwnedRuntimeSemantics(const Lua::ApiCatalog& catalog) {
    Lua::SemanticRuntime runtime;
    if (!catalog.nativesLoaded) {
        runtime.reason = "没有读取到游戏 common.j，未检查 jass.* 接口。";
        return runtime;
    }
    runtime.verified = true;
    runtime.binding = catalog.binding;
    runtime.reason = "接口来自" + catalog.source + "。";
    runtime.moduleRegistryComplete = true;
    for (const auto& [name, module] : catalog.modules) if (module.requirable) runtime.presentModules.insert(name);
    if (const auto* common = catalog.Module("jass.common")) {
        runtime.commonLookupVerified = true;
        for (const auto& member : common->members)
            if (member.kind == Lua::ApiKind::Function) runtime.commonArguments.emplace(member.name, member.parameters.size());
    }
    return runtime;
}

} // namespace War3Map::Services
