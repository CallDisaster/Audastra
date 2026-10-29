#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/ScriptInjectionSources.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Hash.h"
#include "../Jass/JassLexer.h"
#include "../Jass/JassCheckBudget.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

// 代码注入：函数库 jass 目录中每个 xxx.cfg 列出函数名，对应 xxx.j 在脚本用到这些函数时追加到脚本末尾。
// cfg 行格式：空行与 # 注释忽略；[general] 通用、[new] 仅 1.24+、[old] 仅 1.20。
namespace War3Map::Script {
namespace {

std::optional<std::string> ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return std::move(buffer).str();
}

void Warn(std::vector<Diagnostic>& diagnostics, std::string code, std::string message) {
    diagnostics.push_back({DiagnosticSeverity::Warning, "Script.Inject", std::move(code), std::move(message), {}});
}

} // namespace

InjectTable ScanInjectTable(const std::vector<std::filesystem::path>& jassDirs, bool version24, std::vector<Diagnostic>& diagnostics) {
    struct Candidate {
        std::filesystem::path file;
        std::filesystem::file_time_type modified;
    };
    std::map<std::string, Candidate> table;
    std::set<std::string> reported;
    for (const auto& directory : jassDirs) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) continue;
        std::vector<std::filesystem::path> configs;
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error) && it->path().extension() == ".cfg") configs.push_back(it->path());
        }
        std::sort(configs.begin(), configs.end());
        for (const auto& config : configs) {
            const auto text = ReadText(config);
            if (!text) {
                Warn(diagnostics, "INJECT_CFG_UNREADABLE", "无法读取注入配置：" + config.string());
                continue;
            }
            auto file = config;
            file.replace_extension(".j");
            std::error_code timeError;
            const auto modified = std::filesystem::last_write_time(file, timeError);
            int section = 0; // 0 通用，1 新版，2 旧版
            std::istringstream lines(*text);
            for (std::string raw; std::getline(lines, raw);) {
                const std::string line = War3Map::Text::TrimAsciiControlSpaceCopy(raw);
                if (line.empty() || line.front() == '#') continue;
                if (line == "[general]") section = 0;
                else if (line == "[new]") section = 1;
                else if (line == "[old]") section = 2;
                else if (section == 0 || section == (version24 ? 1 : 2)) {
                    auto [found, inserted] = table.try_emplace(line, Candidate{file, modified});
                    if (inserted || found->second.file == file) continue;
                    // 重复定义：取修改时间较新的文件（与 YDWE 行为一致），只报告一次。
                    if (modified > found->second.modified) found->second = Candidate{file, modified};
                    if (reported.insert(line).second) {
                        Warn(diagnostics, "INJECT_DUPLICATE", "注入函数 " + line + " 重复定义，使用 " + found->second.file.string());
                    }
                }
            }
        }
    }
    InjectTable result;
    result.entries.reserve(table.size());
    for (auto& [name, candidate] : table) result.entries.push_back({name, std::move(candidate.file)});
    return result;
}

std::string InjectCode(std::string script, const InjectTable& table, std::vector<Diagnostic>& diagnostics,
    std::vector<std::filesystem::path>* injected, std::vector<std::string>* sourceSha256,
    std::span<const std::string_view> requiredIdentifiers) {
    // 只扫描原始脚本（不递归扫描追加内容），与 YDWE 一致。
    std::set<std::filesystem::path> files;
    // 使用共用 JASS/vJass 词法器跳过注释、字符串和 rawcode，按完整标识符匹配。
    // MHDebug_EnableLeakMonitor 等较长名称不能触发另一套 LeakMonitor 库。
    std::unordered_set<std::string_view> identifiers;
    identifiers.insert(requiredIdentifiers.begin(), requiredIdentifiers.end());
    for (const auto& token : Jass::detail::LexJass(script, {.blockComments = true})) {
        if (token.kind == Jass::detail::TokenKind::Identifier) identifiers.insert(token.text);
    }
    for (const auto& entry : table.entries) {
        if (files.contains(entry.file)) continue;
        if (identifiers.contains(entry.functionName)) files.insert(entry.file);
    }
    for (const auto& file : files) {
        const auto text = ReadText(file);
        if (!text) {
            diagnostics.push_back({DiagnosticSeverity::Error, "Script.Inject", "INJECT_FILE_UNREADABLE", "无法读取注入文件：" + file.string(), {}});
            continue;
        }
        if (sourceSha256) {
            const auto hash = Sha256(std::as_bytes(std::span(text->data(), text->size())));
            if (!hash.Succeeded()) {
                diagnostics.insert(diagnostics.end(), hash.diagnostics.begin(), hash.diagnostics.end());
                continue;
            }
            sourceSha256->push_back(*hash.value);
        }
        script += *text;
        script += "\r\n";
        if (injected != nullptr) injected->push_back(file);
    }
    return script;
}

Result<std::string> InjectOwnedCode(std::string_view script, const ScriptInjectionSources& sources,
    const Jass::JassCheckOptions& check, std::vector<std::filesystem::path>* injected) {
    using Output = Result<std::string>;
    try {
    Jass::detail::CheckBudget budget(check);
    budget.RegisterSource(script.size());
    auto tokens = Jass::detail::LexJassBounded(script, {.blockComments = true},
        budget.RemainingTokens(), &budget);
    budget.Poll();
    if (!tokens.Succeeded() || !tokens.value) return Output::Failure("Script.Inject",
        "CODE_CHECK_INJECTION_BUDGET", "作者正文注入词法检查未能完成。");
    budget.ConsumeTokens(tokens.value->size());
    std::unordered_set<std::string_view> identifiers;
    std::size_t visits = 0;
    for (const auto& token : *tokens.value) {
        budget.PollOccasionally(++visits);
        if (token.kind == Jass::detail::TokenKind::Identifier) identifiers.insert(token.text);
    }
    if (auto missing = CheckMissingInjectionLibraries(sources, identifiers); !missing.Succeeded())
        return Output{{}, std::move(missing.diagnostics)};
    std::map<std::filesystem::path, const ScriptInjectionSourceFile*> files;
    for (const auto& choice : sources.choices) {
        budget.PollOccasionally(++visits);
        if (choice.selectedFile >= sources.files.size()) return Output::Failure(
            "Script.Inject", "CODE_CHECK_INJECTION_INVALID", "冻结注入来源无效、已取消或超过预算。");
        if (!identifiers.contains(choice.key)) continue;
        const auto& file = sources.files[choice.selectedFile];
        files.emplace(file.binding.path, &file);
    }
    std::string result(script);
    std::vector<std::filesystem::path> paths;
    for (const auto& [path, file] : files) {
        budget.RegisterSource(file->bytes.size());
        if (result.size() > check.maxSourceBytes || file->bytes.size() > check.maxSourceBytes - result.size()
            || check.maxSourceBytes - result.size() - file->bytes.size() < 2) return Output::Failure(
                "Script.Inject", "CODE_CHECK_INJECTION_BUDGET", "冻结注入正文被取消或超过预算。");
        result.append(reinterpret_cast<const char*>(file->bytes.data()), file->bytes.size());
        result += "\r\n";
        paths.push_back(path);
    }
    if (injected) injected->insert(injected->end(), paths.begin(), paths.end());
    return Output::Success(std::move(result));
    } catch (const std::exception&) {
        return Output::Failure("Script.Inject", "CODE_CHECK_INJECTION_BUDGET",
            "作者正文注入检查被取消或超过预算，未返回部分输出。");
    }
}

} // namespace War3Map::Script
