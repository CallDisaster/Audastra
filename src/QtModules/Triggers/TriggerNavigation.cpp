#include "TriggerModule.h"
#include "TriggerLuaScripts.h"
#include "TriggerModels.h"
#include "TriggerBlueprint.h"
#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "Widgets/CodeView.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QLineEdit>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTreeView>
#include <QSortFilterProxyModel>

#include <algorithm>
#include <cmath>
#include <limits>

namespace War3Map::Modules {
namespace {
constexpr auto kSchema = "audastra.navigation.triggers.v1";
bool Integer(const QJsonObject& state, const char* key, int minimum = 0) {
    const auto value = state.value(QLatin1String(key));
    const auto number = value.toDouble(-1.0);
    return value.isDouble() && number >= minimum && number <= (std::numeric_limits<int>::max)()
        && number == static_cast<int>(number);
}
std::optional<StableId> Identity(const QJsonValue& value, bool allowZero = false) {
    if (!value.isString()) return std::nullopt;
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 20 || !std::all_of(text.begin(), text.end(), [](QChar c) {
        return c >= QLatin1Char('0') && c <= QLatin1Char('9');
    })) return std::nullopt;
    bool valid = false;
    const auto id = text.toULongLong(&valid);
    return valid && (allowZero || id != 0) ? std::optional<StableId>(id) : std::nullopt;
}
bool SmallString(const QJsonObject& state, const char* key) {
    const auto value = state.value(QLatin1String(key));
    return value.isString() && value.toString().size() <= 4096 && !value.toString().contains(QChar{});
}
} // namespace

void TriggerModule::CancelNavigationRestoration() {
    ++navigationRestoreGeneration_;
    disconnect(navigationTextLoaded_);
    navigationTextLoaded_ = {};
}

std::optional<QJsonObject> TriggerModule::CaptureNavigationBookmark() const {
    if (!page_) return std::nullopt;
    QJsonObject state{{"schema", QLatin1String(kSchema)}, {"trigger", QString::number(selectedTrigger_)},
        {"eca", QStringLiteral("0")}, {"search", explorerSearch_->text()},
        {"treeScroll", explorerView_->verticalScrollBar()->value()},
        {"treeHorizontal", explorerView_->horizontalScrollBar()->value()},
        {"logicScroll", ecaView_->verticalScrollBar()->value()},
        {"logicHorizontal", ecaView_->horizontalScrollBar()->value()},
        {"blueprintScroll", blueprint_->verticalScrollBar()->value()},
        {"blueprintHorizontal", blueprint_->horizontalScrollBar()->value()},
        {"blueprintZoom", blueprint_->transform().m11()}, {"codeLanguage", codeLanguage_->currentIndex()},
        {"rawValues", showRawValues_->isChecked()}, {"mapRawBytes", mapScriptRawBytes_->isChecked()}};
    QJsonArray collapsed;
    for (const auto id : collapsedCategories_) collapsed.append(id);
    state.insert("collapsed", collapsed);
    UiKit::CodeView* code = nullptr;
    if (workspaceTabs_->currentIndex() == 1 && luaScripts_) {
        if (luaScripts_->SelectedPath().isEmpty()) return std::nullopt;
        state.insert("mode", QStringLiteral("lua-file"));
        state.insert("path", luaScripts_->SelectedPath()); code = luaScripts_->Editor();
    } else if (showingMapScript_) {
        state.insert("mode", QStringLiteral("map-script")); code = mapScriptView_;
    } else if (showingGlobalCode_) {
        state.insert("mode", QStringLiteral("global-code")); code = scriptView_;
    } else if (codeTarget_ && codeTarget_->kind == Trigger::CodeBlockKind::CustomTrigger) {
        state.insert("mode", QStringLiteral("custom-code")); code = scriptView_;
    } else if (selectedTrigger_) {
        state.insert("mode", BlueprintVisible() ? QStringLiteral("blueprint") : QStringLiteral("traditional"));
        const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex());
        const auto id = BlueprintVisible() ? blueprint_->SelectedEca() : eca ? eca->id : 0;
        state.insert("eca", QString::number(id));
    } else if (selectedCategory_) {
        state.insert("mode", QStringLiteral("category")); state.insert("category", *selectedCategory_);
    } else return std::nullopt;
    const auto mode = state.value("mode").toString();
    if (mode != QLatin1String("lua-file") && mode != QLatin1String("map-script")
        && !project_.triggerDocument_) return std::nullopt;
    if (code) {
        if (code->IsLoading()) return std::nullopt;
        const auto cursor = code->textCursor();
        state.insert("cursor", cursor.position()); state.insert("cursorAnchor", cursor.anchor());
        state.insert("codeScroll", code->verticalScrollBar()->value());
        state.insert("codeHorizontal", code->horizontalScrollBar()->value());
    }
    return state;
}

