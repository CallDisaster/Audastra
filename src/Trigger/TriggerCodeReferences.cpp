#include "War3MapCore/Trigger/TriggerCodeReferences.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "TriggerCodeAuditBlocks.h"
#include "TriggerRenameEvidence.h"

#include <algorithm>
#include <array>

namespace War3Map::Trigger {

Result<std::vector<DeclarationCodeReference>> FindDeclarationCodeReferences(
    const TriggerDocument& document, const UiCatalog& catalog, std::span<const GraphRecordRef> targets) {
    using Output = Result<std::vector<DeclarationCodeReference>>;
    std::vector<detail::CodeSymbolTarget> symbols;
    symbols.reserve(targets.size());
    for (const auto& target : targets) {
        if (target.kind == GraphRecordKind::Trigger) {
            const auto found = std::find_if(document.triggers.begin(), document.triggers.end(), [&](const auto& t) { return t.id == target.id; });
            if (found == document.triggers.end()) return Output::Failure("Trigger.CodeReferences", "TRIGGER_MISSING", "找不到要检查的触发器。");
            // 按写出字节和按 UTF-8 正文换算的脚本名都查；编辑器显示名在运行期不存在，不按字符串查。
            const std::array scripts{TriggerDirectoryEditor::ScriptName(found->name), TriggerDirectoryEditor::ScriptName(found->name.text)};
            symbols.push_back(detail::TriggerCodeSymbols(scripts));
        } else if (target.kind == GraphRecordKind::Variable) {
            const auto found = std::find_if(document.variables.begin(), document.variables.end(), [&](const auto& v) { return v.id == target.id; });
            if (found == document.variables.end()) return Output::Failure("Trigger.CodeReferences", "TRIGGER_VARIABLE_MISSING", "找不到要检查的变量。");
            detail::CodeSymbolTarget variable;
            variable.exact.push_back("udg_" + found->name.text);
            symbols.push_back(std::move(variable));
        } else {
            return Output::Failure("Trigger.CodeReferences", "TRIGGER_CODE_REFERENCE_UNSUPPORTED", "只检查触发器和变量的代码引用。");
        }
    }
    // 目录预设的脚本里出现这些符号时，预设参数也按代码块查。
    std::vector<std::string> presetSymbols;
    for (const auto& target : symbols) {
        presetSymbols.insert(presetSymbols.end(), target.exact.begin(), target.exact.end());
        presetSymbols.insert(presetSymbols.end(), target.generated.begin(), target.generated.end());
    }
    std::vector<DeclarationCodeReference> references;
    const auto status = detail::VisitOpaqueCodeBlocks(document, catalog, presetSymbols,
        [&](AtomicRenameCodeKind kind, StableId id, const RawText& text) -> Status {
            if (text.text.empty()) return Status::Ok();
            const auto uses = detail::FindSymbolUses(text.text, symbols);
            for (std::size_t i = 0; i < uses.size(); ++i) {
                if (uses[i].kind == TriggerDocumentEditor::RenameEvidence::None) continue;
                references.push_back({i, kind, id, uses[i].line, uses[i].excerpt});
            }
            return Status::Ok();
        });
    if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
    return Output::Success(std::move(references));
}

} // namespace War3Map::Trigger
