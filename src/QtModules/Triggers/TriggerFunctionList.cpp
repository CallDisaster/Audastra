#include "TriggerFunctionList.h"

#include "TriggerUiText.h"

#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QCollator>
#include <QCoreApplication>

#include <algorithm>
#include <utility>

using TriggerUiText::Field;
using TriggerUiText::Utf8;

TriggerFunctionList::TriggerFunctionList(const War3Map::Trigger::UiCatalog& catalog, CategoryIcon icon)
    : catalog_(catalog), categories_(catalog), icon_(std::move(icon)) {}

QString TriggerFunctionList::Title(const War3Map::Trigger::UiDefinition& definition) const {
    if (const auto found = titles_.constFind(&definition); found != titles_.constEnd()) return found.value();
    const QString title = Utf8(War3Map::Trigger::PrefixedTitle(catalog_, categories_, definition));
    titles_.insert(&definition, title);
    return title;
}

QString TriggerFunctionList::CategoryId(const War3Map::Trigger::UiDefinition& definition) const {
    return Utf8(categories_.CanonicalId(War3Map::Trigger::TriggerCategoryTable::CategoryOf(definition)));
}

QString TriggerFunctionList::CategoryName(const QString& categoryId) const {
    if (categoryId.isEmpty()) return QCoreApplication::translate("TriggerFunctionList", "未分类");
    return Utf8(categories_.DisplayName(categoryId.toStdString()));
}

QIcon TriggerFunctionList::Icon(const QString& categoryId) const {
    if (!icon_ || categoryId.isEmpty()) return {};
    if (const auto found = icons_.constFind(categoryId); found != icons_.constEnd()) return found.value();
    const QIcon icon = icon_(categoryId);
    icons_.insert(categoryId, icon);
    return icon;
}

QIcon TriggerFunctionList::Icon(const War3Map::Trigger::UiDefinition& definition) const {
    return Icon(CategoryId(definition));
}

bool TriggerFunctionList::Matches(const War3Map::Trigger::UiDefinition& definition, const QString& query) const {
    if (query.isEmpty()) return true;
    const auto contains = [&query](const QString& text) { return text.contains(query, Qt::CaseInsensitive); };
    if (contains(Title(definition)) || contains(Utf8(definition.name))) return true;
    const auto description = Field(definition.Active(), "description");
    if (description.has_value() && contains(Utf8(*description))) return true;
    const QString category = CategoryId(definition);
    return contains(category) || contains(CategoryName(category)) || contains(Utf8(definition.Active().layer));
}

bool TriggerFunctionList::Fits(const War3Map::Trigger::TriggerTypeSystem& types, std::string_view expected,
    const War3Map::Trigger::UiDefinition& definition) {
    if (expected == "Null") return false;
    const std::string returns = TriggerUiText::CallReturnType(definition);
    return !returns.empty() && types.Accepts(expected, returns);
}

TriggerFunctionList::Choices TriggerFunctionList::ForType(const War3Map::Trigger::TriggerTypeSystem& types,
    std::string_view expected, const QString& query) const {
    Choices choices;
    if (const auto slot = War3Map::Trigger::TriggerTypeSystem::FunctionSlotKind(expected)) {
        for (const auto* definition : catalog_.Definitions(*slot))
            if (Matches(*definition, query)) choices.slot.push_back(definition);
    }
    for (const auto* definition : catalog_.Definitions(War3Map::Trigger::UiDefinitionKind::Call)) {
        if (!Fits(types, expected, *definition) || !Matches(*definition, query)) continue;
        const std::string returns = TriggerUiText::CallReturnType(*definition);
        (returns == expected ? choices.exact : returns == "AnyReturnType" ? choices.generic : choices.compatible)
            .push_back(definition);
    }
    for (auto* group : {&choices.slot, &choices.exact, &choices.compatible, &choices.generic}) Sort(*group);
    return choices;
}

void TriggerFunctionList::Sort(std::vector<const War3Map::Trigger::UiDefinition*>& definitions) const {
    QCollator collator(QLocale(QLocale::Chinese, QLocale::China));
    collator.setNumericMode(true);
    std::stable_sort(definitions.begin(), definitions.end(), [&](const auto* left, const auto* right) {
        const int order = collator.compare(Title(*left), Title(*right));
        return order != 0 ? order < 0 : left->name < right->name;
    });
}