void TriggerModule::RestoreNavigationCodeView(UiKit::CodeView* view, const QJsonObject& state) {
    const QPointer<UiKit::CodeView> guarded(view);
    const auto token = navigationRestoreGeneration_, documentGeneration = navigationDocumentGeneration_;
    const auto editGeneration = project_.editRevision_.Generation(), revision = project_.editRevision_.Revision();
    const auto host = host_;
    const auto workspaceEpoch = host ? host->NavigationEpoch() : 0;
    const auto codeGeneration = view->Generation();
    const auto source = view->SourceName();
    const auto target = codeTarget_;
    const auto settingsBinding = codeSourceSettingsSha256_;
    const auto luaPath = luaScripts_ ? luaScripts_->SelectedPath() : QString{};
    const auto mode = state.value("mode").toString();
    const auto apply = [this, guarded, token, documentGeneration, editGeneration, revision, host,
        workspaceEpoch, codeGeneration, source, target, settingsBinding, luaPath, mode, state]() {
        if (!guarded || guarded->IsLoading() || navigationRestoreGeneration_ != token
            || navigationDocumentGeneration_ != documentGeneration || host_ != host
            || (host_ && host_->NavigationEpoch() != workspaceEpoch)
            || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_
            || project_.editRevision_.Generation() != editGeneration || project_.editRevision_.Revision() != revision
            || guarded->Generation() != codeGeneration || guarded->SourceName() != source) return;
        if (mode == QLatin1String("lua-file")) {
            if (workspaceTabs_->currentIndex() != 1 || !luaScripts_ || luaScripts_->SelectedPath() != luaPath) return;
        } else if (mode == QLatin1String("map-script")) {
            if (workspaceTabs_->currentIndex() != 0 || !showingMapScript_) return;
        } else {
            if (workspaceTabs_->currentIndex() != 0 || !target || !codeTarget_
                || target->kind != codeTarget_->kind || target->triggerId != codeTarget_->triggerId
                || codeSourceSettingsSha256_ != settingsBinding
                || showingGlobalCode_ != (mode == QLatin1String("global-code"))) return;
            if (!project_.triggerDocument_) return;
            if (target->kind == Trigger::CodeBlockKind::CustomTrigger
                && (selectedTrigger_ != target->triggerId || std::none_of(project_.triggerDocument_->triggers.begin(),
                    project_.triggerDocument_->triggers.end(), [&](const auto& item) {
                        return item.id == target->triggerId && item.type == 0 && item.customTextFlag != 0;
                    }))) return;
        }
        // 同一稳定对象正文可能已经变短；纯视图坐标按当前正文范围收紧，不恢复正文。
        const int last = (std::max)(0, guarded->document()->characterCount() - 1);
        auto cursor = guarded->textCursor();
        cursor.setPosition((std::min)(state.value("cursorAnchor").toInt(), last));
        cursor.setPosition((std::min)(state.value("cursor").toInt(), last), QTextCursor::KeepAnchor);
        guarded->setTextCursor(cursor);
        guarded->verticalScrollBar()->setValue(state.value("codeScroll").toInt());
        guarded->horizontalScrollBar()->setValue(state.value("codeHorizontal").toInt());
    };
    if (view->IsLoading()) navigationTextLoaded_ = connect(view, &UiKit::CodeView::TextLoaded,
        this, apply, Qt::SingleShotConnection);
    else apply();
}

