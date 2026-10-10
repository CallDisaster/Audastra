#include "TriggerFunctionList.h"

#include "TriggerUiText.h"

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

void TriggerFunctionList::Sort(std::vector<const War3Map::Trigger::UiDefinition*>& definitions) const {
    QCollator collator(QLocale(QLocale::Chinese, QLocale::China));
    collator.setNumericMode(true);
    std::stable_sort(definitions.begin(), definitions.end(), [&](const auto* left, const auto* right) {
        const int order = collator.compare(Title(*left), Title(*right));
        return order != 0 ? order < 0 : left->name < right->name;
    });
}
