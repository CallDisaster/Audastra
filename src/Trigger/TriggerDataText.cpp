#include "War3MapCore/Trigger/TriggerDataText.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <cctype>

namespace War3Map::Trigger {
namespace {

bool Space(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::string WithoutCategoryPrefix(std::string_view id) {
    if (id.size() > 3 && id.starts_with("TC_")) id.remove_prefix(3);
    return std::string(id);
}

} // namespace

std::vector<std::string> SplitTriggerDataCsv(std::string_view value) {
    std::vector<std::string> result;
    std::string current;
    char quote = '\0';
    bool closed = false; // 刚结束一段引号，尚未遇到逗号
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (quote != '\0') {
            if (character == '\\' && index + 1 < value.size()) {
                current.push_back(value[++index]);
            } else if (character == quote) {
                quote = '\0';
                closed = true;
            } else {
                current.push_back(character);
            }
            continue;
        }
        if (character == ',') {
            result.push_back(std::move(current));
            current.clear();
            closed = false;
            continue;
        }
        if (closed) {
            if (Space(character)) continue;
            // 右引号后缺逗号：前一段引号内容自成一列。
            result.push_back(std::move(current));
            current.clear();
            closed = false;
        }
        if (character == '\'' || character == '"') {
            quote = character;
        } else {
            current.push_back(character);
        }
    }
    result.push_back(std::move(current));
    for (std::string& part : result) part = Text::TrimCWhitespaceCopy(part);
    return result;
}

std::optional<std::string_view> DefinitionField(const UiDefinition& definition, std::string_view name) {
    const auto& fields = definition.Active().fields;
    const auto found = std::find_if(fields.begin(), fields.end(), [name](const auto& field) { return field.first == name; });
    return found == fields.end() ? std::nullopt : std::optional<std::string_view>(found->second);
}

std::vector<std::string> DefineColumns(const UiDefinition& definition) {
    const auto value = DefinitionField(definition, "value");
    return value.has_value() ? SplitTriggerDataCsv(*value) : std::vector<std::string>{};
}

namespace {

TriggerPreset MakePreset(const UiCatalog& catalog, const UiDefinition& definition) {
    const auto columns = DefineColumns(definition);
    TriggerPreset preset;
    preset.definition = &definition;
    preset.name = definition.name;
    preset.type = columns.size() > 1 ? columns[1] : std::string{};
    preset.script = columns.size() > 2 ? columns[2] : std::string{};
    const std::string display = columns.size() > 3 ? catalog.Localize(columns[3]) : std::string{};
    if (!display.empty() && !Game::WorldEditStrings::IsKey(display)) {
        preset.display = display;
        preset.displayResolved = true;
    } else {
        preset.display = !preset.script.empty() ? preset.script : preset.name;
    }
    return preset;
}

} // namespace

std::optional<TriggerPreset> FindPreset(const UiCatalog& catalog, std::string_view name) {
    const auto* definition = catalog.Find(UiDefinitionKind::Define, name, "TriggerParams");
    if (definition == nullptr) return std::nullopt;
    return MakePreset(catalog, *definition);
}

std::vector<TriggerPreset> PresetsOfType(const UiCatalog& catalog, std::string_view type) {
    std::vector<TriggerPreset> presets;
    for (const auto* definition : catalog.Definitions(UiDefinitionKind::Define)) {
        if (definition->group != "TriggerParams") continue;
        auto preset = MakePreset(catalog, *definition);
        if (!type.empty() && preset.type != type) continue;
        presets.push_back(std::move(preset));
    }
    return presets;
}

namespace {

// WE 的 INI 键不区分大小写（MapUI 声明 TC_MapUI、函数写 TC_MAPUI 仍归入同一分类）。
std::string UpperKey(std::string_view id) {
    std::string key(id);
    for (char& c : key) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return key;
}

} // namespace

TriggerCategoryTable::TriggerCategoryTable(const UiCatalog& catalog) {
    for (const auto* definition : catalog.Definitions(UiDefinitionKind::Define)) {
        if (definition->group != "TriggerCategories") continue;
        const auto columns = DefineColumns(*definition);
        TriggerCategory category;
        category.id = definition->name;
        category.layer = definition->Active().layer;
        const std::string display = columns.empty() ? std::string{} : catalog.Localize(columns[0]);
        category.displayResolved = !display.empty() && !Game::WorldEditStrings::IsKey(display);
        category.display = category.displayResolved ? display : WithoutCategoryPrefix(category.id);
        category.icon = columns.size() > 1 ? columns[1] : std::string{};
        category.hidePrefix = columns.size() > 2 && columns[2] == "1";
        index_.emplace(UpperKey(category.id), categories_.size());
        categories_.push_back(std::move(category));
    }
}

const TriggerCategory* TriggerCategoryTable::Find(std::string_view id) const {
    const auto found = index_.find(UpperKey(id));
    return found == index_.end() ? nullptr : &categories_[found->second];
}

std::string TriggerCategoryTable::CanonicalId(std::string_view id) const {
    const auto* category = Find(id);
    return category != nullptr ? category->id : std::string(id);
}

std::string TriggerCategoryTable::DisplayName(std::string_view id) const {
    if (const auto* category = Find(id)) return category->display;
    return WithoutCategoryPrefix(id);
}

std::string TriggerCategoryTable::CategoryOf(const UiDefinition& definition) {
    std::string category(DefinitionField(definition, "category").value_or(std::string_view{}));
    category = Text::TrimCWhitespaceCopy(category);
    // 新格式允许 category = "TC_X"（带引号）。
    if (category.size() >= 2 && category.front() == '"' && category.back() == '"') category = category.substr(1, category.size() - 2);
    return category;
}

std::string TriggerCategoryTable::Prefix(const UiDefinition& definition) const {
    const auto* category = Find(CategoryOf(definition));
    // 显示名未能解析时仍用 id 作前缀：比没有前缀更容易辨认。
    if (category == nullptr || category->hidePrefix) return {};
    return category->display;
}

std::string DefinitionTitle(const UiCatalog& catalog, const UiDefinition& definition) {
    const auto title = DefinitionField(definition, "title");
    return title.has_value() && !title->empty() ? catalog.Localize(*title) : definition.name;
}

std::string PrefixedTitle(const UiCatalog& catalog, const TriggerCategoryTable& categories, const UiDefinition& definition) {
    const std::string title = DefinitionTitle(catalog, definition);
    const std::string prefix = categories.Prefix(definition);
    return prefix.empty() ? title : prefix + " - " + title;
}

} // namespace War3Map::Trigger
