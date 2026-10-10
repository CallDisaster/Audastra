#include "MainWindow.h"
#include "GlobalSearchDialog.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QLineEdit>
#include <QPointer>

#include <algorithm>

War3Map::Result<War3Map::Services::ProjectSearchPage> MainWindow::QueryProjectSearchPage(
    std::string_view expectedRevision, std::string_view query,
    const War3Map::Services::ProjectSearchOptions& options, std::size_t pageLimit,
    const War3Map::Services::ProjectSearchCursor* cursor) {
    using Output = War3Map::Result<War3Map::Services::ProjectSearchPage>;
    namespace Services = War3Map::Services;
    const auto revision = CommandContextRevision();
    if (expectedRevision != revision) return Output::Failure("Qt.Search", "PROJECT_SEARCH_STALE_CONTEXT", "工程内容已变化，请重新搜索。");
    if (!projectSearchCapture_) projectSearchCapture_ = std::make_unique<War3Map::QtApp::ProjectSearchCapture>();
    const auto prepared = projectSearchCapture_->Prepare(*project_, commandWindowId_,
        project_->editRevision_.Generation(), revision);
    if (!prepared.Succeeded()) return {std::nullopt, prepared.diagnostics};
    auto borrowed = projectSearchCapture_->Borrow(*project_, revision);
    if (!borrowed.Succeeded()) return {std::nullopt, std::move(borrowed.diagnostics)};
    auto context = std::move(*borrowed.value);
    context.triggers = project_->triggerDocument_.get();
    if (context.triggers) context.triggerState = Services::ProjectSearchState::Ready;
    const auto codeIndex = project_->CurrentCodeCheckIndex();
    if (codeIndex) {
        context.codeDeclarations = &codeIndex->declarations;
        context.codeBinding = codeIndex->contextBinding;
        context.codeState = codeIndex->contextComplete && codeIndex->declarations.completed && codeIndex->externalSourcesComplete
            ? Services::ProjectSearchState::Ready : Services::ProjectSearchState::Partial;
    }
    context.standardObjects = project_->standardCatalog_.get();
    if (context.standardObjects) context.standardState = Services::ProjectSearchState::Ready;
    // 所有借用数据仅在本次同步调用内有效，返回页拥有字符串和稳定身份。
    const auto result = Services::SearchProjectPage(context, revision, query, options, pageLimit, cursor);
    if (CommandContextRevision() != revision)
        return Output::Failure("Qt.Search", "PROJECT_SEARCH_STALE_CONTEXT", "搜索期间工程内容已变化，请重新搜索。");
    return result;
}

void MainWindow::ActivateSearchImportedFile(const War3Map::Services::ProjectSearchRow& row) {
    const auto revision = CommandContextRevision();
    if (row.contextRevision != revision || !projectSearchCapture_) {
        ShowStatusMessage(tr("资源来源已变化，请重新搜索")); return;
    }
    const auto borrowed = projectSearchCapture_->Borrow(*project_, revision);
    if (!borrowed.Succeeded()) { ShowStatusMessage(tr("资源来源已变化，请重新搜索")); return; }
    std::optional<War3Map::QtProject::AssetEntry> selected;
    for (const auto& asset : projectSearchCapture_->Assets()) {
        const auto identity = (asset.Pending() ? "pending/" + asset.path : asset.originalPath).toUtf8().toStdString();
        if (identity != row.rowKey) continue;
        if (selected || asset.ambiguous) { ShowStatusMessage(tr("资源路径有多份来源，不能选择其中一份定位")); return; }
        selected = asset;
    }
    if (!selected || CommandContextRevision() != revision) {
        ShowStatusMessage(tr("资源已变化，请重新搜索")); return;
    }
    Navigate({"assets", "file", selected->path, {{"stableIdentity", true},
        {"sourceIdentity", QString::fromUtf8(row.rowKey.data(), static_cast<qsizetype>(row.rowKey.size()))}}});
}

void MainWindow::ShowGlobalSearch() {
    if (!globalSearchDialog_) {
        const QPointer<MainWindow> window(this);
        globalSearchDialog_ = new War3Map::QtApp::GlobalSearchDialog(
            [window]() { return window ? window->CommandContextRevision() : std::string{}; },
            [window](std::string_view revision, std::string_view query,
                const War3Map::Services::ProjectSearchOptions& options, std::size_t pageLimit,
                const War3Map::Services::ProjectSearchCursor* cursor) {
                if (window) return window->QueryProjectSearchPage(revision, query, options, pageLimit, cursor);
                return War3Map::Result<War3Map::Services::ProjectSearchPage>::Failure("Qt.Search", "PROJECT_SEARCH_WINDOW_CLOSED", "工程窗口已关闭。");
            }, [window](const War3Map::Services::ProjectSearchRow& row) {
                if (window) window->ActivateSearchResult(row);
            }, this);
    }
    globalSearchDialog_->FocusQuery(globalSearch_ ? globalSearch_->text() : QString{});
}
