#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Text/TextUtil.h"

#include "TriggerIdAllocation.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Trigger {
namespace {

constexpr const char* kDomain = "Trigger.Directory";

template <typename T>
Result<T> Forward(std::vector<Diagnostic> diagnostics) {
    Result<T> result;
    result.diagnostics = std::move(diagnostics);
    return result;
}

Status ValidateText(std::string_view name) {
    if (Text::TrimLineWhitespace(name).empty()) {
        return Status::Failure(kDomain, "NAME_EMPTY", "名称不能为空。");
    }
    if (name.find('\0') != std::string_view::npos) {
        return Status::Failure(kDomain, "NAME_INVALID", "名称不能包含 NUL。");
    }
    return Status::Ok();
}

CategoryRecord* FindCategory(TriggerDocument& document, std::int32_t categoryId) {
    for (auto& category : document.categories) {
        if (category.categoryId == categoryId) {
            return &category;
        }
    }
    return nullptr;
}

std::optional<std::size_t> TriggerRow(const TriggerDocument& document, StableId triggerId) {
    for (std::size_t row = 0; row < document.triggers.size(); ++row) {
        if (document.triggers[row].id == triggerId) {
            return row;
        }
    }
    return std::nullopt;
}

// 类别内的插入位置：同类别的 afterRow 之后；否则该类别最后一个触发器之后；类别为空时放到末尾。
std::size_t InsertRow(const TriggerDocument& document, std::int32_t categoryId, std::optional<std::size_t> afterRow) {
    if (afterRow && *afterRow < document.triggers.size() && document.triggers[*afterRow].categoryId == categoryId) {
        return *afterRow + 1;
    }
    for (std::size_t row = document.triggers.size(); row > 0; --row) {
        if (document.triggers[row - 1].categoryId == categoryId) {
            return row;
        }
    }
    return document.triggers.size();
}

} // namespace

TriggerDirectoryLayout TriggerDirectoryEditor::CaptureLayout(const TriggerDocument& document) {
    TriggerDirectoryLayout layout;
    layout.categories = document.categories;
    layout.triggers.reserve(document.triggers.size());
    for (const auto& trigger : document.triggers) {
        layout.triggers.emplace_back(trigger.id, trigger.categoryId);
    }
    return layout;
}

void TriggerDirectoryEditor::ApplyLayout(TriggerDocument& document, const TriggerDirectoryLayout& layout) {
    document.categories = layout.categories;
    std::unordered_map<StableId, std::pair<std::size_t, std::int32_t>> positions;
    positions.reserve(layout.triggers.size());
    for (std::size_t index = 0; index < layout.triggers.size(); ++index) {
        positions.emplace(layout.triggers[index].first, std::make_pair(index, layout.triggers[index].second));
    }
    const auto rank = [&positions](const TriggerRecord& trigger) {
        const auto found = positions.find(trigger.id);
        return found == positions.end() ? std::numeric_limits<std::size_t>::max() : found->second.first;
    };
    std::stable_sort(document.triggers.begin(), document.triggers.end(),
        [&rank](const TriggerRecord& left, const TriggerRecord& right) { return rank(left) < rank(right); });
    for (auto& trigger : document.triggers) {
        if (const auto found = positions.find(trigger.id); found != positions.end()) {
            trigger.categoryId = found->second.second;
        }
    }
}

std::string TriggerDirectoryEditor::UniqueName(const std::vector<std::string>& existing, std::string_view base) {
    const std::unordered_set<std::string> names(existing.begin(), existing.end());
    for (unsigned number = 1;; ++number) {
        char suffix[16] = {};
        std::snprintf(suffix, sizeof(suffix), " %03u", number);
        std::string name = std::string(base) + suffix;
        if (!names.contains(name)) {
            return name;
        }
    }
}

