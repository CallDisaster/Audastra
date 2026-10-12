#include "War3MapCore/Trigger/UiCallbackActions.h"

#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

namespace War3Map::Trigger {
namespace {

constexpr std::size_t kMaxFiles = 8192;
constexpr std::uintmax_t kMaxFileBytes = 8u * 1024u * 1024u;
constexpr std::uintmax_t kMaxTotalBytes = 64u * 1024u * 1024u;

std::string PathText(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

Diagnostic Note(DiagnosticSeverity severity, std::string code, std::string message, const std::filesystem::path& source = {}) {
    Diagnostic diagnostic{severity, "Trigger.UiCallback", std::move(code), std::move(message), std::nullopt};
    if (!source.empty()) diagnostic.origin = OriginSpan{source, 0, 0, {}};
    return diagnostic;
}

bool IdentifierStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool IdentifierChar(char c) { return IdentifierStart(c) || (c >= '0' && c <= '9'); }
bool Space(char c) { return c == ' ' || c == '\t'; }

// 一行内的标识符序列（遇到非标识符、非空白、非逗号字符时停止）。
std::vector<std::string_view> Words(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < line.size()) {
        if (Space(line[at]) || line[at] == ',') { ++at; continue; }
        if (!IdentifierStart(line[at])) break;
        const auto begin = at;
        while (at < line.size() && IdentifierChar(line[at])) ++at;
        words.push_back(line.substr(begin, at - begin));
    }
    return words;
}

// 行尾 // 注释：跳过同一行内引号中的内容（JASS 字符串与四字码）。
std::string_view StripLineComment(std::string_view line) {
    char quote = 0;
    for (std::size_t at = 0; at < line.size(); ++at) {
        const char c = line[at];
        if (quote != 0) {
            if (c == '\\' && quote == '"') { ++at; continue; }
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '/' && at + 1 < line.size() && line[at + 1] == '/') {
            return line.substr(0, at);
        }
    }
    return line;
}

std::string ScriptNameOf(const UiDefinition& definition) {
    for (const auto& [key, value] : definition.Active().fields) {
        if ((key == "script_name" || key == "script") && !value.empty()) {
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
            return value;
        }
    }
    return definition.name;
}

// 标题是作者可见的唯一语义线索；只认三个关键词，空白差异（“做动作 (多项)”“做动作(多项)”）不影响。
bool EnumeratesImmediately(const UiCatalog& catalog, const UiDefinition& definition) {
    const auto title = DefinitionTitle(catalog, definition);
    return title.find("做动作") != std::string::npos || title.find("做多动作") != std::string::npos ||
        title.find("遍历") != std::string::npos;
}

bool SameTypes(const UiJassDeclaration& left, const UiJassDeclaration& right) {
    return left.parameterTypes == right.parameterTypes;
}

} // namespace

const UiCallbackAction* UiCallbackActionIndex::Find(std::string_view actionName) const {
    const auto found = actions.find(std::string(actionName));
    return found == actions.end() ? nullptr : &found->second;
}

std::vector<UiJassDeclaration> ScanUiJassDeclarations(std::string_view text, const std::filesystem::path& source) {
    std::vector<UiJassDeclaration> result;
    std::uint32_t lineNumber = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        auto end = text.find_first_of("\r\n", at);
        if (end == std::string_view::npos) end = text.size();
        ++lineNumber;
        const auto words = Words(StripLineComment(text.substr(at, end - at)));
        std::size_t word = 0;
        while (word < words.size() && (words[word] == "private" || words[word] == "public" ||
                                          words[word] == "static" || words[word] == "constant")) ++word;
        // 形如 native|function 名 takes …… returns 类型；参数为“类型 名”对或 nothing。
        if (word + 3 < words.size() && (words[word] == "native" || words[word] == "function") &&
            words[word + 2] == "takes") {
            UiJassDeclaration declaration{std::string(words[word + 1]), {}, source, lineNumber};
            std::size_t cursor = word + 3;
            if (cursor < words.size() && words[cursor] == "nothing") {
                ++cursor;
            } else {
                while (cursor + 1 < words.size() && words[cursor] != "returns") {
                    declaration.parameterTypes.emplace_back(words[cursor]);
                    cursor += 2;
                }
            }
            if (cursor + 1 < words.size() && words[cursor] == "returns") result.push_back(std::move(declaration));
        }
        if (end == text.size()) break;
        at = end + 1;
        if (text[end] == '\r' && at < text.size() && text[at] == '\n') ++at;
    }
    return result;
}

