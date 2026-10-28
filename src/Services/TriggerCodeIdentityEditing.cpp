#include "War3MapCore/Services/TriggerCodeIdentityEditing.h"
#include "War3MapCore/Jass/IdentifierRewrite.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"
#include <algorithm>
#include <map>
#include <set>

namespace War3Map::Services {
namespace {
using namespace Trigger;
using R = Result<PreparedTriggerCodeIdentityEdit>;
Status Bad(std::string code, std::string text) { return Status::Failure("Trigger.CodeIdentity", "TRIGGER_CODE_" + code, std::move(text)); }
Status Context(const TriggerCodeIdentityContext& c) {
    if (!c.document || !c.catalog || !c.document->HasTrustedSourceSeal()) return Bad("SOURCE_STALE", "需要完整可信WTG/WCT和函数目录来源。");
    if (c.target != TriggerCodeScriptTarget::Jass) return Bad("LUA_UNSUPPORTED", "本阶段只支持明确的JASS目标；Lua或未知目标不能按JASS规则猜测。");
    if (!c.completeScriptSources || !c.opaqueScriptSources.empty()) {
        std::string text = "脚本来源覆盖不完整，不能证明定义和引用；未确定来源：";
        for (const auto& source : c.opaqueScriptSources) { if (text.size() > 4096) break; text += source + "；"; }
        return Bad("COVERAGE_INCOMPLETE", std::move(text));
    }
    return c.verify ? c.verify() : Status::Ok();
}
bool Same(const RawText& a, const RawText& b) {
    const auto x = a.ToBytes(), y = b.ToBytes();
    return a.text == b.text && a.encoding == b.encoding && a.originalBytes == b.originalBytes
        && x.Succeeded() && y.Succeeded() && *x.value == *y.value;
}
bool Same(const TriggerGraphBinding& a, const TriggerGraphBinding& b) {
    return a.sourceWtgSha256 == b.sourceWtgSha256 && a.sourceWctSha256 == b.sourceWctSha256
        && a.currentWtgSha256 == b.currentWtgSha256 && a.currentWctSha256 == b.currentWctSha256
        && a.uiCatalogSha256 == b.uiCatalogSha256 && a.mapObjectReferencesSha256 == b.mapObjectReferencesSha256;
}
struct Block {
    enum class Kind { Global, Trigger, Argument } kind;
    StableId id = 0;
    RawText text;
    std::string label;
    Jass::ExactIdentifierResult facts;
};
std::string Position(const Block& block, std::size_t offset) {
    const auto bytes = block.text.ToBytes();
    const std::string raw = bytes.Succeeded() ? std::string(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size()) : block.text.text;
    offset = std::min(offset, raw.size());
    std::size_t line = 1, start = 0;
    for (std::size_t p = 0; p < offset; ++p) if (raw[p] == '\r' || raw[p] == '\n') {
        if (raw[p] == '\r' && p + 1 < offset && raw[p + 1] == '\n') ++p;
        ++line; start = p + 1;
    }
    auto end = raw.find_first_of("\r\n", start); if (end == raw.npos) end = raw.size();
    const auto snippet = raw.substr(start, std::min<std::size_t>(end - start, 240));
    const auto decoded = DecodeSourceText(std::as_bytes(std::span(snippet.data(), snippet.size())));
    return block.label + " " + std::to_string(line) + ":" + std::to_string(offset - start + 1) + "：" + decoded.text;
}
Result<Jass::ExactIdentifierResult> Rewrite(const Block& block, std::span<const Jass::ExactIdentifierMapping> mappings) {
    using Q = Result<Jass::ExactIdentifierResult>;
    auto bytes = block.text.ToBytes(); if (!bytes.Succeeded()) return {{}, bytes.diagnostics};
    Jass::IdentifierRewriteRequest request;
    request.text = {reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size()};
    if (block.text.encoding == TextEncoding::Utf8 || block.text.encoding == TextEncoding::Utf8Bom) request.encoding = Text::SourceEncoding::Utf8;
    else if (block.text.encoding == TextEncoding::Gb18030) request.encoding = Text::SourceEncoding::Gbk;
    else return Q::Failure("Trigger.CodeIdentity", "TRIGGER_CODE_ENCODING_UNSUPPORTED", block.label + "：代码编码尚不支持严格词法改写。");
    request.sourceName = block.label; request.expectedInputSha256 = *Sha256(*bytes.value).value;
    request.limits.maxInputBytes = 2 * 1024 * 1024; request.limits.maxOutputBytes = 4 * 1024 * 1024;
    auto result = Jass::RewriteExactIdentifiers(request, mappings, block.kind == Block::Kind::Argument);
    for (auto& diagnostic : result.diagnostics) diagnostic.message = Position(block, diagnostic.origin ? diagnostic.origin->offset : 0) + "；" + diagnostic.message;
    return result;
}
RawText Rewritten(const RawText& before, const std::string& raw) {
    auto value = DecodeSourceText(std::as_bytes(std::span(raw.data(), raw.size())));
    value.encoding = before.encoding;
    return value;
}
}
Status TriggerCodeIdentityEditing::VerifyContext(const TriggerCodeIdentityContext& context) { return Context(context); }
Result<PreparedTriggerCodeIdentityEdit> TriggerCodeIdentityEditing::Prepare(
    const TriggerCodeIdentityContext& context, const TriggerCodeIdentityRequest& request) {
    const auto fail = [](Status status) -> R { return {{}, std::move(status.diagnostics)}; };
    auto checked = Context(context); if (!checked.Succeeded()) return fail(checked);
    if (request.action != TriggerCodeIdentityAction::Rename && request.action != TriggerCodeIdentityAction::Copy)
        return fail(Bad("BINDING_UNSUPPORTED", "未知代码身份操作，不能按改名处理。"));
    const auto& source = *context.document; const auto& catalog = *context.catalog;
    checked = WtgWctCodec{}.Validate(source, catalog); if (!checked.Succeeded()) return fail(checked);
    auto binding = CaptureTriggerGraphBinding(source, catalog, context.mapObjects);
    if (!binding.Succeeded()) return {{}, binding.diagnostics};
    if (!Same(*binding.value, request.expected)) return fail(Bad("SOURCE_STALE", "触发器图、来源或Catalog已变化。"));
    const auto* original = TriggerDocumentEditor::FindTrigger(const_cast<TriggerDocument&>(source), request.triggerId);
    if (!original || original->type != 0 || !original->customTextFlag || !original->ecas.empty())
        return fail(Bad("BINDING_UNSUPPORTED", "本入口只编辑无GUI树的既有自定义代码触发器；普通GUI改名保持原路径。"));
    auto bytes = original->customText.ToBytes();
    if (!bytes.Succeeded() || !Same(original->name, request.expectedName) || *Sha256(*bytes.value).value != request.expectedCodeSha256)
        return fail(Bad("SOURCE_STALE", "触发器原名或WCT槽字节已变化。"));
    if (source.triggers.size() > 4096 || source.ecaNodes.size() > 262144 || source.argumentNodes.size() > 262144)
        return fail(Bad("BUDGET_EXCEEDED", "代码身份扫描图规模超过本阶段预算。"));
    if (request.name.empty() || request.name.size() > 4096 || request.name.find('\0') != request.name.npos
        || Text::TrimLineWhitespace(request.name) != request.name)
        return fail(Bad("NAME_INVALID", "新触发器名为空、含NUL或超过4096字节。"));
    RawText newName = original->name; newName.text = request.name; newName.originalBytes.clear();
    if (!newName.ToBytes().Succeeded()) return fail(Bad("ENCODING_UNSUPPORTED", "新触发器名不能按原编码写出。"));
    const auto oldStem = TriggerDirectoryEditor::ScriptName(original->name), newStem = TriggerDirectoryEditor::ScriptName(newName);
    if (newStem.empty()) return fail(Bad("NAME_INVALID", "新名称不能生成有效脚本符号。"));
    const bool copy = request.action == TriggerCodeIdentityAction::Copy;
    auto categoryId = request.categoryId;
    if (copy && request.categoryRecordId) {
        const auto category = std::find_if(source.categories.begin(), source.categories.end(), [&](const auto& c) { return c.id == request.categoryRecordId; });
        if (category == source.categories.end()) return fail(Bad("SOURCE_STALE", "复制目标分类ref.id已不存在。"));
        categoryId = category->categoryId;
    }
    for (const auto& trigger : source.triggers) if (trigger.type == 0 && (copy || trigger.id != original->id)) {
        const auto stem = TriggerDirectoryEditor::ScriptName(trigger.name);
        if ((trigger.id != original->id && stem == oldStem) || stem == newStem || trigger.name.text == request.name)
            return fail(Bad("SYMBOL_CONFLICT", "触发器“" + trigger.name.text + "”的名称或生成符号冲突。"));
    }
    if (copy) {
        std::vector<StableId> order; for (const auto& t : source.triggers) if (t.categoryId == categoryId) order.push_back(t.id);
        if (order != request.expectedCategoryOrder || (request.afterTriggerId && std::find(order.begin(), order.end(), request.afterTriggerId) == order.end()))
            return fail(Bad("SOURCE_STALE", "复制目标类别顺序或定位锚点已变化。"));
    }
    std::vector<Block> blocks;
    if (!source.wct.customCode.text.empty()) blocks.push_back({Block::Kind::Global, 0, source.wct.customCode, "全局WCT", {}});
    for (const auto& trigger : source.triggers) if (!trigger.customText.text.empty())
        blocks.push_back({Block::Kind::Trigger, trigger.id, trigger.customText, "触发器“" + trigger.name.text + "”", {}});
    std::set<StableId> scriptArguments;
    for (const auto& eca : source.ecaNodes) {
        const auto owner = std::find_if(source.triggers.begin(), source.triggers.end(), [&](const auto& trigger) { return trigger.id == eca.sourceTriggerId; });
        const auto ownerLabel = owner == source.triggers.end() ? "触发器（归属未确定）" : "触发器“" + owner->name.text + "”";
        if (eca.name.text == "ExecuteFunc") return fail(Bad("COVERAGE_INCOMPLETE", "触发器ECA含ExecuteFunc动态分发，无法证明改名/复制绑定完整。ECA=" + std::to_string(eca.id)));
        auto open = eca.uiSignature.rfind('('), close = eca.uiSignature.rfind(')');
        if (open == eca.uiSignature.npos || close <= open) return fail(Bad("COVERAGE_INCOMPLETE", "ECA缺少冻结的参数类型签名，不能证明代码参数覆盖。ECA=" + std::to_string(eca.id)));
        auto types = std::string_view(eca.uiSignature).substr(open + 1, close - open - 1); std::size_t at = 0;
        while (!types.empty()) {
            const auto comma = types.find(','); const auto type = types.substr(0, comma);
            if (type != "nothing") {
                if (at >= eca.arguments.size() || eca.arguments[at] >= source.argumentNodes.size()) return fail(Bad("BINDING_UNSUPPORTED", "代码参数槽不完整。"));
                const auto& arg = source.argumentNodes[eca.arguments[at++]];
                if (type == "scriptcode" || eca.name.text == "CustomScriptCode") {
                    if (arg.callEca || arg.indexArgument) return fail(Bad("COVERAGE_INCOMPLETE", "代码参数含调用或数组索引，不能作为完整语句片段改写。"));
                    if (scriptArguments.insert(arg.id).second) blocks.push_back({Block::Kind::Argument, arg.id, arg.value,
                        ownerLabel + " ECA=" + std::to_string(eca.id), {}});
                }
            }
            if (comma == types.npos) break; types.remove_prefix(comma + 1);
        }
    }
    std::size_t totalBytes = 0;
    for (auto& block : blocks) {
        if (block.text.text.size() > 8 * 1024 * 1024 - totalBytes) return fail(Bad("BUDGET_EXCEEDED", "全部代码扫描超过8MiB预算。"));
        totalBytes += block.text.text.size();
        auto inspected = Rewrite(block, {}); if (!inspected.Succeeded()) return {{}, inspected.diagnostics};
        block.facts = std::move(*inspected.value);
    }
    const auto oldInit = "InitTrig_" + oldStem, newInit = "InitTrig_" + newStem;
    const auto oldGlobal = "gg_trg_" + oldStem, newGlobal = "gg_trg_" + newStem;
    const auto prefix = "Trig_" + oldStem + "_";
    std::vector<Jass::ExactIdentifierMapping> mappings{{oldInit, newInit}, {oldGlobal, newGlobal}};
    std::size_t initializers = 0;
    for (const auto& block : blocks) for (const auto& declaration : block.facts.declarations) {
        if (declaration.local) continue;
        const bool own = block.kind == Block::Kind::Trigger && block.id == original->id;
        if (declaration.name == oldGlobal || (declaration.name == oldInit && (!own || !declaration.initializer))
            || (declaration.name.starts_with(prefix) && !own))
            return fail(Bad("SYMBOL_CONFLICT", Position(block, declaration.offset) + "；同前缀声明不属于本触发器，不能猜测归属。"));
        if (declaration.name == oldInit) ++initializers;
        if (own && declaration.name.starts_with(prefix))
            mappings.push_back({declaration.name, "Trig_" + newStem + "_" + declaration.name.substr(prefix.size())});
        else if (copy && own && declaration.name != oldInit)
            return fail(Bad("COPY_DECLARATION_UNSUPPORTED", Position(block, declaration.offset) + "；额外用户声明不能在副本中安全重名，也不隐式改写。"));
    }
    if (initializers != 1) return fail(Bad("BINDING_UNSUPPORTED", "触发器“" + original->name.text + "”须有唯一的无参nothing InitTrig入口。"));
    std::set<std::string> seen;
    for (const auto& mapping : mappings) if (!seen.insert(mapping.before).second) return fail(Bad("SYMBOL_CONFLICT", "自有辅助声明重复：" + mapping.before));
    PreparedTriggerCodeIdentityEdit result; result.binding_ = *binding.value;
    result.summary_ = {original->id, original->id, copy, copy || request.name != original->name.text,
        original->name.text, request.name, oldGlobal, newGlobal, 0, 0, {}};
    TriggerDocument candidate = source;
    RawText copiedCode = original->customText;
    for (const auto& block : blocks) {
        auto rewritten = Rewrite(block, mappings); if (!rewritten.Succeeded()) return {{}, rewritten.diagnostics};
        const bool own = block.kind == Block::Kind::Trigger && block.id == original->id;
        if (copy && !own) continue;
        if (!rewritten.value->replacements) continue;
        ++result.summary_.changedBlocks; result.summary_.changedReferences += rewritten.value->replacements;
        result.summary_.locations.push_back(Position(block, 0));
        auto text = Rewritten(block.text, rewritten.value->text);
        if (copy) copiedCode = std::move(text);
        else if (block.kind == Block::Kind::Global) candidate.wct.customCode = std::move(text);
        else if (block.kind == Block::Kind::Trigger) TriggerDocumentEditor::FindTrigger(candidate, block.id)->customText = std::move(text);
        else TriggerDocumentEditor::FindArgument(candidate, block.id)->value = std::move(text);
    }
    if (copy) {
        auto snapshot = TriggerDirectoryEditor::CopyTrigger(source, original->id); if (!snapshot.Succeeded()) return {{}, snapshot.diagnostics};
        snapshot.value->trigger.customText = std::move(copiedCode);
        const auto added = TriggerDirectoryEditor::PasteTrigger(candidate, catalog, *snapshot.value, categoryId, request.name, request.afterTriggerId);
        if (!added.Succeeded()) return {{}, added.diagnostics};
        result.summary_.resultId = *added.value;
        TriggerDocumentEditor::FindTrigger(candidate, *added.value)->name = std::move(newName);
    } else {
        TriggerDocumentEditor::FindTrigger(candidate, original->id)->name = std::move(newName);
        for (auto& arg : candidate.argumentNodes) if (!scriptArguments.contains(arg.id) && arg.type == 1 && TriggerDirectoryEditor::IsTriggerSymbolOf(arg.value.text, original->name)) {
            if (arg.callEca || arg.indexArgument) return fail(Bad("BINDING_UNSUPPORTED", "触发器引用同时含调用或数组索引，无法证明其身份。"));
            arg.value.text = newGlobal; arg.value.originalBytes.clear(); ++result.summary_.changedReferences;
        }
        for (auto& variable : candidate.variables) if (variable.typeName.text == "trigger" && variable.hasDefault && TriggerDirectoryEditor::IsTriggerSymbolOf(variable.value.text, original->name)) {
            variable.value.text = newGlobal; variable.value.originalBytes.clear(); ++result.summary_.changedReferences;
        }
    }
    checked = WtgWctCodec{}.Validate(candidate, catalog); if (!checked.Succeeded()) return fail(checked);
    auto replay = CaptureTriggerGraphReplayIdentity(source, catalog); if (!replay.Succeeded()) return {{}, replay.diagnostics};
    auto change = std::shared_ptr<PreparedTriggerCodeIdentityChange>(new PreparedTriggerCodeIdentityChange());
    change->before_ = std::move(*replay.value); change->after_ = std::make_shared<const TriggerDocument>(std::move(candidate));
    change->verify_ = context.verify; result.change_ = std::move(change);
    return R::Success(std::move(result));
}
Result<Trigger::TriggerGraphEditReceipt> TriggerCodeIdentityEditing::Apply(const TriggerCodeIdentityContext& context,
    Trigger::TriggerDocument& document, const PreparedTriggerCodeIdentityEdit& prepared) {
    using Q = Result<Trigger::TriggerGraphEditReceipt>;
    auto checked = Context(context); if (!checked.Succeeded()) return {{}, checked.diagnostics};
    auto binding = Trigger::CaptureTriggerGraphBinding(document, *context.catalog, context.mapObjects);
    if (!binding.Succeeded()) return {{}, binding.diagnostics};
    if (!Same(*binding.value, prepared.binding_)) return Q::Failure("Trigger.CodeIdentity", "TRIGGER_CODE_SOURCE_STALE", "发布前图、目录或物体引用已变化。");
    if (!prepared.summary_.changed) { Trigger::TriggerGraphEditReceipt receipt; receipt.before = *binding.value; receipt.after = *binding.value; return Q::Success(std::move(receipt)); }
    Trigger::TriggerGraphEditBatch batch{*binding.value, {prepared.Operation()}};
    return Trigger::ApplyTriggerGraphEditBatch(document, *context.catalog, batch, {}, context.mapObjects);
}
}
