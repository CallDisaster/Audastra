#include "TriggerCodeAuditBlocks.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include <algorithm>

namespace War3Map::Trigger::detail {
Result<std::string> CodeBlockSha256(const RawText& text) {
    const auto bytes = text.ToBytes();
    if (!bytes.Succeeded()) return {std::nullopt, bytes.diagnostics};
    return Sha256(*bytes.value);
}
Status VisitOpaqueCodeBlocks(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const std::string> symbols, const CodeBlockVisitor& visit) {
    if (auto s = visit(AtomicRenameCodeKind::GlobalCustomCode, 0, document.wct.customCode); !s.Succeeded()) return s;
    for (const auto& row : document.triggers)
        if (auto s = visit(AtomicRenameCodeKind::TriggerCustomCode, row.id, row.customText); !s.Succeeded()) return s;
    const TriggerTypeSystem types(catalog);
    std::vector<std::string> expected(document.argumentNodes.size());
    std::vector<bool> dispatch(document.argumentNodes.size());
    for (const auto& eca : document.ecaNodes) {
        const auto* definition = catalog.Find(eca.type, eca.name.text);
        const auto field = [&](std::string_view name) { return definition ? DefinitionField(*definition, name) : std::nullopt; };
        const auto script = field("script_name"), alternate = field("script");
        const bool dynamic = eca.name.text == "ExecuteFunc" || eca.name.text == "CustomScriptCode"
            || eca.name.text == "YDWECustomScriptCode" || (script && *script == "ExecuteFunc")
            || (alternate && *alternate == "ExecuteFunc");
        for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
            if (eca.arguments[i] >= expected.size()) return Status::Failure("Trigger.CodeAudit", "TRIGGER_CODE_AUDIT_STRUCTURE", "代码块参数索引越界。");
            expected[eca.arguments[i]] = types.ExpectedType(document, eca, i);
            dispatch[eca.arguments[i]] = dynamic;
        }
    }
    for (const auto& arg : document.argumentNodes) if (arg.indexArgument) {
        if (*arg.indexArgument >= expected.size()) return Status::Failure("Trigger.CodeAudit", "TRIGGER_CODE_AUDIT_STRUCTURE", "代码块数组索引越界。");
        expected[*arg.indexArgument] = "integer";
    }
    const auto literal = [&](std::string_view type, std::string_view value) {
        const auto kind = types.LiteralKind(type);
        if (kind == TriggerTypeSystem::Literal::String) return true;
        if (kind == TriggerTypeSystem::Literal::Integer || kind == TriggerTypeSystem::Literal::Real
            || kind == TriggerTypeSystem::Literal::Boolean || kind == TriggerTypeSystem::Literal::ObjectCode)
            return types.ValidateLiteral(type, value).Succeeded();
        return value == "null";
    };
    const auto mentions = [&](std::string_view text) {
        return std::any_of(symbols.begin(), symbols.end(), [&](const auto& symbol) { return text.find(symbol) != std::string_view::npos; });
    };
    for (std::size_t i = 0; i < document.argumentNodes.size(); ++i) {
        const auto& arg = document.argumentNodes[i];
        bool opaque = arg.type != 0 && arg.type != 1 && arg.type != 2 && !literal(expected[i], arg.value.text);
        if (arg.type == 0) { const auto preset = FindPreset(catalog, arg.value.text); opaque = !preset || mentions(preset->script); }
        if (opaque || dispatch[i])
            if (auto s = visit(AtomicRenameCodeKind::ArgumentCode, arg.id, arg.value); !s.Succeeded()) return s;
    }
    for (const auto& row : document.variables) if (!row.value.text.empty() && !literal(row.typeName.text, row.value.text)) {
        const auto preset = FindPreset(catalog, row.value.text);
        if (!preset || mentions(preset->script))
            if (auto s = visit(AtomicRenameCodeKind::VariableInitialValue, row.id, row.value); !s.Succeeded()) return s;
    }
    return Status::Ok();
}
}
