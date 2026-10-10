#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Jass/VJass.h"
#include "War3MapCore/Jass/Preprocessor.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace War3Map::Services {
namespace {
constexpr std::size_t kSourceLimit = 32 * 1024 * 1024;
constexpr std::size_t kTotalLimit = 64 * 1024 * 1024;
constexpr std::size_t kNameLimit = 1024;
struct Stopped {};
struct CallBudget {
    std::stop_token cancel;
    std::chrono::steady_clock::time_point deadline;
    void Poll() const {
        if (cancel.stop_requested() || std::chrono::steady_clock::now() >= deadline) throw Stopped{};
    }
    void Occasionally(std::size_t at) const { if ((at & 4095u) == 0) Poll(); }
};

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
bool ValidName(std::string_view name) {
    return !name.empty() && name.size() <= kNameLimit
        && std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20; });
}
std::uint32_t Lines(std::string_view text, const CallBudget& budget) {
    std::uint32_t result = 1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        budget.Occasionally(i);
        if (text[i] == '\r') { ++result; if (i + 1 < text.size() && text[i + 1] == '\n') ++i; }
        else if (text[i] == '\n') ++result;
    }
    return result;
}
struct SourceCursor {
    std::string name;
    std::string_view text;
    std::size_t at = 0;
    std::uint32_t line = 1;
    std::size_t lineStart = 0;
    std::uint32_t Locate(std::size_t offset, const CallBudget& budget) {
        offset = (std::min)(offset, text.size());
        if (offset < at) { at = 0; line = 1; lineStart = 0; }
        for (; at < offset; ++at) {
            budget.Occasionally(at);
            if (text[at] == '\r') { ++line; if (at + 1 < offset && text[at + 1] == '\n') ++at; lineStart = at + 1; }
            else if (text[at] == '\n') { ++line; lineStart = at + 1; }
        }
        return line;
    }
};
bool IsPrimaryLine(const CodeCheckRequest& request, std::uint32_t line, std::uint32_t begin, std::uint32_t count) {
    if (request.prefix.empty() && request.suffix.empty()) return true;
    return line >= begin && (line < begin + count || (request.suffix.empty() && line == begin + count));
}
void Fail(CodeCheckResult& result, std::string code, std::string message) {
    result.completed = false;
    result.syntaxChecked = false;
    result.semanticChecked = false;
    result.contextComplete = false;
    result.reason = message;
    result.diagnostics.push_back({DiagnosticSeverity::Error, {}, std::move(code), std::move(message)});
}
void AppendJassDiagnostics(CodeCheckResult& result, const std::vector<Diagnostic>& diagnostics,
    std::vector<SourceCursor>& sources, const CodeCheckRequest& request, std::uint32_t primaryLine,
    std::uint32_t primaryLines, const Jass::SourceMap& sourceMap, const CallBudget& budget,
    const Jass::VJassResult* emission = nullptr, const Jass::SourceMap* emissionInputMap = nullptr,
    const std::vector<bool>* exactOrigins = nullptr) {
    std::size_t diagnosticIndex = 0;
    for (const auto& diagnostic : diagnostics) {
        budget.Poll();
        const bool backendExact = exactOrigins && diagnosticIndex < exactOrigins->size() && (*exactOrigins)[diagnosticIndex];
        ++diagnosticIndex;
        CodeCheckDiagnostic output;
        output.severity = diagnostic.severity;
        output.code = diagnostic.code;
        output.message = diagnostic.message;
        if (diagnostic.origin) {
            // K1 的 origin 是原字节范围；不从可能已摘要的显示名称反推身份。
            const auto name = Utf8Path(diagnostic.origin->source);
            auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& source) {
                return name == source.name || diagnostic.origin->source == std::filesystem::path(source.name);
            });
            if (found != sources.end()) {
                output.source = found->name;
                output.line = found->Locate(diagnostic.origin->offset, budget);
                output.byteColumn = static_cast<std::uint32_t>((std::min)(found->at - found->lineStart + 1,
                    static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())));
                output.byteLength = static_cast<std::uint32_t>((std::min)(diagnostic.origin->length,
                    static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())));
                output.originMapped = true;
                output.byteRangeExact = backendExact;
                if (!backendExact) { output.byteColumn = 1; output.byteLength = 0; }
                if (found->name == request.source.name) {
                    if (emission && backendExact) {
                        const auto position = emission->FindByteOrigin(diagnostic.origin->offset);
                        if (position) {
                            output.line = position->sourceLine; output.byteColumn = position->sourceColumn;
                            output.byteLength = position->exact ? (std::min)(output.byteLength, position->byteLength) : 0;
                            output.originMapped = position->exact;
                            output.byteRangeExact = position->exact;
                            if (emissionInputMap && !emissionInputMap->Empty()) {
                                if (const auto origin = emissionInputMap->Find(output.line)) {
                                    output.source = emissionInputMap->Files().at(origin->file); output.line = origin->line;
                                    output.byteColumn = 1; output.byteLength = 0;
                                    output.byteRangeExact = false;
                                } else output.originMapped = false;
                            }
                            if (!position->exact) output.message += "（生成片段，显示对应作者行附近的锚点。）";
                        } else { output.line = 0; output.byteLength = 0; output.originMapped = false; output.byteRangeExact = false; }
                    } else if (!sourceMap.Empty()) {
                        if (const auto position = sourceMap.Find(output.line)) {
                            output.source = sourceMap.Files().at(position->file);
                            output.line = position->line;
                            // 行映射不声称重写后的列/字节范围仍对应作者正文。
                            output.byteColumn = 1; output.byteLength = 0;
                            output.byteRangeExact = false;
                        } else output.originMapped = false;
                    } else if (IsPrimaryLine(request, output.line, primaryLine, primaryLines)) {
                        output.line -= primaryLine - 1;
                    } else if (!request.prefix.empty() || !request.suffix.empty()) {
                        output.source += "（生成上下文）";
                        output.originMapped = false;
                    }
                }
            }
        }
        result.diagnostics.push_back(std::move(output));
    }
}

