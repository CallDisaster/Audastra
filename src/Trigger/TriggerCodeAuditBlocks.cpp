#include "TriggerCodeAuditBlocks.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include <algorithm>
#include <map>
#include <tuple>

namespace War3Map::Trigger::detail {
Result<std::string> CodeBlockSha256(const RawText& text) {
    const auto bytes = text.ToBytes();
    if (!bytes.Succeeded()) return {std::nullopt, bytes.diagnostics};
    return Sha256(*bytes.value);
}
Status VisitOpaqueCodeBlocks(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const std::string> symbols, const CodeBlockVisitor& visit, const ArgumentClassObserver& observer) {
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
        if (observer) if (const auto status = observer(arg, expected[i], opaque || dispatch[i], dispatch[i]); !status.Succeeded()) return status;
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

namespace War3Map::Trigger {
Result<OpaqueArgumentInventory> InspectOpaqueArgumentInventory(const TriggerDocument& document, const UiCatalog& catalog) {
    using Output = Result<OpaqueArgumentInventory>;
    if (document.argumentNodes.size() > 524288 || document.ecaNodes.size() > 524288)
        return Output::Failure("Trigger.CodeAudit", "TRIGGER_CODE_INVENTORY_BUDGET", "分类统计超出图规模预算，未截断统计。");
    OpaqueArgumentInventory inventory;
    const TriggerTypeSystem types(catalog);
    std::map<std::tuple<std::string, bool, bool, bool>, std::size_t> groups;
    const auto status = detail::VisitOpaqueCodeBlocks(document, catalog, {},
        [](AtomicRenameCodeKind, StableId, const RawText&) { return Status::Ok(); },
        [&](const ArgumentNode& argument, std::string_view expected, bool opaque, bool dispatch) {
            if (argument.type != 3) return Status::Ok();
            const auto encoded = argument.value.ToBytes(); if (!encoded.Succeeded()) return Status{encoded.diagnostics};
            const auto bytes = encoded.value->size();
            if (bytes > 64 * 1024 * 1024 - inventory.totalBytes)
                return Status::Failure("Trigger.CodeAudit", "TRIGGER_CODE_INVENTORY_BUDGET", "分类统计超过64MiB文本预算，未截断统计。");
            inventory.totalBytes += bytes; ++inventory.arguments;
            const bool valid = types.ValidateLiteral(expected, argument.value.text).Succeeded();
            const auto key = std::tuple{std::string(expected), valid, opaque, dispatch};
            auto [entry, inserted] = groups.emplace(key, inventory.classes.size());
            if (inserted) {
                if (groups.size() > 512) return Status::Failure("Trigger.CodeAudit", "TRIGGER_CODE_INVENTORY_BUDGET", "分类统计超过512类，未截断统计。");
                inventory.classes.push_back({std::string(expected), static_cast<int>(types.LiteralKind(expected)), valid, opaque, dispatch});
            }
            auto& group = inventory.classes[entry->second]; ++group.count; group.bytes += bytes;
            if (bytes <= 64) ++group.shortCount;
            if (group.samples.size() < 3) {
                auto length = std::min<std::size_t>(argument.value.text.size(), 64);
                while (length < argument.value.text.size() && length && (static_cast<unsigned char>(argument.value.text[length]) & 0xC0U) == 0x80U) --length;
                group.samples.push_back({argument.id, bytes, argument.value.text.substr(0, length)});
            }
            if (opaque) ++inventory.opaqueArguments;
            return Status::Ok();
        });
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    std::stable_sort(inventory.classes.begin(), inventory.classes.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
    return Output::Success(std::move(inventory));
}
}
