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
}

NativeTypesResult NativeTypeHierarchy::ReadGame(const std::filesystem::path& gameRoot) {
    try {
        auto mounted = War3Map::Game::GameDataSource::MountStrict(gameRoot);
        if (!mounted.Succeeded() || !mounted.value) return {{}, {}, "无法挂载可信游戏类型来源：" + Error(mounted.diagnostics)};
        auto common = mounted.value->ReadFileBoundedWithSource("scripts\\common.j", kCommonLimit);
        if (!common.Succeeded() || !common.value) return {{}, {}, "无法读取游戏 common.j：" + Error(common.diagnostics)};
        const auto& bytes = common.value->bytes;
        auto parsed = Parse({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
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
        return {std::move(hierarchy), {}, {}};
    } catch (const std::exception& error) { return {{}, {}, std::string("解析游戏类型来源失败：") + error.what()}; }
    catch (...) { return {{}, {}, "解析游戏类型来源失败。"}; }
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
