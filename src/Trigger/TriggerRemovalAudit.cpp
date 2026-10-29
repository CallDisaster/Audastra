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
constexpr std::size_t kBytes = 64 * 1024 * 1024, kBlocks = 16384, kEvidence = 4096;
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
struct Token { std::string text; std::size_t offset; bool quoted = false; std::size_t length = 0; };
void LocateRawEvidence(RemovalCodeBlock& block, const RawText& source) {
    if (block.evidence.empty()) return;
    const auto original = source.ToBytes(), encoded = RawText{source.text, source.encoding, {}}.ToBytes();
    if (!original.Succeeded() || !encoded.Succeeded() || *original.value != *encoded.value) { block.complete = false; return; }
    const auto prefix = RawText{"", source.encoding, {}}.ToBytes();
    if (!prefix.Succeeded()) { block.complete = false; return; }
    const auto bom = prefix.value->size();
    std::vector<std::pair<std::size_t, std::size_t>> points{{0, bom}};
    for (const auto& hit : block.evidence) {
        if (hit.decodedByteOffset > source.text.size() || hit.decodedByteLength > source.text.size() - hit.decodedByteOffset) { block.complete = false; return; }
        points.emplace_back(hit.decodedByteOffset, 0); points.emplace_back(hit.decodedByteOffset + hit.decodedByteLength, 0);
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), points.end());
    std::size_t previous = 0, raw = bom;
    for (auto& point : points) {
        const auto segment = RawText{source.text.substr(previous, point.first - previous), source.encoding, {}}.ToBytes();
        if (!segment.Succeeded() || segment.value->size() < bom) { block.complete = false; return; }
        raw += segment.value->size() - bom; previous = point.first; point.second = raw;
    }
    const auto at = [&](std::size_t offset) { return std::lower_bound(points.begin(), points.end(), offset, [](const auto& p, auto n) { return p.first < n; })->second; };
    for (auto& hit : block.evidence) { hit.rawByteOffset = at(hit.decodedByteOffset); hit.rawByteLength = at(hit.decodedByteOffset + hit.decodedByteLength) - hit.rawByteOffset; hit.rawRangeExact = true; }
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
    if (text.find('\0') != std::string::npos) { block.complete = false; Evidence(block, source, text.find('\0'), 1, "不支持的NUL字节", false); }
    for (std::size_t i = 0; i < text.size();) {
        if (!tokensLeft) { block.complete = false; break; }
        const auto begin = i;
        if (std::isspace(static_cast<unsigned char>(text[i]))) { ++i; continue; }
        if (text[i] == '#' || (text[i] == '/' && i + 2 < text.size() && text[i + 1] == '/' && text[i + 2] == '!')) {
            block.complete = false; Evidence(block, source, i, 1, "未展开指令/外部来源", false);
        }
        if (text[i] == '/' && i + 1 < text.size() && (text[i + 1] == '/' || text[i + 1] == '*')) {
            const bool line = text[i + 1] == '/'; i += 2;
            if (line) { while (i < text.size() && text[i] != '\n') ++i; }
            else { const auto end = text.find("*/", i); if (end == std::string::npos) { block.complete = false; i = text.size(); } else i = end + 2; }
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
                    else { block.complete = false; decoded += '\\'; decoded += escaped; }
                } else decoded += c;
            }
            if (!closed) block.complete = false;
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
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].quoted) continue;
        const auto& word = tokens[i].text;
        if (word == "load" || word == "loadstring" || word == "ExecuteString") {
            block.dynamic = true; block.complete = false;
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
            std::size_t depth = 1, first = i + 2; bool closed = false;
            for (std::size_t cursor = first; cursor < tokens.size(); ++cursor) {
                if (!tokensLeft) { block.complete = false; break; } --tokensLeft;
                const auto& token = tokens[cursor];
                if (!token.quoted && token.text == "(") ++depth;
                if (!token.quoted && token.text == ")") --depth;
                if (!depth || (depth == 1 && !token.quoted && token.text == ",")) {
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
                    if (!depth) { closed = true; break; }
                }
            }
            if (!closed) block.complete = false;
        } else Evidence(block, source, tokens[i].offset, word.size(), "按名入口引用（须单独确认）", false);
    }
    std::stable_sort(block.evidence.begin(), block.evidence.end(), [](const auto& a, const auto& b) { return a.decodedByteOffset < b.decodedByteOffset; });
    std::size_t at = 0, line = 1, column = 1;
    for (auto& hit : block.evidence) {
        while (at < hit.decodedByteOffset) {
            if (text[at] == '\n') { ++line; column = 1; }
            else if ((static_cast<unsigned char>(text[at]) & 0xc0) != 0x80) ++column;
            ++at;
        }
        hit.line = line; hit.column = column;
    }
    block.groupable = block.complete && !block.dynamic && block.evidence.empty();
}
}
Result<DeclarationRemovalReview> QueryDeclarationRemovalAudit(const TriggerDocument& doc, const UiCatalog& catalog,
    const GraphRemove& op, const MapObjectReferenceSnapshot* objects) {
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
    std::size_t bytes = 0, count = 0, tokensLeft = 262144, findings = 0;
    std::string manifest = review.binding.currentWtgSha256 + review.binding.currentWctSha256 + catalog.Sha256()
        + std::to_string(static_cast<unsigned>(op.target.kind)) + ":" + std::to_string(op.target.id) + ":" + op.expectedName;
    const auto status = detail::VisitOpaqueCodeBlocks(doc, catalog, review.symbols, [&](auto kind, StableId id, const RawText& text) {
        if (text.text.empty()) return Status::Ok();
        if ((kind == AtomicRenameCodeKind::ArgumentCode && removedArguments.contains(id))
            || (kind == AtomicRenameCodeKind::VariableInitialValue && op.target.kind == GraphRecordKind::Variable && id == op.target.id)) return Status::Ok();
        const auto size = std::max(text.text.size(), text.originalBytes.size());
        if (size > kBytes - bytes || ++count > kBlocks) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "代码删除审计超出64MiB或块数预算。");
        bytes += size;
        const auto sha = detail::CodeBlockSha256(text); if (!sha.Succeeded()) return Status{sha.diagnostics};
        RemovalCodeBlock block; block.identity = {kind, id, text.encoding, *sha.value, {}};
        Scan(block, text, review, tokensLeft);
        if (kind == AtomicRenameCodeKind::ArgumentCode) {
            const auto found = arguments.find(id);
            if (found != arguments.end()) {
                const auto& arg = *found->second;
                std::optional<std::string> constantName;
                if (arg.type == 0) {
                    const auto preset = FindPreset(catalog, arg.value.text);
                    if (!preset) { block.complete = false; Evidence(block, text, 0, text.text.size(), "目录中缺少预设：" + arg.value.text, false); }
                    else {
                        if (preset->script.size() > kBytes - bytes) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "目录预设展开超出代码审计预算。");
                        bytes += preset->script.size();
                        RemovalCodeBlock expanded;
                        Scan(expanded, RawText{preset->script}, review, tokensLeft);
                        block.complete &= expanded.complete; block.dynamic |= expanded.dynamic;
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
        findings += block.evidence.size();
        if (findings > 16384) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "引用证据总量超出预算，未提供截断放行结果。");
        const auto decodedSha = Sha256Text(text.text); if (!decodedSha.Succeeded()) return Status{decodedSha.diagnostics};
        std::string evidence = *decodedSha.value + "\n" + std::to_string(block.complete) + std::to_string(block.dynamic);
        for (const auto& hit : block.evidence) evidence += "\n" + hit.context + ":" + std::to_string(hit.decodedByteOffset) + ":" + std::to_string(hit.definite);
        const auto digest = Sha256Text(evidence); if (!digest.Succeeded()) return Status{digest.diagnostics};
        block.identity.evidenceSha256 = *digest.value;
        manifest += "\n" + Key(kind, id) + ":" + *sha.value + ":" + *digest.value + ":" + std::to_string(static_cast<unsigned>(text.encoding));
        review.complete &= block.complete;
        (block.groupable ? review.groupableBlocks : review.individualBlocks).push_back(std::move(block));
        return Status::Ok();
    });
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    const auto digest = Sha256Text(manifest); if (!digest.Succeeded()) return {std::nullopt, digest.diagnostics};
    review.reviewSha256 = *digest.value; return Out::Success(std::move(review));
}
Status CheckDeclarationRemovalAudit(const TriggerDocument& doc, const UiCatalog& catalog, const GraphRemove& op,
    const MapObjectReferenceSnapshot* objects) {
    const auto review = QueryDeclarationRemovalAudit(doc, catalog, op, objects);
    if (!review.Succeeded()) return {review.diagnostics};
    const auto& r = *review.value;
    if (!r.typedArgumentIds.empty()) return Fail("TRIGGER_GRAPH_REMOVE_REFERENCED", "声明仍有存活typed参数引用，须先移除或改绑。");
    for (const auto& block : r.individualBlocks) if (block.referenced)
        return Fail("TRIGGER_GRAPH_REMOVE_REFERENCED", "代码仍引用删除目标，请先移除或改绑；块=" + Key(block.identity.kind, block.identity.id));
    if (!r.complete) return Fail("TRIGGER_GRAPH_REMOVE_REFERENCE_UNKNOWN", "未解析的执行代码、外部指令或截断使引用证据不完整，未删除。");
    if (op.codeAudits.size() > kBlocks) return Fail("TRIGGER_REMOVE_CODE_AUDIT_BUDGET", "确认条数超过预算。");
    std::set<std::string> consumed;
    for (const auto& audit : op.codeAudits) {
        if (audit.reviewSha256 != r.reviewSha256) return Fail("TRIGGER_REMOVE_CODE_AUDIT_STALE", "删除审计的目标、修订、代码或证据已变化。");
        if (audit.decision != "no_reference" || audit.reason.empty() || audit.reason.size() > 4096 || audit.reason.find('\0') != std::string::npos
            || audit.blocks.empty() || audit.blocks.size() > kBlocks) return Fail("TRIGGER_REMOVE_CODE_AUDIT_INVALID", "确认必须逐块绑定身份、no_reference及审阅理由。");
        for (const auto& identity : audit.blocks) {
            const auto find = [&](const auto& blocks) { return std::find_if(blocks.begin(), blocks.end(), [&](const auto& b) { return b.identity == identity; }); };
            const bool groupable = find(r.groupableBlocks) != r.groupableBlocks.end();
            if (!groupable && find(r.individualBlocks) == r.individualBlocks.end()) return Fail("TRIGGER_REMOVE_CODE_AUDIT_STALE", "确认中包含不匹配或旧代码块。");
            if ((!groupable && audit.blocks.size() != 1) || !consumed.insert(Key(identity.kind, identity.id)).second)
                return Fail("TRIGGER_REMOVE_CODE_AUDIT_INVALID", "有命中或动态构造的块须单独确认；不得重复确认。");
        }
    }
    if (consumed.size() != r.groupableBlocks.size() + r.individualBlocks.size())
        return Fail("TRIGGER_REMOVE_CODE_AUDIT_REQUIRED", "请审阅所有代码块：零命中且无动态构造的块可合并，其余须单独确认。");
    return Status::Ok();
}
}