UiCallbackActionIndex BuildUiCallbackActionIndex(const UiCatalog& catalog, std::span<const UiJassDeclaration> declarations) {
    UiCallbackActionIndex index;
    std::map<std::string, std::vector<const UiJassDeclaration*>, std::less<>> byName;
    for (const auto& declaration : declarations) byName[declaration.name].push_back(&declaration);
    for (const auto* definition : catalog.Definitions(UiDefinitionKind::Action)) {
        const auto found = byName.find(ScriptNameOf(*definition));
        if (found == byName.end()) continue;
        const auto& candidates = found->second;
        const auto& first = *candidates.front();
        const std::size_t uiArguments = definition->ArgumentCount();
        if (first.parameterTypes.size() != uiArguments + 1 || first.parameterTypes.back() != "code") continue;
        const bool consistent = std::all_of(candidates.begin(), candidates.end(),
            [&](const UiJassDeclaration* other) { return SameTypes(*other, first); });
        if (!consistent) {
            index.diagnostics.push_back(Note(DiagnosticSeverity::Warning, "ui.callback.ambiguous",
                "动作 " + definition->name + " 在函数库 J 中有参数不一致的多份声明，不按回调块处理。", first.source));
            continue;
        }
        index.actions.emplace(definition->name,
            UiCallbackAction{uiArguments, EnumeratesImmediately(catalog, *definition), first.source, first.line});
    }
    return index;
}

std::vector<std::filesystem::path> UiCallbackScanRoots(const std::filesystem::path& uiRoot, std::span<const std::string> layers) {
    std::vector<std::filesystem::path> roots;
    for (const auto& layer : layers) {
        roots.push_back(uiRoot / std::filesystem::path(std::u8string(layer.begin(), layer.end())) / "jass");
    }
    const auto normalized = uiRoot.lexically_normal();
    auto tail = normalized;
    if (tail.has_filename() == false) tail = tail.parent_path();
    if (tail.filename() == "mpq" && tail.parent_path().filename() == "share") {
        roots.push_back(tail.parent_path().parent_path() / "jass");
    }
    return roots;
}

UiJassDeclarationScan ScanUiJassRoots(std::span<const std::filesystem::path> roots) {
    UiJassDeclarationScan scan;
    std::size_t files = 0;
    std::uintmax_t total = 0;
    std::set<std::filesystem::path> seen;
    const auto stop = [&](std::string code, std::string message, const std::filesystem::path& where) {
        scan.complete = false;
        scan.diagnostics.push_back(Note(DiagnosticSeverity::Warning, std::move(code), std::move(message), where));
    };
    for (const auto& root : roots) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) continue;
        std::vector<std::filesystem::path> paths;
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), last;
             !error && it != last; it.increment(error)) {
            if (it->is_symlink(error)) { if (it->is_directory(error)) it.disable_recursion_pending(); continue; }
            if (!it->is_regular_file(error)) continue;
            auto extension = PathText(it->path().extension());
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".j") paths.push_back(it->path());
        }
        if (error) { stop("ui.callback.scan-failed", "读取函数库 J 目录失败：" + error.message(), root); return scan; }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            if (!seen.insert(path.lexically_normal()).second) continue;
            if (++files > kMaxFiles) { stop("ui.callback.scan-budget", "函数库 J 文件超过 8192 个，回调动作识别不完整。", root); return scan; }
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > kMaxFileBytes || total + size > kMaxTotalBytes) {
                stop("ui.callback.scan-budget", "函数库 J 文件过大或总量超过 64 MiB，回调动作识别不完整。", path);
                return scan;
            }
            total += size;
            std::ifstream stream(path, std::ios::binary);
            std::string text(static_cast<std::size_t>(size), '\0');
            if (!stream.read(text.data(), static_cast<std::streamsize>(size))) {
                stop("ui.callback.scan-failed", "函数库 J 读取失败。", path);
                return scan;
            }
            auto found = ScanUiJassDeclarations(text, path);
            scan.declarations.insert(scan.declarations.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        }
    }
    return scan;
}

} // namespace War3Map::Trigger
