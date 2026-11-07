#include "NativeTypeHierarchy.h"
#include "NativeSignature.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "../../Jass/JassParser.h"

#include <chrono>
#include <exception>

namespace Audastra::Runtime32::OwnLua {
namespace {
constexpr std::size_t kCommonLimit = 4 * 1024 * 1024;
std::string Error(const std::vector<War3Map::Diagnostic>& diagnostics) {
    return diagnostics.empty() ? "来源未返回有效数据。" : diagnostics.front().code + ": " + diagnostics.front().message;
}
std::optional<NativeGlobalDeclaration> Declaration(std::string_view type, bool array, bool constant,
    const NativeTypeHierarchy& hierarchy) {
    NativeGlobalDeclaration value; value.array = array; value.constant = constant;
    if (type == "integer") value.type.kind = NativeKind::Integer;
    else if (type == "real") value.type.kind = NativeKind::Real;
    else if (type == "boolean") value.type.kind = NativeKind::Boolean;
    else if (type == "string") value.type.kind = NativeKind::String;
    else if (type == "code") value.type.kind = NativeKind::Code;
    else if (hierarchy.IsA(type, type)) { value.type.kind = NativeKind::Handle; value.type.handleType = type; }
    else return {};
    return value;
}
}

NativeTypesResult NativeTypeHierarchy::ReadGame(const std::filesystem::path& gameRoot) {
    try {
        auto mounted = War3Map::Game::GameDataSource::MountStrict(gameRoot);
        if (!mounted.Succeeded() || !mounted.value) return {{}, {}, "无法挂载可信游戏类型来源：" + Error(mounted.diagnostics)};
        auto common = mounted.value->ReadFileBoundedWithSource("scripts\\common.j", kCommonLimit);
        if (!common.Succeeded() || !common.value) return {{}, {}, "无法读取游戏 common.j：" + Error(common.diagnostics)};
        const auto& bytes = common.value->bytes;
        auto parsed = Parse({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (!parsed.types) return parsed;
        auto blizzard = mounted.value->ReadFileBoundedWithSource("scripts\\blizzard.j", kCommonLimit);
        if (!blizzard.Succeeded()) return {{}, {}, "无法读取游戏 Blizzard.j：" + Error(blizzard.diagnostics)};
        parsed = parsed.types->WithGlobals({reinterpret_cast<const char*>(blizzard.value->bytes.data()), blizzard.value->bytes.size()});
        parsed.sourceArchive = common.value->archiveName;
        return parsed;
    } catch (const std::exception& error) { return {{}, {}, std::string("读取游戏类型来源异常：") + error.what()}; }
    catch (...) { return {{}, {}, "读取游戏类型来源发生未知异常。"}; }
}
NativeTypesResult NativeTypeHierarchy::Parse(std::string_view source) {
    if (source.empty() || source.size() > kCommonLimit) return {{}, {}, "common.j 为空或超过 4 MiB 预算。"};
    try {
        War3Map::Jass::JassCheckOptions options;
        options.maxDuration = std::chrono::milliseconds(1000);
        War3Map::Jass::detail::CheckBudget budget(options);
        const auto program = War3Map::Jass::detail::ParseProgram(source, &budget);
        if (!program.errors.empty()) return {{}, {}, "common.j 第 " + std::to_string(program.errors.front().line)
            + " 行无法解析：" + program.errors.front().message};
        if (program.types.empty() || program.types.size() > 1024) return {{}, {}, "common.j 没有类型声明或超过 1024 项预算。"};
        auto hierarchy = std::make_shared<NativeTypeHierarchy>();
        hierarchy->parents_.emplace("handle", ""); // JASS 语言内建根类型；其余全部来自实际声明。
        for (const auto& type : program.types) {
            if (!NativeIdentifier(type.name) || !NativeIdentifier(type.parent) || hierarchy->parents_.contains(type.name)
                || !hierarchy->parents_.contains(type.parent))
                return {{}, {}, "common.j 第 " + std::to_string(type.line) + " 行含重复、未知父类型或非 handle 类型声明。"};
            hierarchy->parents_.emplace(type.name, type.parent);
        }
        if (program.globals.size() > 16384) return {{}, {}, "common.j 全局声明超过 16384 项预算。"};
        for (const auto& global : program.globals) {
            auto declaration = Declaration(global.type, global.array, global.constant, *hierarchy);
            if (!declaration) return {{}, {}, "common.j 全局声明包含未知类型：" + std::string(global.type)};
            if (!NativeIdentifier(global.name) || !hierarchy->globals_.emplace(global.name, std::move(*declaration)).second)
                return {{}, {}, "common.j 全局声明包含无效或重复名称。"};
        }
        return {std::move(hierarchy), {}, {}};
    } catch (const std::exception& error) { return {{}, {}, std::string("解析游戏类型来源失败：") + error.what()}; }
    catch (...) { return {{}, {}, "解析游戏类型来源失败。"}; }
}
NativeTypesResult NativeTypeHierarchy::WithGlobals(std::string_view source) const {
    if (source.size() > 16u * 1024u * 1024u) return {{}, {}, "地图全局声明来源超过16 MiB。"};
    try {
        War3Map::Jass::JassCheckOptions options;
        options.maxDuration = std::chrono::milliseconds(2000);
        War3Map::Jass::detail::CheckBudget budget(options);
        const auto program = War3Map::Jass::detail::ParseProgram(source, &budget);
        if (!program.errors.empty()) return {{}, {}, "全局声明来源无法解析：" + program.errors.front().message};
        auto output = std::make_shared<NativeTypeHierarchy>(*this);
        if (program.globals.size() > 65536 - std::min<std::size_t>(65536, globals_.size())) return {{}, {}, "全局声明超过65536项。"};
        for (const auto& global : program.globals) {
            auto declaration = Declaration(global.type, global.array, global.constant, *this);
            if (!declaration || !NativeIdentifier(global.name) || !output->globals_.emplace(global.name, std::move(*declaration)).second)
                return {{}, {}, "全局声明类型未知或名称冲突：" + std::string(global.name)};
        }
        return {std::move(output), {}, {}};
    } catch (const std::exception& error) { return {{}, {}, std::string("读取全局声明失败：") + error.what()}; }
}
std::optional<NativeGlobalDeclaration> NativeTypeHierarchy::Global(std::string_view name) const {
    const auto found = globals_.find(name);
    return found == globals_.end() ? std::nullopt : std::optional<NativeGlobalDeclaration>(found->second);
}
bool NativeTypeHierarchy::IsA(std::string_view actual, std::string_view expected) const noexcept {
    if (!parents_.contains(expected)) return false;
    for (std::size_t depth = 0; depth <= parents_.size(); ++depth) {
        const auto found = parents_.find(actual);
        if (found == parents_.end()) return false;
        if (actual == expected) return true;
        if (found->second.empty()) return false;
        actual = found->second;
    }
    return false;
}

} // namespace Audastra::Runtime32::OwnLua
