#include "MainWindow.h"
#include "MapSizeDialog.h"
#include "MapSizeNavigation.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Workspace.h"

#include <QPointer>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
QString MapSizeText(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
} // namespace

War3Map::Result<War3Map::Services::MapSizeReport> MainWindow::QueryMapSizeAnalysis(std::string_view expectedRevision) {
    namespace Services = War3Map::Services;
    using Output = War3Map::Result<Services::MapSizeReport>;
    const auto revision = CommandContextRevision();
    if (expectedRevision != revision)
        return Output::Failure("Qt.MapSize", "MAP_SIZE_STALE_CONTEXT", "工程内容已变化，请刷新体积分析。");
    if (!project_ || (!project_->archiveSession_ && !project_->directorySnapshot_))
        return Output::Failure("Qt.MapSize", "MAP_SIZE_SOURCE_UNAVAILABLE", "没有已捕获的归档或目录元数据。");
    try {
        Services::MapSizeContext context; context.contextRevision = revision;
        // 只冻结有界元数据；额外一行让正式服务准确报告默认4096窗口截断。
        constexpr std::size_t rowLimit = 4097;
        std::size_t captured = 0, pathBytes = 0;
        const auto accept = [&](std::string_view first, std::string_view second = {}) {
            constexpr std::size_t pathLimit = 2048, totalPathLimit = 1024 * 1024;
            if (captured == rowLimit || first.size() > pathLimit || second.size() > pathLimit
                || first.size() > totalPathLimit - pathBytes) return false;
            const auto remaining = totalPathLimit - pathBytes - first.size();
            if (second.size() > remaining) return false;
            ++captured; pathBytes += first.size() + second.size(); return true;
        };
        if (project_->archiveSession_) {
            context.sourceKind = Services::MapSizeSourceKind::ArchiveMembers; context.membersComplete = true;
            const auto& archive = *project_->archiveSession_;
            context.opaqueMemberCount = static_cast<std::uint64_t>(archive.OpaqueMemberCount());
            context.opaqueMemberCountUnknown = archive.HasOpaqueMembers() && !archive.OpaqueMemberCount();
            for (const auto& member : archive.Members()) {
                if (!accept(member.name)) { context.membersComplete = false; break; }
                context.members.push_back({member.name, static_cast<std::uint64_t>(member.data.size())});
            }
            // attributes/signature 只有名称缓存，没有正文大小。将其作为已知存在、大小未知，不能补读。
            for (const auto& name : archive.MetadataMembers()) {
                if (!accept(name)) { context.membersComplete = false; break; }
                context.members.push_back({name, std::nullopt});
            }
        } else {
            context.sourceKind = Services::MapSizeSourceKind::DirectorySnapshot;
            context.membersComplete = false; // 保存的是已捕获正证据，不能声称权限跳过项不存在。
            for (const auto& member : project_->directorySnapshot_->Snapshot().files) {
                // 先限Windows原路径长度，避免为无界长路径创建临时UTF8副本。
                if (member.relativePath.native().size() > 2048) break;
                const auto path = War3Map::IO::GenericPathUtf8(member.relativePath);
                if (!accept(path)) break;
                context.members.push_back({path, static_cast<std::uint64_t>(member.size)});
            }
        }
        for (const auto& edit : project_->assetEdits_) {
            if (edit.from.size() > 2048 || edit.to.size() > 2048) { context.overlayComplete = false; break; }
            const auto from = edit.from.toUtf8().toStdString(), to = edit.to.toUtf8().toStdString();
            if (!accept(from, to)) { context.overlayComplete = false; break; }
            context.assetEdits.push_back({from, to});
        }
        for (const auto& pending : project_->pendingImports_) {
            if (pending.archivePath.size() > 2048) { context.overlayComplete = false; break; }
            const auto path = pending.archivePath.toUtf8().toStdString();
            if (!accept(path)) { context.overlayComplete = false; break; }
            const auto size = pending.ownedPayload
                ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(pending.ownedPayload->size())}
                : pending.size >= 0 ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(pending.size)} : std::nullopt;
            context.pending.push_back({path, size, pending.ownedPayload ? Services::MapSizeByteTrust::OwnedMetadata
                : size ? Services::MapSizeByteTrust::ReportedUnverified : Services::MapSizeByteTrust::Unknown});
        }
        const bool mapDirty = project_->mapEdits_ && project_->mapEdits_->IsDirty();
        context.generatedScriptSizeUnknown = project_->triggerDirty_ || mapDirty;
        // 新WTG/物编/基础数据尚未序列化，已有正文size只能证明原始快照。
        // 不调用Replacements或编译器求新size；当前账整体Unknown，独立baseline保留。
        if (project_->triggerDirty_ || mapDirty || std::any_of(project_->objectDirty_.begin(), project_->objectDirty_.end(), [](bool dirty) { return dirty; }))
            context.overlayComplete = false;
        if (project_->sourceChangeSuspected_ || project_->sourceChangeConfirmed_) {
            context.membersComplete = false; context.overlayComplete = false;
        }
        const auto result = Services::AnalyzeMapSize(context, revision);
        if (CommandContextRevision() != revision)
            return Output::Failure("Qt.MapSize", "MAP_SIZE_STALE_CONTEXT", "统计期间工程内容变化，请刷新重试。");
        return result;
    } catch (const std::bad_alloc&) {
        return Output::Failure("Qt.MapSize", "MAP_SIZE_CAPTURE_ALLOCATION", "无法分配有界元数据统计。");
    } catch (const std::exception&) {
        return Output::Failure("Qt.MapSize", "MAP_SIZE_CAPTURE_INVALID", "冻结元数据路径或长度无效，未补读来源文件。");
    }
}