Jass::SourceMap ComposeMain(const Jass::SourceMap& outer, const Jass::SourceMap& inner,
    std::string_view mainName, std::uint32_t lines, const CallBudget& budget,
    const std::map<std::string, std::string>* aliases = nullptr) {
    Jass::SourceMap result;
    std::map<std::string, std::uint32_t> files;
    const auto fileId = [&](const std::string& name) {
        auto found = files.find(name);
        if (found == files.end()) found = files.emplace(name, result.AddFile(name)).first;
        return found->second;
    };
    for (std::uint32_t line = 1; line <= lines; ++line) {
        budget.Occasionally(line);
        const auto position = outer.Find(line);
        if (!position || position->file >= outer.Files().size()) continue;
        const auto& name = outer.Files()[position->file];
        if (name == mainName && !inner.Empty()) {
            if (const auto origin = inner.Find(position->line); origin && origin->file < inner.Files().size()) {
                result.Map(line, {fileId(inner.Files()[origin->file]), origin->line}); continue;
            }
        }
        const auto alias = aliases ? aliases->find(name) : std::map<std::string, std::string>::const_iterator{};
        result.Map(line, {fileId(aliases && alias != aliases->end() ? alias->second : name), position->line});
    }
    return result;
}
void MapSemanticIndex(CodeCheckResult& result, const CodeCheckRequest& request,
    const Jass::SourceMap& map, const Jass::VJassResult* emission, const Jass::SourceMap* emissionMap,
    bool sameInputBytes, std::uint32_t primaryLine, std::uint32_t primaryLines, const CallBudget& budget) {
    auto& index = result.semanticIndex;
    std::map<std::string, std::uint32_t> sourceIds;
    const auto file = [&](const std::string& name) {
        auto [found, inserted] = sourceIds.try_emplace(name, static_cast<std::uint32_t>(index.sources.size()));
        if (inserted) index.sources.push_back(name);
        return found->second;
    };
    const auto locate = [&](const Jass::SymbolPosition& raw) {
        budget.Poll(); CodeSymbolPosition output;
        if (raw.key.file >= index.parsed.sources.size()) return output;
        std::string name = index.parsed.sources[raw.key.file];
        output.line = raw.line; output.byteColumn = raw.byteColumn; output.byteLength = raw.length; output.exact = true;
        if (name == request.source.name) {
            if (emission) {
                const auto origin = emission->FindByteOrigin(raw.key.offset);
                if (!origin) { output.line = 0; output.exact = false; }
                else {
                    output.line = origin->sourceLine; output.byteColumn = origin->sourceColumn;
                    output.byteLength = (std::min)(raw.length, origin->byteLength); output.exact = origin->exact;
                    output.inputLine = output.line; output.inputByteColumn = output.byteColumn;
                    output.inputByteLength = output.byteLength; output.inputExact = output.exact;
                    if (emissionMap && !emissionMap->Empty() && !sameInputBytes) {
                        if (const auto mapped = emissionMap->Find(output.line); mapped && mapped->file < emissionMap->Files().size()) {
                            name = emissionMap->Files()[mapped->file]; output.line = mapped->line;
                            output.exact = false;
                        } else { output.line = 0; output.exact = false; }
                    }
                }
            } else if (!request.sourceMap.Empty()) {
                output.inputLine = output.line; output.inputByteColumn = output.byteColumn;
                output.inputByteLength = output.byteLength; output.inputExact = true;
                if (const auto mapped = map.Find(output.line); mapped && mapped->file < map.Files().size()) {
                    name = map.Files()[mapped->file]; output.line = mapped->line; output.exact = false;
                } else { output.line = 0; output.exact = false; }
            } else if (IsPrimaryLine(request, output.line, primaryLine, primaryLines)) output.line -= primaryLine - 1;
            else { name += "（生成上下文）"; output.exact = false; }
        }
        if (!output.exact) { output.byteColumn = 1; output.byteLength = 0; }
        output.file = file(name); return output;
    };
    for (const auto& definition : index.parsed.definitions) index.definitions.push_back(locate(definition.position));
    for (const auto& reference : index.parsed.references) index.references.push_back(locate(reference.position));
}
} // namespace