std::string TriggerDirectoryEditor::UniqueTriggerName(const TriggerDocument& document, std::string_view base) {
    std::unordered_set<std::string> names;
    std::unordered_set<std::string> scriptNames;
    for (const auto& trigger : document.triggers) {
        names.insert(trigger.name.text);
        scriptNames.insert(ScriptName(trigger.name.text));
    }
    for (unsigned number = 1;; ++number) {
        char suffix[16] = {};
        std::snprintf(suffix, sizeof(suffix), " %03u", number);
        std::string name = std::string(base) + suffix;
        if (!names.contains(name) && !scriptNames.contains(ScriptName(name))) {
            return name;
        }
    }
}

std::string TriggerDirectoryEditor::ScriptName(std::string_view name) {
    return Text::ObjectScriptName(name);
}

const TriggerRecord* TriggerDirectoryEditor::FindScriptNameConflict(
    const TriggerDocument& document, std::string_view name, StableId self) {
    const std::string wanted = ScriptName(name);
    // 注释条目（WTG isComment）不生成 gg_trg_ 变量，不参与脚本名冲突。
    for (const auto& trigger : document.triggers) {
        if (trigger.id != self && trigger.type == 0 && ScriptName(trigger.name.text) == wanted) return &trigger;
    }
    return nullptr;
}

std::string TriggerDirectoryEditor::UniqueCategoryName(const TriggerDocument& document, std::string_view base) {
    std::vector<std::string> names;
    names.reserve(document.categories.size());
    for (const auto& category : document.categories) names.push_back(category.name.text);
    return UniqueName(names, base);
}

Status TriggerDirectoryEditor::ValidateTriggerName(
    const TriggerDocument& document, std::string_view name, StableId self) {
    if (Status text = ValidateText(name); !text.Succeeded()) {
        return text;
    }
    const std::string_view trimmed = Text::TrimLineWhitespace(name);
    for (const auto& trigger : document.triggers) {
        if (trigger.id != self && trigger.name.text == trimmed) {
            return Status::Failure(kDomain, "TRIGGER_NAME_DUPLICATE",
                "已有名为“" + std::string(trimmed) + "”的触发器。");
        }
    }
    const auto renamed = std::find_if(document.triggers.begin(), document.triggers.end(),
        [self](const TriggerRecord& trigger) { return self != 0 && trigger.id == self; });
    if (renamed != document.triggers.end() && renamed->type != 0) return Status::Ok(); // 注释条目改名只查同名
    if (const auto* conflict = FindScriptNameConflict(document, trimmed, self)) {
        return Status::Failure(kDomain, "TRIGGER_SCRIPT_NAME_DUPLICATE",
            "“" + std::string(trimmed) + "”与触发器“" + conflict->name.text + "”在脚本中的名称相同（gg_trg_" +
                ScriptName(trimmed) + "），请换一个名称。");
    }
    return Status::Ok();
}

Status TriggerDirectoryEditor::ValidateCategoryName(
    const TriggerDocument& document, std::string_view name, std::optional<std::int32_t> self) {
    if (Status text = ValidateText(name); !text.Succeeded()) {
        return text;
    }
    const std::string_view trimmed = Text::TrimLineWhitespace(name);
    for (const auto& category : document.categories) {
        if ((!self || category.categoryId != *self) && category.name.text == trimmed) {
            return Status::Failure(kDomain, "CATEGORY_NAME_DUPLICATE",
                "已有名为“" + std::string(trimmed) + "”的类别。");
        }
    }
    return Status::Ok();
}

