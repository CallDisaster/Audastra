#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Hash.h"
#include "TriggerCodeAuditBlocks.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Trigger {
namespace {
constexpr std::size_t kBytes = 64 * 1024 * 1024, kBlocks = 262144, kIndividuals = 16384, kEvidence = 4096;
Status Fail(const char* code, std::string message) { return Status::Failure("Trigger.RemoveAudit", code, std::move(message)); }
std::string Key(AtomicRenameCodeKind kind, StableId id) { return std::to_string(static_cast<unsigned>(kind)) + ":" + std::to_string(id); }
bool NameByte(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 128; }
bool Match(std::string_view value, const DeclarationRemovalReview& review, bool identifier) {
    if (!identifier && value == review.expectedName) return true;
    return std::any_of(review.symbols.begin(), review.symbols.end(), [&](const auto& s) {
        return s.ends_with('_') ? value.starts_with(s) : value == s;
    });
}
bool Mentions(std::string_view value, const DeclarationRemovalReview& review) {
    return value.find(review.expectedName) != std::string_view::npos
        || std::any_of(review.symbols.begin(), review.symbols.end(), [&](const auto& s) { return value.find(s) != std::string_view::npos; });
}
bool MacroMentionsSymbol(std::string_view body, const DeclarationRemovalReview& review) {
    for (std::size_t i = 0; i < body.size();) {
        if (!NameByte(static_cast<unsigned char>(body[i]))) { ++i; continue; }
        const auto begin = i;
        while (i < body.size() && NameByte(static_cast<unsigned char>(body[i]))) ++i;
        if (Match(body.substr(begin, i - begin), review, true)) return true;
    }
    return false;
}
struct Token { std::string text; std::size_t offset; bool quoted = false; std::size_t length = 0; };
bool NeedsPreprocessing(std::string_view text) {
    if (text.find("<?") != std::string_view::npos) return true;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '#' || text.substr(i).starts_with("//!")) return true;
        if (text.substr(i).starts_with("//")) { while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i; }
        else if (text.substr(i).starts_with("/*")) {
            const auto end = text.find("*/", i + 2); i = end == std::string_view::npos ? text.size() : end + 2;
        } else if (text[i] == '"' || text[i] == '\'') {
            const auto quote = text[i++];
            while (i < text.size()) { if (text[i] == '\\' && i + 1 < text.size()) i += 2; else if (text[i++] == quote) break; }
        } else ++i;
    }
    return false;
}
std::vector<std::size_t> ArgumentBoundaries(const std::vector<Token>& tokens) {
    std::vector<std::size_t> next(tokens.size(), tokens.size()), tails;
    // 每个逗号只归属当前括号层，嵌套按名调用不重复遍历内部表达式。
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].quoted) continue;
        if (tokens[i].text == "(") tails.push_back(i);
        else if (!tails.empty() && (tokens[i].text == "," || tokens[i].text == ")")) {
            next[tails.back()] = i;
            if (tokens[i].text == ")") tails.pop_back(); else tails.back() = i;
        }
    }
    return next;
}
void Incomplete(RemovalCodeBlock& block, const char* reason) { block.complete = false; if (block.incompleteReason.empty()) block.incompleteReason = reason; }
void LocateRawEvidence(RemovalCodeBlock& block, const RawText& source) {
    if (block.evidence.empty()) return;
    const auto original = source.ToBytes(), encoded = RawText{source.text, source.encoding, {}}.ToBytes();
    if (!original.Succeeded() || !encoded.Succeeded() || *original.value != *encoded.value) { Incomplete(block, "原字节不能按声明编码精确往返"); return; }
    const auto prefix = RawText{"", source.encoding, {}}.ToBytes();
    if (!prefix.Succeeded()) { Incomplete(block, "无法核对原编码前缀"); return; }
    const auto bom = prefix.value->size();
    std::vector<std::pair<std::size_t, std::size_t>> points{{0, bom}};
    for (const auto& hit : block.evidence) {
        if (!hit.sourceFile.empty()) continue;
        if (hit.decodedByteOffset > source.text.size() || hit.decodedByteLength > source.text.size() - hit.decodedByteOffset) { Incomplete(block, "证据位置越界"); return; }
        points.emplace_back(hit.decodedByteOffset, 0); points.emplace_back(hit.decodedByteOffset + hit.decodedByteLength, 0);
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), points.end());
    std::size_t previous = 0, raw = bom;
    for (auto& point : points) {
        const auto segment = RawText{source.text.substr(previous, point.first - previous), source.encoding, {}}.ToBytes();
        if (!segment.Succeeded() || segment.value->size() < bom) { Incomplete(block, "证据边界不能按原编码精确定位"); return; }
        raw += segment.value->size() - bom; previous = point.first; point.second = raw;
    }
    const auto at = [&](std::size_t offset) { return std::lower_bound(points.begin(), points.end(), offset, [](const auto& p, auto n) { return p.first < n; })->second; };
    for (auto& hit : block.evidence) if (hit.sourceFile.empty()) { hit.rawByteOffset = at(hit.decodedByteOffset); hit.rawByteLength = at(hit.decodedByteOffset + hit.decodedByteLength) - hit.rawByteOffset; hit.rawRangeExact = true; }
}
void Evidence(RemovalCodeBlock& block, const RawText& source, std::size_t at, std::size_t size,
    std::string context, bool definite) {
    if (block.evidence.size() >= kEvidence) { block.complete = false; return; }
    RemovalReferenceEvidence hit; hit.context = std::move(context); hit.definite = definite;
    hit.decodedByteOffset = at; hit.decodedByteLength = size;
    auto snippet = std::min<std::size_t>(size, 256);
    while (snippet && at + snippet < source.text.size() && (static_cast<unsigned char>(source.text[at + snippet]) & 0xc0) == 0x80) --snippet;
    hit.text = source.text.substr(at, snippet);
    block.referenced |= definite; block.evidence.push_back(std::move(hit));
}
// 只作有界词法证据，不执行宏或猜测动态目标。未知指令/未闭合文本不取得完整覆盖。
void Scan(RemovalCodeBlock& block, const RawText& source, const DeclarationRemovalReview& review, std::size_t& tokensLeft) {
    const auto& text = source.text; std::vector<Token> tokens;
    if (text.find("<?") != std::string::npos) Incomplete(block, "本轮未覆盖Lua模板");
    if (text.find('\0') != std::string::npos) { Incomplete(block, "含NUL字节"); Evidence(block, source, text.find('\0'), 1, "不支持的NUL字节", false); }
    for (std::size_t i = 0; i < text.size();) {
        if (!tokensLeft) { Incomplete(block, "词法扫描预算截断"); break; }
        const auto begin = i;
        if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (text[i] == '#' || (text[i] == '/' && i + 2 < text.size() && text[i + 1] == '/' && text[i + 2] == '!')) {
            Incomplete(block, "含未展开指令/外部来源"); Evidence(block, source, i, 1, "未展开指令/外部来源", false);
        }
        if (text[i] == '/' && i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*')) {
            const bool line = text[i + 1] == '/'; i += 2;
            if (line) { while (i < text.size() && text[i] != '\n' && text[i] != '\r') ++i; }
            else { const auto end = text.find("*/", i); if (end == std::string::npos) { Incomplete(block, "块注释未闭合"); i = text.size(); } else i = end + 2; }
            const auto comment = std::string_view(text).substr(begin, i - begin);
            if (Mentions(comment, review))
                Evidence(block, source, begin, i - begin, "注释命中（非执行引用）", false);
            continue;
        }
        if (text[i] == '"' || text[i] == '\'') {
            const char quote = text[i++]; std::string decoded; bool closed = false;
            while (i < text.size()) {
                const char c = text[i++]; if (c == quote) { closed = true; break; }
                if (c == '\\' && i < text.size()) {
                    const auto escaped = text[i++];
                    if (escaped == '"' || escaped == '\\' || escaped == '\'') decoded += escaped;
                    else if (escaped == 'n') decoded += '\n';
                    else if (escaped == 'r') decoded += '\r';
                    else if (escaped == 't') decoded += '\t';
                    else { Incomplete(block, "含未知字符串转义"); decoded += '\\'; decoded += escaped; }
                } else decoded += c;
            }
            if (!closed) Incomplete(block, "字符串或物编字面量未闭合");
            --tokensLeft; tokens.push_back({decoded, begin, true, i - begin});
            if (quote == '"' && Mentions(decoded, review)) Evidence(block, source, begin, i - begin, "名字字符串（须审阅用途）", false);
            continue;
        }
        if (NameByte(static_cast<unsigned char>(text[i]))) {
            while (i < text.size() && NameByte(static_cast<unsigned char>(text[i]))) ++i;
            auto value = text.substr(begin, i - begin); --tokensLeft; tokens.push_back({value, begin, false, i - begin});
            if (Match(value, review, true)) Evidence(block, source, begin, i - begin, "可执行生成符号", true);
        } else { --tokensLeft; tokens.push_back({text.substr(i, 1), i, false, 1}); ++i; }
    }
    const auto boundaries = ArgumentBoundaries(tokens);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].quoted) continue;
        const auto& word = tokens[i].text;
        if (word == "load" || word == "loadstring" || word == "ExecuteString") {
            block.dynamic = true; Incomplete(block, "从字符串执行代码，未覆盖其正文");
            Evidence(block, source, tokens[i].offset, word.size(), "动态环境/脚本构造未覆盖", false);
        }
        if (word == "_G" || word == "_ENV") {
            block.dynamic = true; Evidence(block, source, tokens[i].offset, word.size(), "动态环境按名访问（须单独确认）", false);
        }
        std::string lower = word;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool call = i + 1 < tokens.size() && tokens[i + 1].text == "(";
        const bool dispatch = word == "ExecuteFunc" || (call && (lower.find("byname") != std::string::npos
            || lower.find("bystring") != std::string::npos || lower.find("lookup") != std::string::npos));
        if (!dispatch) continue;
        block.dynamic = true; Evidence(block, source, tokens[i].offset, word.size(), "按名/动态调用位置", false);
        if (i + 2 < tokens.size() && tokens[i + 1].text == "(") {
            std::size_t first = i + 2; bool closed = false;
            for (auto cursor = boundaries[i + 1]; cursor < tokens.size(); cursor = boundaries[cursor]) {
                if (cursor > first) {
                    const auto& argument = tokens[first];
                    if (cursor == first + 1 && argument.quoted) {
                        if (Match(argument.text, review, false)) Evidence(block, source, argument.offset, argument.length, "常量按名目标引用", true);
                    } else {
                        const auto end = tokens[cursor - 1].offset + tokens[cursor - 1].length;
                        Evidence(block, source, argument.offset, end - argument.offset, "动态调用目标表达式（须单独确认）", false);
                    }
                }
                first = cursor + 1;
                if (tokens[cursor].text == ")") { closed = true; break; }
            }
            if (!closed) Incomplete(block, "按名调用表达式未闭合或预算不足");
        } else Evidence(block, source, tokens[i].offset, word.size(), "按名入口引用（须单独确认）", false);
    }
    std::stable_sort(block.evidence.begin(), block.evidence.end(), [](const auto& a, const auto& b) { return a.decodedByteOffset < b.decodedByteOffset; });
    std::size_t at = 0, line = 1, column = 1;
    for (auto& hit : block.evidence) {
        while (at < hit.decodedByteOffset) {
            if (text[at] == '\r' || (text[at] == '\n' && (!at || text[at - 1] != '\r'))) { ++line; column = 1; }
            else if (text[at] == '\n') {}
            else if ((static_cast<unsigned char>(text[at]) & 0xc0) != 0x80) ++column;
            ++at;
        }
        hit.line = line; hit.column = column;
    }
    block.groupable = block.complete && !block.dynamic && block.evidence.empty();
}
}
Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(const TriggerDocument& doc, const UiCatalog& catalog,
    const GraphRemove& op, const MapObjectReferenceSnapshot* objects, const RemovalPreprocessProvider* preprocessing) {
    using Out = Result<DeclarationRemovalReview>;
    const auto valid = WtgWctCodec{}.Validate(doc, catalog); if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    const auto binding = CaptureTriggerGraphBinding(doc, catalog, objects); if (!binding.Succeeded()) return {std::nullopt, binding.diagnostics};
    DeclarationRemovalReview review; review.binding = *binding.value; review.target = op.target; review.expectedName = op.expectedName;
    if (op.target.kind == GraphRecordKind::Variable) {
        const auto at = std::find_if(doc.variables.begin(), doc.variables.end(), [&](const auto& r) { return r.id == op.target.id; });
        if (at == doc.variables.end() || at->name.text != op.expectedName) return Out::Failure("Trigger.RemoveAudit", "TRIGGER_GRAPH_NAME_STALE", "删除变量的身份或原名已变化。");
        review.symbols = {"udg_" + at->name.text};
    } else if (op.target.kind == GraphRecordKind::Trigger) {
        const auto at = std::find_if(doc.triggers.begin(), doc.triggers.end(), [&](const auto& r) { return r.id == op.target.id; });
        if (at == doc.triggers.end() || at->name.text != op.expectedName) return Out::Failure("Trigger.RemoveAudit", "TRIGGER_GRAPH_NAME_STALE", "删除触发器的身份或原名已变化。");
        const auto name = TriggerDirectoryEditor::ScriptName(at->name);
        review.symbols = {"gg_trg_" + name, "InitTrig_" + name, "Trig_" + name + "_"};
    } else return Out::Failure("Trigger.RemoveAudit", "TRIGGER_GRAPH_REMOVE_UNSUPPORTED", "代码删除审计只支持触发器或变量声明。");
    std::unordered_map<StableId, const ArgumentNode*> arguments;
    std::unordered_set<StableId> removedArguments, byNameArguments;
    const TriggerRecord* removedTrigger = nullptr;
    if (op.target.kind == GraphRecordKind::Trigger)
        removedTrigger = &*std::find_if(doc.triggers.begin(), doc.triggers.end(), [&](const auto& r) { return r.id == op.target.id; });
    for (const auto& arg : doc.argumentNodes) arguments.emplace(arg.id, &arg);
    for (const auto& eca : doc.ecaNodes) {
        const auto* definition = catalog.Find(eca.type, eca.name.text);
        const auto script = definition ? DefinitionField(*definition, "script_name") : std::nullopt;
        const auto alternate = definition ? DefinitionField(*definition, "script") : std::nullopt;
        const bool byName = eca.name.text == "ExecuteFunc" || (script && *script == "ExecuteFunc") || (alternate && *alternate == "ExecuteFunc");
        for (const auto index : eca.arguments) if (index < doc.argumentNodes.size()) {
            const auto& arg = doc.argumentNodes[index];
            if (removedTrigger && eca.sourceTriggerId == op.target.id) {
                auto child = index;
                for (std::size_t depth = 0; depth <= 128 && child < doc.argumentNodes.size(); ++depth) {
                    const auto& nested = doc.argumentNodes[child]; removedArguments.insert(nested.id);
                    if (!nested.indexArgument) break; child = *nested.indexArgument;
                }
            }
            if (byName) byNameArguments.insert(arg.id);
        }
    }
    for (const auto& arg : doc.argumentNodes) if (arg.type == 1 && !removedArguments.contains(arg.id)) {
        bool referenced = op.target.kind == GraphRecordKind::Variable
            ? arg.value.text == "udg_" + op.expectedName || arg.value.text == op.expectedName
            : TriggerDirectoryEditor::IsTriggerSymbolOf(arg.value.text, removedTrigger->name);
        if (referenced) review.typedArgumentIds.push_back(arg.id);
    }
    std::size_t bytes = 0, count = 0, tokensLeft = kBytes, findings = 0;
    std::optional<RemovalPreprocessCoverage> coverage;
    std::unordered_map<std::string, const RemovalPreprocessBlock*> covered;
    bool needsPreprocessing = false;
    if (preprocessing && *preprocessing && review.typedArgumentIds.empty()) {
        const auto probe = detail::VisitOpaqueCodeBlocks(doc, catalog, review.symbols, [&](auto, auto, const RawText& text) {
            needsPreprocessing |= NeedsPreprocessing(text.text); return Status::Ok();
        });
        if (!probe.Succeeded()) return {std::nullopt, probe.diagnostics};
    }
    if (needsPreprocessing && review.typedArgumentIds.empty() && preprocessing && *preprocessing) {
        const auto captured = (*preprocessing)(doc, catalog, op);
        if (!captured.Succeeded()) return {std::nullopt, captured.diagnostics};
        coverage = std::move(*captured.value);
        if (coverage->inputBytes > kBytes || coverage->blocks.size() > kBlocks)
            return Out::Failure("Trigger.RemoveAudit", "TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "预处理来源超过删除审计预算。");
        bytes = coverage->inputBytes;
        review.externalDependenciesVerified = coverage->complete;
        review.preprocessIdentitySha256 = coverage->identitySha256;
        review.preprocessProfileSha256 = coverage->profileSha256;
        review.includedFileCount = coverage->includedFileCount;
        if (coverage->identitySha256.size() != 64 || coverage->profileSha256.size() != 64)
            return Out::Failure("Trigger.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_INVALID", "预处理覆盖缺少来源身份。");
        for (const auto& entry : coverage->blocks)
            if (!covered.emplace(Key(entry.kind, entry.id), &entry).second)
                return Out::Failure("Trigger.RemoveAudit", "TRIGGER_REMOVE_PREPROCESS_INVALID", "预处理覆盖代码块身份重复。");
    }
    Sha256Stream manifest(256 * kBlocks + 4096), group(256 * kBlocks);
    const auto update = [](Sha256Stream& stream, std::string_view text) {
        return stream.Update(std::as_bytes(std::span(text.data(), text.size())));
    };
    const auto header = update(manifest, "remove-audit-v2:" + review.binding.currentWtgSha256 + review.binding.currentWctSha256 + catalog.Sha256()
        + std::to_string(static_cast<unsigned>(op.target.kind)) + ":" + std::to_string(op.target.id) + ":" + op.expectedName
        + ":preprocess:" + review.preprocessProfileSha256 + ":" + review.preprocessIdentitySha256);
    if (!header.Succeeded()) return {std::nullopt, header.diagnostics};
    const auto status = detail::VisitOpaqueCodeBlocks(doc, catalog, review.symbols, [&](auto kind, StableId id, const RawText& text) {
        if (text.text.empty() && !(coverage && kind == AtomicRenameCodeKind::GlobalCustomCode)) return Status::Ok();
        if ((kind == AtomicRenameCodeKind::ArgumentCode && removedArguments.contains(id))
            || (kind == AtomicRenameCodeKind::VariableInitialValue && op.target.kind == GraphRecordKind::Variable && id == op.target.id)) return Status::Ok();
        const auto size = std::max(text.text.size(), text.originalBytes.size());
        if (size > kBytes - bytes || ++count > kBlocks) {
            std::string message = "删除审计预算不足；当前块=" + Key(kind, id) + "；累计块=" + std::to_string(count)
                + "；累计字节=" + std::to_string(bytes) + "；当前块字节=" + std::to_string(size)
                + "；上限=262144块/64MiB；已完成可合并=" + std::to_string(review.groupableCount)
                + "、单独=" + std::to_string(review.individualBlocks.size()) + "（部分结果，不可放行）";
            const auto arg = arguments.find(id);
            if (kind == AtomicRenameCodeKind::ArgumentCode && arg != arguments.end())
                message += "；参数来源种类=" + std::to_string(arg->second->type);
            return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", std::move(message));
        }
        bytes += size;
        const auto sha = detail::CodeBlockSha256(text); if (!sha.Succeeded()) return Status{sha.diagnostics};
        RemovalCodeBlock block; block.identity = {kind, id, text.encoding, *sha.value, {}};
        const auto expanded = covered.find(Key(kind, id));
        if (coverage && !coverage->complete && kind == AtomicRenameCodeKind::GlobalCustomCode) Incomplete(block, coverage->incompleteReason.c_str());
        if (coverage && coverage->complete && expanded != covered.end()) {
            const auto& entry = *expanded->second;
            if (entry.sourceSha256 != *sha.value) return Fail("TRIGGER_REMOVE_CODE_AUDIT_STALE", "预处理覆盖与当前代码块不同。");
            if (entry.text.size() > kBytes - bytes) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "展开文本超过删除审计预算。");
            bytes += entry.text.size();
            Scan(block, RawText{entry.text}, review, tokensLeft);
            for (auto& hit : block.evidence) {
                if (!hit.line || hit.line > entry.origins.size()) { Incomplete(block, "展开证据缺少来源映射"); continue; }
                const auto& origin = entry.origins[hit.line - 1];
                hit.sourceFile = origin.file; hit.sourceLine = origin.line;
                if (hit.sourceFile.empty() || !hit.sourceLine) Incomplete(block, "展开证据缺少来源映射");
            }
            for (const auto& macro : entry.macros) {
                const bool definite = !macro.expanded && (Match(macro.name, review, true) || MacroMentionsSymbol(macro.body, review));
                if (!definite && !(macro.expanded && macro.tokenConstruction)) continue;
                RemovalReferenceEvidence hit; hit.context = definite ? "宏定义含目标符号" : "宏调用含拼接/字符串化（须单独确认）";
                hit.text = macro.name; hit.sourceFile = macro.file; hit.sourceLine = macro.line;
                if (hit.sourceFile.empty() || !hit.sourceLine) Incomplete(block, "宏证据缺少来源映射");
                hit.definite = definite; block.referenced |= definite;
                block.dynamic |= !definite;
                if (block.evidence.size() >= kEvidence) Incomplete(block, "宏证据数量超过预算");
                else block.evidence.push_back(std::move(hit));
            }
        } else Scan(block, text, review, tokensLeft);
        if (kind == AtomicRenameCodeKind::ArgumentCode) {
            const auto found = arguments.find(id);
            if (found != arguments.end()) {
                const auto& arg = *found->second;
                std::optional<std::string> constantName;
                if (arg.type == 0) {
                    const auto preset = FindPreset(catalog, arg.value.text);
                    if (!preset) { Incomplete(block, "目录中缺少预设"); Evidence(block, text, 0, text.text.size(), "目录中缺少预设：" + arg.value.text, false); }
                    else {
                        if (preset->script.size() > kBytes - bytes) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "目录预设展开超出代码审计预算。");
                        bytes += preset->script.size();
                        RemovalCodeBlock expanded;
                        Scan(expanded, RawText{preset->script}, review, tokensLeft);
                        if (!expanded.complete) { block.complete = false; block.incompleteReason = "目录预设展开：" + expanded.incompleteReason; }
                        block.dynamic |= expanded.dynamic;
                        for (const auto& hit : expanded.evidence)
                            Evidence(block, text, 0, text.text.size(), "目录预设 " + arg.value.text + "：" + hit.context + "：" + hit.text, hit.definite);
                        const auto value = std::string_view(preset->script);
                        if (value.size() >= 2 && value.front() == '"' && value.back() == '"' && value.find('\\') == std::string_view::npos)
                            constantName = std::string(value.substr(1, value.size() - 2));
                    }
                }
                if (byNameArguments.contains(id)) {
                    block.dynamic = true;
                    const bool definite = arg.type == 3 ? Match(text.text, review, false) : constantName && Match(*constantName, review, false);
                    Evidence(block, text, 0, text.text.size(), arg.type == 3 || constantName ? "typed按名调用常量参数" : "typed按名动态参数（须单独确认）", definite);
                }
            }
        }
        block.groupable = block.complete && !block.dynamic && block.evidence.empty();
        LocateRawEvidence(block, text);
        if (!block.complete && block.incompleteReason.empty()) block.incompleteReason = "引用证据数量达到上限，未提供截断放行结果";
        findings += block.evidence.size();
        if (findings > 16384) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "引用证据总量超出预算，未提供截断放行结果。");
        const auto decodedSha = Sha256Text(text.text); if (!decodedSha.Succeeded()) return Status{decodedSha.diagnostics};
        std::string evidence = *decodedSha.value + "\n" + std::to_string(block.complete) + std::to_string(block.dynamic);
        for (const auto& hit : block.evidence) evidence += "\n" + hit.context + ":" + std::to_string(hit.decodedByteOffset) + ":" + std::to_string(hit.definite)
            + ":" + hit.sourceFile + ":" + std::to_string(hit.sourceLine);
        const auto digest = Sha256Text(evidence); if (!digest.Succeeded()) return Status{digest.diagnostics};
        block.identity.evidenceSha256 = *digest.value;
        const auto row = Key(kind, id) + ":" + *sha.value + ":" + *digest.value + ":" + std::to_string(static_cast<unsigned>(text.encoding)) + "\n";
        if (const auto added = update(manifest, row); !added.Succeeded()) return added;
        review.complete &= block.complete;
        const auto sample = [&] {
            auto length = std::min<std::size_t>(text.text.size(), 256);
            while (length && length < text.text.size() && (static_cast<unsigned char>(text.text[length]) & 0xc0) == 0x80) --length;
            block.sampleText = text.text.substr(0, length); block.sampleTruncated = length < text.text.size();
        };
        if (block.groupable) {
            if (const auto added = update(group, row); !added.Succeeded()) return added;
            ++review.groupableCount; review.groupableIdentities_.push_back(block.identity);
            if (review.groupableSamples.size() < 20) {
                sample();
                review.groupableSamples.push_back(std::move(block));
            }
        } else {
            if (review.individualBlocks.size() >= kIndividuals) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "须单独确认的代码块超过16384块，未提供部分放行。");
            sample();
            review.individualBlocks.push_back(std::move(block));
        }
        return Status::Ok();
    });
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    const auto grouped = group.Finish(); if (!grouped.Succeeded()) return {std::nullopt, grouped.diagnostics};
    review.groupableSha256 = *grouped.value;
    const auto digest = manifest.Finish(); if (!digest.Succeeded()) return {std::nullopt, digest.diagnostics};
    review.reviewSha256 = *digest.value; return Out::Success(std::move(review));
}
Status CheckDeclarationRemovalAudit(const TriggerDocument& doc, const UiCatalog& catalog, const GraphRemove& op,
    const MapObjectReferenceSnapshot* objects, const RemovalPreprocessProvider* preprocessing) {
    const auto review = QueryDeclarationRemovalAudit(doc, catalog, op, objects, preprocessing);
    if (!review.Succeeded()) return {review.diagnostics};
    const auto& r = *review.value;
    if (!r.typedArgumentIds.empty()) return Fail("TRIGGER_GRAPH_REMOVE_REFERENCED", "声明仍有存活typed参数引用，须先移除或改绑。");
    for (const auto& block : r.individualBlocks) if (block.referenced)
        return Fail("TRIGGER_GRAPH_REMOVE_REFERENCED", "代码仍引用删除目标，请先移除或改绑；块=" + Key(block.identity.kind, block.identity.id));
    if (!r.complete) return Fail("TRIGGER_GRAPH_REMOVE_REFERENCE_UNKNOWN", "未解析的执行代码、外部指令或截断使引用证据不完整，未删除。");
    if (op.codeAudits.size() > kIndividuals + 1) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "确认条数超过预算。");
    struct Entry { const RemovalBlockIdentity* identity; bool groupable; };
    std::unordered_map<std::string, Entry> index;
    index.reserve(r.groupableCount + r.individualBlocks.size());
    for (const auto& identity : r.groupableIdentities_) index.emplace(Key(identity.kind, identity.id), Entry{&identity, true});
    for (const auto& block : r.individualBlocks) index.emplace(Key(block.identity.kind, block.identity.id), Entry{&block.identity, false});
    std::unordered_set<std::string> consumed;
    consumed.reserve(index.size());
    bool wholeGroup = false, legacyGroup = false;
    std::size_t consumedGroup = 0, consumedIndividual = 0;
    for (const auto& audit : op.codeAudits) {
        if (audit.reviewSha256 != r.reviewSha256) return Fail("TRIGGER_REMOVE_CODE_AUDIT_STALE", "删除审计的目标、修订、代码或证据已变化。");
        if (audit.decision != "no_reference" || audit.reason.empty() || audit.reason.size() > 4096 || audit.reason.find('\0') != std::string::npos
            || (!audit.group && audit.blocks.empty()) || audit.blocks.size() > kIndividuals)
            return Fail("TRIGGER_REMOVE_CODE_AUDIT_INVALID", "确认必须绑定整组或逐块身份、no_reference及审阅理由。");
        if (audit.group) {
            if (!audit.blocks.empty() || wholeGroup || legacyGroup || !audit.group->count)
                return Fail("TRIGGER_REMOVE_CODE_AUDIT_INVALID", "整组确认不得与可合并块逐块确认混用或重复。");
            if (audit.group->count != r.groupableCount || audit.group->sha256 != r.groupableSha256)
                return Fail("TRIGGER_REMOVE_CODE_AUDIT_STALE", "可合并代码整组数量或摘要已变化。");
            wholeGroup = true;
            continue;
        }
        for (const auto& identity : audit.blocks) {
            const auto key = Key(identity.kind, identity.id);
            const auto found = index.find(key);
            if (found == index.end() || *found->second.identity != identity) return Fail("TRIGGER_REMOVE_CODE_AUDIT_STALE", "确认中包含不匹配或旧代码块。");
            const bool groupable = found->second.groupable;
            if ((!groupable && audit.blocks.size() != 1) || !consumed.insert(key).second || (groupable && wholeGroup))
                return Fail("TRIGGER_REMOVE_CODE_AUDIT_INVALID", "有命中或动态构造的块须单独确认；不得重复确认。");
            if (groupable) { legacyGroup = true; ++consumedGroup; } else ++consumedIndividual;
        }
    }
    if ((!wholeGroup && consumedGroup != r.groupableCount) || consumedIndividual != r.individualBlocks.size())
        return Fail("TRIGGER_REMOVE_CODE_AUDIT_REQUIRED", "请审阅所有代码块：零命中且无动态构造的块可合并，其余须单独确认。");
    return Status::Ok();
}
}