bool CodeCheckResult::Succeeded() const noexcept {
    return completed && syntaxChecked && std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const auto& value) { return value.severity == DiagnosticSeverity::Error; });
}

CodeCheckResult CheckCode(const CodeCheckRequest& request, std::stop_token cancel) {
    CodeCheckResult result;
    result.generation = request.generation;
    result.contextBinding = request.context.binding;
    result.contextComplete = request.context.complete;
    std::stop_source combinedCancellation;
    const auto requestCancel = request.language == CodeLanguage::Lua53 ? request.lua.cancel : request.jass.cancel;
    std::stop_callback suppliedCancellation(cancel, [&] { combinedCancellation.request_stop(); });
    std::stop_callback configuredCancellation(requestCancel, [&] { combinedCancellation.request_stop(); });
    const auto actualCancel = combinedCancellation.get_token();
    const auto configuredDeadline = request.language == CodeLanguage::Lua53 ? request.lua.deadline : request.jass.deadline;
    const auto configuredDuration = request.language == CodeLanguage::Lua53 ? request.lua.maxDuration : request.jass.maxDuration;
    const CallBudget budget{actualCancel, (std::min)(configuredDeadline, std::chrono::steady_clock::now()
        + std::clamp(configuredDuration, std::chrono::milliseconds{0}, std::chrono::milliseconds{30000}))};
    try {
        if (actualCancel.stop_requested()) {
            result.cancelled = true;
            Fail(result, "CODE_CHECK_CANCELLED", "检查已取消，未形成通过证据。");
            return result;
        }
        budget.Poll();
        if (!ValidName(request.source.name) || request.context.binding.size() > 4096
            || request.context.reason.size() > 4096 || request.source.text.size() > kSourceLimit
            || request.prefix.size() > kSourceLimit || request.suffix.size() > kSourceLimit
            || request.context.declarations.size() > 63 || request.context.includes.size() > 512) {
            Fail(result, "CODE_CHECK_INPUT_BUDGET", "代码检查输入名称、来源数量或文本超过预算。");
            return result;
        }
        std::size_t total = request.source.text.size() + request.prefix.size() + request.suffix.size();
        std::set<std::filesystem::path> declarationNames{std::filesystem::path(request.source.name).lexically_normal()};
        for (const auto* collection : {&request.context.declarations, &request.context.includes}) {
            for (const auto& source : *collection) {
                budget.Poll();
                if (!ValidName(source.name) || source.text.size() > kSourceLimit || total > kTotalLimit - source.text.size()) {
                    Fail(result, "CODE_CHECK_INPUT_BUDGET", "冻结声明/包含文件无效或累计文本超过预算。");
                    return result;
                }
                if (!declarationNames.insert(std::filesystem::path(source.name).lexically_normal()).second) {
                    Fail(result, "CODE_CHECK_SOURCE_AMBIGUOUS", "主正文、冻结声明或包含的来源名重复，不能可靠定位错误。");
                    return result;
                }
                total += source.text.size();
            }
        }
        if (total > kTotalLimit || request.prefix.size() + request.source.text.size() + request.suffix.size() > kSourceLimit) {
            Fail(result, "CODE_CHECK_INPUT_BUDGET", "组合正文或累计文本超过预算。");
            return result;
        }
        const auto primaryLine = Lines(request.prefix, budget);
        const auto primaryLines = Lines(request.source.text, budget)
            - static_cast<std::uint32_t>(!request.source.text.empty()
                && (request.source.text.back() == '\r' || request.source.text.back() == '\n'));
        std::string combined;
        combined.reserve(request.prefix.size() + request.source.text.size() + request.suffix.size());
        combined += request.prefix; combined += request.source.text; combined += request.suffix;
        if (request.language == CodeLanguage::Lua53) {
            auto options = request.lua;
            options.cancel = actualCancel;
            options.deadline = budget.deadline;
            const auto semantic = Lua::CheckSemantics(combined, request.source.name, request.context.luaSemantic, options);
            const auto& checked = semantic.syntax;
            result.completed = checked.completed;
            result.syntaxChecked = checked.completed;
            result.cancelled = std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == "LUA_CHECK_CANCELLED"; });
            result.semanticChecked = semantic.checked;
            result.contextComplete = semantic.complete;
            result.reason = semantic.reason;
            for (const auto& diagnostic : checked.diagnostics) {
                budget.Poll();
                auto line = diagnostic.line;
                const bool inPrimary = IsPrimaryLine(request, line, primaryLine, primaryLines);
                result.diagnostics.push_back({DiagnosticSeverity::Error,
                    inPrimary ? request.source.name : request.source.name + "（生成上下文）", diagnostic.code,
                    diagnostic.message, inPrimary ? line - primaryLine + 1 : line, 1, 0, inPrimary});
            }
            for (const auto& warning : semantic.warnings) {
                budget.Poll();
                const bool inPrimary = IsPrimaryLine(request, warning.line, primaryLine, primaryLines);
                result.diagnostics.push_back({DiagnosticSeverity::Warning,
                    inPrimary ? request.source.name : request.source.name + "（生成上下文）", warning.code,
                    warning.message, inPrimary ? warning.line - primaryLine + 1 : warning.line, 1, 0, inPrimary});
            }
            return result;
        }
        std::vector<Jass::JassSource> sources;
        std::vector<SourceCursor> cursors;
        sources.reserve(request.context.declarations.size() + 1);
        cursors.reserve(request.context.declarations.size() + 1);
        for (const auto& source : request.context.declarations) {
            sources.push_back({source.name, source.text});
            cursors.push_back({source.name, source.text});
        }
        auto options = request.jass;
        options.syntaxOnly = !request.context.complete;
        options.collectSymbols = request.context.complete;
        options.cancel = actualCancel;
        options.deadline = budget.deadline;
        Jass::SourceMap actualMap = request.sourceMap;
        if (actualMap.Empty() && (!request.prefix.empty() || !request.suffix.empty())) {
            const auto lines = Lines(combined, budget);
            const auto primaryFile = actualMap.AddFile(request.source.name);
            const auto generatedFile = actualMap.AddFile(request.source.name + "（生成上下文）");
            for (std::uint32_t line = 1; line <= lines; ++line) {
                budget.Occasionally(line);
                actualMap.Map(line, IsPrimaryLine(request, line, primaryLine, primaryLines)
                    ? Jass::SourcePosition{primaryFile, line - primaryLine + 1} : Jass::SourcePosition{generatedFile, line});
            }
        }
        Jass::VJassResult emissionOrigins;
        Jass::SourceMap emissionInputMap;
        bool sameInputBytes = request.sourceMap.Empty() && request.prefix.empty() && request.suffix.empty();
        if (request.language == CodeLanguage::VJass) {
            std::string preprocessed;
            std::string_view vjassInput = combined;
            if (!request.preprocessed) {
                // 文本/MCP路径只读请求拥有的包含；虚拟根不创建、不枚举，也不会回退磁盘。
                const std::filesystem::path virtualRoot(L"C:/Audastra/CodeCheck");
                std::map<std::filesystem::path, const CodeCheckSource*> includes;
                std::map<std::string, std::string> aliases;
                Jass::PreprocessOptions preprocess;
                preprocess.check = options; preprocess.check.maxSources = 1024;
                preprocess.maxOutputBytes = kSourceLimit; preprocess.userIncludes.push_back(virtualRoot);
                for (const auto& source : request.context.includes) {
                    budget.Poll();
                    auto path = std::filesystem::path(std::u8string(source.name.begin(), source.name.end()));
                    if (!path.is_absolute()) path = virtualRoot / path;
                    path = path.lexically_normal();
                    if (!includes.emplace(path, &source).second) {
                        Fail(result, "CODE_CHECK_SOURCE_AMBIGUOUS", "冻结包含来源重复，不能可靠定位错误。"); return result;
                    }
                    aliases.emplace(Utf8Path(path), source.name);
                    if (std::find(preprocess.userIncludes.begin(), preprocess.userIncludes.end(), path.parent_path()) == preprocess.userIncludes.end())
                        preprocess.userIncludes.push_back(path.parent_path());
                }
                preprocess.includeProvider = [&](const std::filesystem::path& path) {
                    budget.Poll();
                    const auto found = includes.find(path.lexically_normal());
                    return Result<Jass::PreprocessIncludeContent>::Success(found == includes.end()
                        ? Jass::PreprocessIncludeContent{false, {}} : Jass::PreprocessIncludeContent{true, found->second->text});
                };
                const auto inputPath = std::filesystem::path(std::u8string(request.source.name.begin(), request.source.name.end()));
                auto expanded = Jass::Preprocess(combined, request.source.name,
                    (inputPath.is_absolute() ? inputPath : virtualRoot / inputPath).parent_path(), preprocess);
                auto originalCursors = cursors;
                originalCursors.push_back({request.source.name, combined});
                for (const auto& source : request.context.includes) originalCursors.push_back({source.name, source.text});
                for (auto& diagnostic : expanded.diagnostics) if (diagnostic.origin) {
                    const auto found = aliases.find(Utf8Path(diagnostic.origin->source));
                    if (found != aliases.end()) diagnostic.origin->source = std::filesystem::path(
                        std::u8string(found->second.begin(), found->second.end()));
                }
                AppendJassDiagnostics(result, expanded.diagnostics, originalCursors, request, primaryLine, primaryLines, actualMap, budget);
                if (!expanded.Succeeded()) {
                    result.completed = expanded.completed; result.reason = "内置预处理未通过或遇到不支持的语法；未形成检查通过证据。";
                    return result;
                }
                actualMap = ComposeMain(expanded.sourceMap, actualMap, request.source.name, Lines(expanded.text, budget), budget, &aliases);
                sameInputBytes = sameInputBytes && expanded.text == combined;
                preprocessed = std::move(expanded.text); vjassInput = preprocessed;
            }
            Jass::VJassOptions vjass;
            vjass.baseScripts = sources; vjass.check = options; vjass.debug = request.debug;
            auto expanded = Jass::TranslateVJass(vjassInput, request.source.name, vjass);
            auto originalCursors = cursors;
            originalCursors.push_back({request.source.name, vjassInput});
            AppendJassDiagnostics(result, expanded.diagnostics, originalCursors, request, primaryLine,
                primaryLines, actualMap, budget);
            if (!expanded.Succeeded()) {
                result.completed = expanded.completed;
                // 已完成的真实vJass语法错误仍是“检查已执行”；不支持的语法不授予完整检查资格。
                result.syntaxChecked = expanded.completed && expanded.unsupported.empty();
                result.cancelled = actualCancel.stop_requested();
                result.reason = expanded.unsupported.empty() ? "vJass展开没有通过，未形成检查通过证据。" : expanded.unsupported;
                if (!expanded.unsupported.empty()) result.diagnostics.push_back({DiagnosticSeverity::Error,
                    request.source.name, "CODE_CHECK_VJASS_UNSUPPORTED", expanded.unsupported});
                return result;
            }
            emissionOrigins = std::move(expanded);
            emissionInputMap = std::move(actualMap);
            actualMap = ComposeMain(emissionOrigins.sourceMap, emissionInputMap, request.source.name,
                Lines(emissionOrigins.text, budget), budget);
        }
        const std::string_view input = request.language == CodeLanguage::VJass ? emissionOrigins.text : combined;
        sources.push_back({request.source.name, input});
        cursors.push_back({request.source.name, input});
        auto checked = Jass::CheckJass(sources, options);
        result.completed = checked.completed;
        result.syntaxChecked = checked.completed;
        result.semanticChecked = checked.semanticChecked;
        result.cancelled = std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "JASS_CHECK_CANCELLED"; });
        result.reason = request.context.complete ? "已按冻结声明执行共同 JASS 检查。"
            : request.context.reason.empty() ? "声明上下文不完整，仅检查语法。" : request.context.reason;
        AppendJassDiagnostics(result, checked.diagnostics, cursors, request, primaryLine, primaryLines, actualMap, budget,
            request.language == CodeLanguage::VJass ? &emissionOrigins : nullptr,
            request.language == CodeLanguage::VJass ? &emissionInputMap : nullptr, &checked.diagnosticOriginExact);
        if (checked.completed) {
            result.semanticIndex.parsed = std::move(checked.symbols);
            MapSemanticIndex(result, request, actualMap, request.language == CodeLanguage::VJass ? &emissionOrigins : nullptr,
                request.language == CodeLanguage::VJass ? &emissionInputMap : nullptr, sameInputBytes,
                primaryLine, primaryLines, budget);
            if (checked.semanticChecked) {
                result.declarations.completed = result.semanticIndex.parsed.completed;
                result.declarations.reason = result.semanticIndex.parsed.reason;
                for (std::size_t i = 0; i < result.semanticIndex.parsed.definitions.size(); ++i) {
                    budget.Poll(); const auto& definition = result.semanticIndex.parsed.definitions[i];
                    if (definition.kind == Jass::SymbolKind::Parameter || definition.kind == Jass::SymbolKind::Local) continue;
                    const auto& position = result.semanticIndex.definitions[i]; Jass::DeclarationSymbol symbol;
                    symbol.kind = definition.kind == Jass::SymbolKind::Type ? Jass::DeclarationKind::Type
                        : definition.kind == Jass::SymbolKind::Global ? Jass::DeclarationKind::Global
                        : definition.kind == Jass::SymbolKind::Native ? Jass::DeclarationKind::Native : Jass::DeclarationKind::Function;
                    symbol.name = definition.name; symbol.type = definition.type; symbol.array = definition.array; symbol.constant = definition.constant;
                    symbol.source = result.semanticIndex.sources[position.file]; symbol.line = position.line; symbol.byteColumn = position.byteColumn;
                    for (const auto& parameter : definition.parameters) symbol.parameters.push_back({parameter.type, parameter.name});
                    result.declarations.symbols.push_back(std::move(symbol));
                }
            } else result.declarations = Jass::IndexJassDeclarations(sources, options);
            for (auto& symbol : result.declarations.symbols) {
                budget.Poll();
                if (!checked.semanticChecked && symbol.source == request.source.name && !actualMap.Empty()) {
                    if (const auto origin = actualMap.Find(symbol.line); origin && origin->file < actualMap.Files().size()) {
                        symbol.source = actualMap.Files()[origin->file]; symbol.line = origin->line; symbol.byteColumn = 1;
                    }
                }
            }
            result.externalSources = request.context.declarations;
            result.externalSources.insert(result.externalSources.end(), request.context.includes.begin(), request.context.includes.end());
        }
    } catch (const Stopped&) {
        result.cancelled = actualCancel.stop_requested();
        Fail(result, result.cancelled ? "CODE_CHECK_CANCELLED" : "CODE_CHECK_DEADLINE",
            result.cancelled ? "代码检查已取消，未形成通过证据。" : "代码检查超过时间预算，未形成通过证据。");
    } catch (const std::exception&) {
        Fail(result, "CODE_CHECK_EXCEPTION", "代码检查未能完整执行，未形成通过证据。");
    }
    return result;
}
} // namespace War3Map::Services
