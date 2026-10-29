#pragma once
#include "War3MapCore/Trigger/TriggerEditing.h"
#include <functional>

namespace War3Map::Trigger::detail {
using CodeBlockVisitor = std::function<Status(AtomicRenameCodeKind, StableId, const RawText&)>;
using ArgumentClassObserver = std::function<Status(const ArgumentNode&, std::string_view, bool, bool)>;
// rename/remove共用同一份不透明块清单；symbol用于目录预设中的精确保守匹配。
Status VisitOpaqueCodeBlocks(const TriggerDocument&, const UiCatalog&, std::span<const std::string> symbols,
    const CodeBlockVisitor&, const ArgumentClassObserver& observer = {});
Result<std::string> CodeBlockSha256(const RawText&);
}