Result<std::int32_t> TriggerDirectoryEditor::AddCategory(
    TriggerDocument& document, std::string name, std::optional<std::int32_t> afterCategoryId) {
    if (Status text = ValidateText(name); !text.Succeeded()) {
        return Forward<std::int32_t>(std::move(text.diagnostics));
    }
    std::int32_t categoryId = 0;
    for (const auto& category : document.categories) {
        if (category.categoryId == std::numeric_limits<std::int32_t>::max()) {
            return Result<std::int32_t>::Failure(kDomain, "CATEGORY_ID_EXHAUSTED", "类别编号已耗尽。");
        }
        categoryId = std::max(categoryId, category.categoryId + 1);
    }
    std::uint64_t counter = detail::NextGeneratedCounter(document);
    const StableId id = detail::AllocateId(counter);
    if (id == 0) {
        return Result<std::int32_t>::Failure(kDomain, "CATEGORY_STABLE_ID_EXHAUSTED", "生成类别 ID 空间已耗尽。");
    }
    CategoryRecord record;
    record.id = id;
    record.categoryId = categoryId;
    record.name.encoding = detail::NewTextEncoding(document);
    record.name.text = std::string(Text::TrimLineWhitespace(name));
    auto at = document.categories.end();
    if (afterCategoryId) {
        at = std::find_if(document.categories.begin(), document.categories.end(),
            [&](const CategoryRecord& category) { return category.categoryId == *afterCategoryId; });
        if (at != document.categories.end()) ++at;
    }
    document.categories.insert(at, std::move(record));
    return Result<std::int32_t>::Success(categoryId);
}

Status TriggerDirectoryEditor::RenameCategory(TriggerDocument& document, std::int32_t categoryId, std::string name) {
    if (Status text = ValidateText(name); !text.Succeeded()) {
        return text;
    }
    CategoryRecord* category = FindCategory(document, categoryId);
    if (category == nullptr) {
        return Status::Failure(kDomain, "CATEGORY_MISSING", "找不到目标类别。");
    }
    category->name.text = std::string(Text::TrimLineWhitespace(name));
    category->name.originalBytes.clear();
    return Status::Ok();
}

Status TriggerDirectoryEditor::RemoveEmptyCategory(TriggerDocument& document, std::int32_t categoryId) {
    const auto found = std::find_if(document.categories.begin(), document.categories.end(),
        [categoryId](const CategoryRecord& category) { return category.categoryId == categoryId; });
    if (found == document.categories.end()) {
        return Status::Failure(kDomain, "CATEGORY_MISSING", "找不到目标类别。");
    }
    const bool used = std::any_of(document.triggers.begin(), document.triggers.end(),
        [categoryId](const TriggerRecord& trigger) { return trigger.categoryId == categoryId; });
    if (used) {
        return Status::Failure(kDomain, "CATEGORY_NOT_EMPTY", "类别中仍有触发器，需先删除或移走。");
    }
    document.categories.erase(found);
    return Status::Ok();
}

Status TriggerDirectoryEditor::MoveCategory(
    TriggerDocument& document, std::int32_t categoryId, std::optional<std::int32_t> beforeCategoryId) {
    if (beforeCategoryId && *beforeCategoryId == categoryId) {
        return Status::Ok();
    }
    const auto found = std::find_if(document.categories.begin(), document.categories.end(),
        [categoryId](const CategoryRecord& category) { return category.categoryId == categoryId; });
    if (found == document.categories.end()) {
        return Status::Failure(kDomain, "CATEGORY_MISSING", "找不到目标类别。");
    }
    CategoryRecord moving = std::move(*found);
    document.categories.erase(found);
    auto at = document.categories.end();
    if (beforeCategoryId) {
        at = std::find_if(document.categories.begin(), document.categories.end(),
            [&](const CategoryRecord& category) { return category.categoryId == *beforeCategoryId; });
    }
    document.categories.insert(at, std::move(moving));
    return Status::Ok();
}

Status TriggerDirectoryEditor::MoveTrigger(
    TriggerDocument& document, StableId triggerId, std::int32_t categoryId, StableId beforeTriggerId) {
    if (beforeTriggerId == triggerId) {
        return Status::Ok();
    }
    if (FindCategory(document, categoryId) == nullptr) {
        return Status::Failure(kDomain, "CATEGORY_MISSING", "目标类别不存在。");
    }
    const auto row = TriggerRow(document, triggerId);
    if (!row) {
        return Status::Failure(kDomain, "TRIGGER_MISSING", "找不到目标触发器。");
    }
    TriggerRecord moving = std::move(document.triggers[*row]);
    document.triggers.erase(document.triggers.begin() + static_cast<std::ptrdiff_t>(*row));
    moving.categoryId = categoryId;
    std::size_t at = InsertRow(document, categoryId, std::nullopt);
    if (beforeTriggerId != 0) {
        if (const auto before = TriggerRow(document, beforeTriggerId);
            before && document.triggers[*before].categoryId == categoryId) {
            at = *before;
        }
    }
    document.triggers.insert(document.triggers.begin() + static_cast<std::ptrdiff_t>(at), std::move(moving));
    return Status::Ok();
}