Shell::NavigationRestoreResult TriggerModule::RestoreNavigationBookmark(const QJsonObject& state) {
    using Result = Shell::NavigationRestoreResult;
    const auto mode = state.value("mode").toString();
    const auto triggerId = Identity(state.value("trigger"), true), ecaId = Identity(state.value("eca"), true);
    if (state.value("schema").toString() != QLatin1String(kSchema) || !triggerId || !ecaId
        || !SmallString(state, "search") || !state.value("rawValues").isBool()
        || !state.value("mapRawBytes").isBool() || !state.value("collapsed").isArray()
        || state.value("collapsed").toArray().size() > 4096) return Result::Invalid;
    const auto zoom = state.value("blueprintZoom");
    if (!zoom.isDouble() || !std::isfinite(zoom.toDouble()) || zoom.toDouble() < 0.25
        || zoom.toDouble() > 1.7 || !Integer(state, "codeLanguage") || state.value("codeLanguage").toInt() > 2)
        return Result::Invalid;
    for (const auto* key : {"treeScroll", "treeHorizontal", "logicScroll", "logicHorizontal"})
        if (!Integer(state, key)) return Result::Invalid;
    // QGraphicsView 使用场景坐标：原蓝图的负场景边界可产生合法的负滚动值。
    // 仍严格限制为有限、完整的 int；普通树/逻辑滚动和代码光标继续要求非负。
    for (const auto* key : {"blueprintScroll", "blueprintHorizontal"})
        if (!Integer(state, key, (std::numeric_limits<int>::min)())) return Result::Invalid;
    for (const auto& value : state.value("collapsed").toArray()) {
        const auto number = value.toDouble();
        if (!value.isDouble() || !std::isfinite(number) || number < (std::numeric_limits<int>::min)()
            || number > (std::numeric_limits<int>::max)() || number != static_cast<int>(number)) return Result::Invalid;
    }
    const bool code = mode == QLatin1String("global-code") || mode == QLatin1String("custom-code")
        || mode == QLatin1String("map-script") || mode == QLatin1String("lua-file");
    if (code) for (const auto* key : {"cursor", "cursorAnchor", "codeScroll", "codeHorizontal"})
        if (!Integer(state, key)) return Result::Invalid;
    const auto* document = project_.triggerDocument_.get();
    std::optional<QByteArray> mapBytes;
    std::optional<QtProject::OwnedAssetSource> luaSource;
    if (mode != QLatin1String("lua-file") && mode != QLatin1String("map-script")
        && (!document || !project_.uiCatalog_)) return Result::Invalid;
    const Trigger::TriggerRecord* trigger = nullptr;
    if (document) {
        const auto found = std::find_if(document->triggers.begin(), document->triggers.end(),
            [&](const auto& item) { return item.id == *triggerId; });
        if (found != document->triggers.end()) trigger = &*found;
    }
    if (mode == QLatin1String("category")) {
        if (!Integer(state, "category", (std::numeric_limits<int>::min)())
            || std::none_of(document->categories.begin(), document->categories.end(), [&](const auto& item) {
                return item.categoryId == state.value("category").toInt();
            })) return Result::Invalid;
    } else if (mode == QLatin1String("lua-file")) {
        if (!SmallString(state, "path") || state.value("path").toString().isEmpty()) return Result::Invalid;
        const auto entries = project_.AssetEntries();
        const auto path = state.value("path").toString();
        if (!path.endsWith(".lua", Qt::CaseInsensitive) || std::none_of(entries.begin(), entries.end(), [&](const auto& item) {
            return !item.ambiguous && item.path == path;
        })) return Result::Invalid;
        const auto read = QtProject::ReadOwnedAsset(project_, path);
        if (!read.Succeeded() || !read.value || read.value->path != path) return Result::Invalid;
        luaSource = *read.value;
    } else if (mode == QLatin1String("map-script")) {
        mapBytes = ReadMapScriptBytes();
        if (!mapBytes) return Result::Invalid;
    } else if (mode == QLatin1String("global-code")) {
        // 地图头是文档唯一槽位；不按名称或上一次索引猜测来源。
    } else if (mode == QLatin1String("custom-code")) {
        if (!trigger || trigger->type != 0 || trigger->customTextFlag == 0) return Result::Invalid;
    } else if (mode == QLatin1String("traditional") || mode == QLatin1String("blueprint")) {
        if (!trigger || trigger->customTextFlag != 0) return Result::Invalid;
        if (*ecaId) {
            const auto placement = Trigger::TriggerDocumentEditor::LocateEcaPlacement(*document, *ecaId);
            if (!placement.found || placement.parentTriggerId != *triggerId) return Result::Invalid;
        }
    } else return Result::Invalid;
    // 校验 anchor 在提交草稿之前；缺失目标不能吞掉作者当前草稿。
    if (!CommitPendingEdits()) return Result::Blocked;
    CancelNavigationRestoration();
    (void)Widget();
    // accepted Lua 来源不再读盘；若来源身份在提交门中失效，保持原 tab / filter / 正文。
    if (luaSource && !luaScripts_->SelectAcceptedScript(std::move(*luaSource))) return Result::Blocked;
    workspaceTabs_->setCurrentIndex(mode == QLatin1String("lua-file") ? 1 : 0);
    explorerSearch_->clear();
    UiKit::CodeView* view = nullptr;
    if (mode == QLatin1String("lua-file")) {
        view = luaScripts_->Editor();
    } else if (mode == QLatin1String("global-code")) {
        if (!ShowGlobalCustomCode()) return Result::Blocked;
        view = scriptView_;
    } else if (mode == QLatin1String("map-script")) {
        mapScriptRawBytes_->setChecked(state.value("mapRawBytes").toBool());
        ShowMapScript(std::move(mapBytes));
        if (!showingMapScript_) return Result::Invalid;
        view = mapScriptView_;
    } else if (mode == QLatin1String("category")) {
        explorerView_->setCurrentIndex(filter_->mapFromSource(explorerModel_->IndexForCategoryId(state.value("category").toInt())));
        ShowSelectedTrigger();
    } else {
        SelectTrigger(*triggerId); ShowSelectedTrigger();
        if (mode == QLatin1String("custom-code")) view = scriptView_;
        else { RefreshEditor(*ecaId); SetBlueprintVisible(mode == QLatin1String("blueprint")); }
    }
    collapsedCategories_.clear();
    for (const auto& id : state.value("collapsed").toArray()) collapsedCategories_.insert(id.toInt());
    ApplyExplorerExpansion(); explorerSearch_->setText(state.value("search").toString());
    showRawValues_->setChecked(state.value("rawValues").toBool());
    if (mode == QLatin1String("blueprint")) blueprint_->ZoomBy(zoom.toDouble() / blueprint_->transform().m11());
    if (mode == QLatin1String("global-code") || mode == QLatin1String("custom-code"))
        codeLanguage_->setCurrentIndex(state.value("codeLanguage").toInt());
    explorerView_->verticalScrollBar()->setValue(state.value("treeScroll").toInt());
    explorerView_->horizontalScrollBar()->setValue(state.value("treeHorizontal").toInt());
    ecaView_->verticalScrollBar()->setValue(state.value("logicScroll").toInt());
    ecaView_->horizontalScrollBar()->setValue(state.value("logicHorizontal").toInt());
    blueprint_->verticalScrollBar()->setValue(state.value("blueprintScroll").toInt());
    blueprint_->horizontalScrollBar()->setValue(state.value("blueprintHorizontal").toInt());
    if (view) RestoreNavigationCodeView(view, state);
    return Result::Restored;
}
} // namespace War3Map::Modules