void MainWindow::ShowMapSizeAnalysis() {
    if (!mapSizeDialog_) {
        const QPointer<MainWindow> window(this);
        mapSizeDialog_ = new War3Map::QtApp::MapSizeDialog(
            [window] { return window ? window->CommandContextRevision() : std::string{}; },
            [window](std::string_view revision) {
                if (window) return window->QueryMapSizeAnalysis(revision);
                return War3Map::Result<War3Map::Services::MapSizeReport>::Failure("Qt.MapSize", "MAP_SIZE_WINDOW_CLOSED", "工程窗口已关闭。");
            }, [window](const auto& row, std::string_view revision) {
                if (window) window->ActivateMapSizeRow(row, revision);
            }, this);
    }
    mapSizeDialog_->Focus();
}
void MainWindow::ActivateMapSizeRow(const War3Map::Services::MapSizeRow& row, std::string_view expectedRevision) {
    if (expectedRevision != CommandContextRevision()) { ShowStatusMessage(tr("工程已变化，请刷新体积分析。")); return; }
    if (!row.navigationAllowed || row.managedMember || row.nonAsciiIdentity || row.trust == War3Map::Services::MapSizeByteTrust::Unknown) {
        ShowStatusMessage(tr("成员 %1 只有元数据" ).arg(MapSizeText(row.currentPath))); return;
    }
    // 复用同一纯元数据服务，核identity/原路径/当前路径，不用名字、行号或低层fallback猜资源。
    const auto current = QueryMapSizeAnalysis(expectedRevision);
    if (!current.Succeeded() || !current.value) { ShowStatusMessage(tr("当前成员身份无法确认，请刷新体积分析。")); return; }
    const auto found = std::find_if(current.value->top20.begin(), current.value->top20.end(), [&](const auto& item) {
        return item.identity == row.identity && item.originalPath == row.originalPath && item.currentPath == row.currentPath
            && item.originalLogicalBytes == row.originalLogicalBytes && item.currentLogicalBytes == row.currentLogicalBytes
            && item.category == row.category && item.origin == row.origin && item.trust == row.trust && item.navigationAllowed
            && !item.managedMember && !item.nonAsciiIdentity;
    });
    if (found == current.value->top20.end() || CommandContextRevision() != expectedRevision) {
        ShowStatusMessage(tr("资源元数据已变化，请刷新后定位。")); return;
    }
    const auto entries = project_->AssetEntries(4097);
    const auto target = War3Map::QtApp::MakeMapSizeAssetNavigationTarget(*found, entries);
    if (!target || CommandContextRevision() != expectedRevision) {
        ShowStatusMessage(tr("资源来源已变化或不唯一，请刷新后定位。")); return;
    }
    Navigate(*target);
}
