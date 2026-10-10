#include "EditorProject.h"

#include "ProjectFileIO.h"

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QUndoStack>

#include <algorithm>

// 触发器工作区：按 UI 数据根载入函数目录并解析 WTG/WCT。界面在 TriggerWorkspaceChanged 后重新绑定。
namespace War3Map::QtProject {

bool EditorProject::HasUiCatalogConfig(const std::filesystem::path& candidate) {
    if (candidate.empty()) {
        return false;
    }
    std::error_code error;
    try {
        return std::filesystem::is_regular_file(
            Trigger::UiCatalog::ResolveUiRoot(candidate) / "config", error);
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}

std::filesystem::path EditorProject::ResolveUiDataRoot() const {
    // 本次显式选择的根必须立即生效，不被项目层覆盖。
    if (HasUiCatalogConfig(activeUiDataRoot_)) {
        return activeUiDataRoot_;
    }
    // 展开地图可自带 ui/config，作为项目级 Catalog 层优先于全局后备。
    if (HasUiCatalogConfig(projectRoot_)) {
        return projectRoot_;
    }
    if (HasUiCatalogConfig(uiDataRoot_)) {
        return uiDataRoot_;
    }
    return {};
}

// 分类与函数分类取自 Catalog（各层 define.txt 已在其中按层序合并），不再另读文件。
// 第三方层可以任意命名且可缺失，这里不包含任何库名或函数名白名单。
void EditorProject::RefreshUiIconMappings() {
    if (iconProvider_ == nullptr) {
        return;
    }
    iconProvider_->SetUiToolRoot(UiToolRoot());
    if (uiCatalog_ != nullptr) {
        iconProvider_->MergeCatalogCategories(*uiCatalog_);
    }
}

void EditorProject::ReplaceTriggerWorkspace(std::unique_ptr<Trigger::UiCatalog> catalog,
    std::unique_ptr<Trigger::TriggerDocument> document) {
    // 目录与文档一起替换：先通知界面解除对旧目录 / 文档的引用，再释放旧对象。
    emit TriggerDocumentAboutToChange();
    if (catalog) ApplyWorldEditStrings(*catalog);
    triggerDocument_ = std::move(document);
    uiCatalog_ = std::move(catalog);
    emit TriggerDocumentChanged();
}

bool EditorProject::LoadTriggers() {
    if (triggerUndoStack_ != nullptr) {
        triggerUndoStack_->clear();
        triggerUndoStack_->setClean();
    }
    triggerDirty_ = false;
    ReplaceTriggerWorkspace(nullptr, nullptr);
    if (iconProvider_ != nullptr) {
        // 先移除上一个 Catalog 的映射；删库或切层后不得残留旧函数。
        iconProvider_->ResetUiMappings();
    }
    const auto fail = [this](const QString& status) {
        triggerStatus_ = status;
        emit TriggerDocumentChanged();
        return false;
    };

    const std::filesystem::path wtgPath = projectRoot_ / "map" / "war3map.wtg";
    const std::filesystem::path wctPath = projectRoot_ / "map" / "war3map.wct";
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(wtgPath, fileError) || fileError ||
        !std::filesystem::is_regular_file(wctPath, fileError) || fileError) {
        AddDiagnostic(tr("信息"), tr("触发器"), tr("TRIGGER_FILES_MISSING"),
            tr("未找到完整 WTG/WCT；物体数据仍可独立使用。"));
        return fail(tr("当前工作区没有完整的 map/war3map.wtg 与 war3map.wct。"));
    }

    const std::filesystem::path uiRoot = ResolveUiDataRoot();
    if (uiRoot.empty()) {
        AddDiagnostic(tr("警告"), tr("触发器"), tr("UI_CATALOG_NOT_FOUND"),
            tr("第三方 UI 层本身是可选的，但 WTG 不保存参数数量；解析引用这些函数的节点时必须提供对应库。"));
        return fail(tr("未找到 UI Catalog；请选择当前 Warcraft/YDWE 的 UI 数据根。"));
    }
    activeUiDataRoot_ = uiRoot;

    auto loadedCatalog = LoadEffectiveUiCatalog(uiRoot);
    AddDiagnostics(loadedCatalog.diagnostics);
    WatchUiLibraries(); // 热重载：按本次扫描到的包重新挂监视
    if (!loadedCatalog.Succeeded()) {
        return fail(tr("UI Catalog 加载失败；请查看诊断中的原始路径与层名称。"));
    }
    // 目录先生效（与旧行为一致：WTG 解析失败时仍保留已载入的目录）。
    ReplaceTriggerWorkspace(std::make_unique<Trigger::UiCatalog>(std::move(*loadedCatalog.value)), nullptr);

    // 仅从当前 Catalog 动态补充函数→分类与分类→纹理映射。
    // 未安装的库不会被虚构，WTG 缺失定义诊断仍保留原函数名。
    if (iconProvider_ != nullptr) {
        RefreshUiIconMappings();
        AddDiagnostic(tr("信息"), tr("游戏资源"), tr("ICON_MAPPING_SUMMARY"),
            tr("已按当前 UI Catalog 动态合并图标映射：函数 %1 项 · 分类 %2 项。")
                .arg(iconProvider_->FunctionCategoryCount())
                .arg(iconProvider_->CategoryCount()));
    }

    const auto wtg = ReadProjectFileBytes(wtgPath);
    const auto wct = ReadProjectFileBytes(wctPath);
    if (!wtg || !wct || wtg->empty() || wct->empty()) {
        AddDiagnostic(tr("错误"), tr("触发器"), tr("TRIGGER_READ_FAILED"),
            tr("WTG 或 WCT 为空/不可读，已停止解析。"));
        return fail(tr("无法完整读取 WTG/WCT。"));
    }

    Trigger::WtgWctCodec codec;
    auto parsed = codec.Parse(*wtg, *wct, *uiCatalog_);
    if (!parsed.Succeeded() && std::any_of(parsed.diagnostics.begin(), parsed.diagnostics.end(),
            [](const Diagnostic& diagnostic) { return diagnostic.code == "wtg.missing-ui-definition"; })) {
        if (auto retried = ParseWithLocallyDisabledLibraries(uiRoot, *wtg, *wct)) {
            ReplaceTriggerWorkspace(std::move(retried->catalog), nullptr);
            parsed = Result<Trigger::TriggerDocument>::Success(std::move(*retried->document));
        }
    }
    AddDiagnostics(parsed.diagnostics);
    if (!parsed.Succeeded()) {
        const bool missingFunction = std::any_of(parsed.diagnostics.begin(), parsed.diagnostics.end(),
            [](const Diagnostic& diagnostic) { return diagnostic.code == "wtg.missing-ui-definition"; });
        return fail(missingFunction
            ? tr("WTG 引用了未安装的 UI 函数；诊断保留原函数名，请删除对应节点或补回库。")
            : tr("WTG/WCT 解析失败；当前会话保持只读且不会生成触发器候选。"));
    }
    triggerStatus_.clear();
    auto catalog = std::move(uiCatalog_);
    ReplaceTriggerWorkspace(std::move(catalog), std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value)));

    RecordUiLibraryUsage();
    const Trigger::TriggerInspection inspection = codec.Inspect(*triggerDocument_);
    AddDiagnostic(tr("信息"), tr("触发器"), tr("WTG_WCT_OPENED"),
        tr("WTG/WCT 已按当前动态 UI Catalog 打开：%1 个触发器、%2 个 ECA、%3 个参数；普通 GUI 节点可编辑。")
            .arg(inspection.triggerCount).arg(inspection.ecaCount).arg(inspection.argumentCount));
    AddDiagnostic(tr("信息"), tr("UI Catalog"), tr("UI_CATALOG_LOADED"),
        tr("已启用 %1 个层，解析 %2 个现有 TXT；未安装的可选层不会被硬编码补齐。")
            .arg(uiCatalog_->Layers().size()).arg(uiCatalog_->Documents().size()));
    if (triggerUndoStack_ != nullptr) {
        triggerUndoStack_->clear();
        triggerUndoStack_->setClean();
    }
    triggerDirty_ = false;
    return true;
}

} // namespace War3Map::QtProject