Result<TriggerSnapshot> TriggerDirectoryEditor::CopyTrigger(const TriggerDocument& document, StableId triggerId) {
    const auto row = TriggerRow(document, triggerId);
    if (!row) {
        return Result<TriggerSnapshot>::Failure(kDomain, "TRIGGER_MISSING", "找不到要复制的触发器。");
    }
    TriggerSnapshot snapshot;
    snapshot.trigger = document.triggers[*row];
    snapshot.roots.reserve(snapshot.trigger.ecas.size());
    for (const std::uint32_t root : snapshot.trigger.ecas) {
        if (root >= document.ecaNodes.size()) {
            return Result<TriggerSnapshot>::Failure(kDomain, "TRIGGER_GRAPH_INVALID", "触发器引用了不存在的 ECA 节点。");
        }
        auto copied = TriggerDocumentEditor::CopyEcaSubtree(document, document.ecaNodes[root].id);
        if (!copied.Succeeded()) {
            return Forward<TriggerSnapshot>(std::move(copied.diagnostics));
        }
        snapshot.roots.push_back(std::move(*copied.value));
    }
    snapshot.trigger.ecas.clear();
    return Result<TriggerSnapshot>::Success(std::move(snapshot));
}

Result<StableId> TriggerDirectoryEditor::PasteTrigger(
    TriggerDocument& document,
    const UiCatalog& catalog,
    const TriggerSnapshot& snapshot,
    std::int32_t categoryId,
    std::string name,
    StableId afterTriggerId) {
    if (Status text = ValidateText(name); !text.Succeeded()) {
        return Forward<StableId>(std::move(text.diagnostics));
    }
    if (FindCategory(document, categoryId) == nullptr) {
        return Result<StableId>::Failure(kDomain, "CATEGORY_MISSING", "目标类别不存在。");
    }
    std::uint64_t counter = detail::NextGeneratedCounter(document);
    const StableId id = detail::AllocateId(counter);
    if (id == 0) {
        return Result<StableId>::Failure(kDomain, "TRIGGER_ID_EXHAUSTED", "生成触发器 ID 空间已耗尽。");
    }
    TriggerRecord record = snapshot.trigger;
    record.id = id;
    record.categoryId = categoryId;
    record.name.text = std::string(Text::TrimLineWhitespace(name));
    record.name.originalBytes.clear();
    record.ecas.clear();
    record.origin = {};
    const std::size_t at = InsertRow(document, categoryId,
        afterTriggerId != 0 ? TriggerRow(document, afterTriggerId) : std::nullopt);
    document.triggers.insert(document.triggers.begin() + static_cast<std::ptrdiff_t>(at), std::move(record));

    for (std::size_t index = 0; index < snapshot.roots.size(); ++index) {
        const auto& root = snapshot.roots[index];
        std::optional<std::int32_t> childId;
        for (std::size_t position = 0; position < root.ecaOldIndices.size(); ++position) {
            if (root.ecaOldIndices[position] == root.rootOldIndex) {
                childId = root.ecas[position].childId;
                break;
            }
        }
        auto pasted = TriggerDocumentEditor::PasteEcaSubtree(document, catalog, root, id, 0, index, childId);
        if (!pasted.Succeeded()) {
            // 回滚：删除新触发器会连同已粘贴的子树一起压实，文档回到粘贴前。
            (void)TriggerDocumentEditor::RemoveTrigger(document, id);
            return Forward<StableId>(std::move(pasted.diagnostics));
        }
    }
    return Result<StableId>::Success(id);
}

} // namespace War3Map::Trigger
