#include "VariablesModule.h"
#include "VariablesDialog.h"
#include "EditorProject.h"
#include "Widgets/ColumnLayout.h"

#include <QCheckBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QScrollBar>
#include <QTableWidget>

#include <algorithm>
#include <limits>

namespace War3Map::Modules {
namespace {
bool Number(const QJsonObject& state, const char* key) {
    const auto value = state.value(QLatin1String(key));
    const auto number = value.toDouble(-1.0);
    return value.isDouble() && number >= 0 && number <= (std::numeric_limits<int>::max)()
        && number == static_cast<int>(number);
}
std::optional<StableId> StableVariable(const QJsonValue& value) {
    if (!value.isString()) return std::nullopt;
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 20 || !std::all_of(text.begin(), text.end(), [](QChar c) {
        return c >= QLatin1Char('0') && c <= QLatin1Char('9');
    })) return std::nullopt;
    bool valid = false;
    const auto id = text.toULongLong(&valid);
    return valid && id ? std::optional<StableId>(id) : std::nullopt;
}
} // namespace

std::optional<QJsonObject> VariablesModule::CaptureNavigationBookmark() const {
    const auto* table = editor_ ? editor_->findChild<QTableWidget*>("globalVariablesTable") : nullptr;
    const auto* raw = editor_ ? editor_->findChild<QCheckBox*>("variableRawValues") : nullptr;
    const auto* cell = table ? table->item(table->currentRow(), 0) : nullptr;
    if (!cell || !raw) return std::nullopt;
    const auto id = cell->data(Qt::UserRole).toULongLong();
    const auto* document = project_.triggerDocument_.get();
    if (!id || !document || std::none_of(document->variables.begin(), document->variables.end(),
        [id](const auto& item) { return item.id == id; })) return std::nullopt;
    return QJsonObject{{"schema", QStringLiteral("audastra.navigation.variables.v1")},
        {"selected", QString::number(id)}, {"scroll", table->verticalScrollBar()->value()},
        {"horizontal", table->horizontalScrollBar()->value()}, {"rawValues", raw->isChecked()},
        {"columns", UiKit::SaveColumnWidths(table->horizontalHeader())}};
}

Shell::NavigationRestoreResult VariablesModule::RestoreNavigationBookmark(const QJsonObject& state) {
    using Result = Shell::NavigationRestoreResult;
    const auto id = StableVariable(state.value("selected"));
    const auto* document = project_.triggerDocument_.get();
    if (state.value("schema").toString() != QStringLiteral("audastra.navigation.variables.v1")
        || !id || !document || !Number(state, "scroll") || !Number(state, "horizontal")
        || !state.value("rawValues").isBool() || !state.value("columns").isArray()
        || state.value("columns").toArray().size() != 5
        || std::none_of(document->variables.begin(), document->variables.end(),
            [id](const auto& item) { return item.id == *id; })) return Result::Invalid;
    for (const auto& value : state.value("columns").toArray()) {
        const auto number = value.toDouble(-1.0);
        if (!value.isDouble() || number < 16 || number > 8192 || number != static_cast<int>(number)) return Result::Invalid;
    }
    // 先验证文档稳定 ID，再创建/刷新视图；不按名称或旧行号回退。
    (void)Widget(); Refresh();
    auto* table = editor_ ? editor_->findChild<QTableWidget*>("globalVariablesTable") : nullptr;
    auto* raw = editor_ ? editor_->findChild<QCheckBox*>("variableRawValues") : nullptr;
    if (!table || !raw) return Result::Invalid;
    raw->setChecked(state.value("rawValues").toBool()); // 模型重建后仍按同一 ID 定位。
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* cell = table->item(row, 0);
        if (!cell || cell->data(Qt::UserRole).toULongLong() != *id) continue;
        table->setCurrentCell(row, 0); table->selectRow(row);
        UiKit::RestoreColumnWidths(table->horizontalHeader(), state.value("columns").toArray());
        table->verticalScrollBar()->setValue(state.value("scroll").toInt());
        table->horizontalScrollBar()->setValue(state.value("horizontal").toInt());
        return Result::Restored;
    }
    return Result::Invalid;
}
} // namespace War3Map::Modules
