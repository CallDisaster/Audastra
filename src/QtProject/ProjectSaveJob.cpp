#include "ProjectSaveJob.h"
#include "PendingImportPayload.h"
#include "War3MapCore/VerifiedFile.h"

#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <exception>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace War3Map::QtProject {
namespace {
constexpr std::uint64_t kProjectBytes = 512ull * 1024 * 1024;

Result<ProjectSaveResult> Failed(const QString& message) {
    return Result<ProjectSaveResult>::Failure("工程", "PROJECT_SAVE_FAILED", message.toStdString());
}
std::vector<std::byte> Bytes(const QByteArray& value) {
    const auto* start = reinterpret_cast<const std::byte*>(value.constData());
    return {start, start + value.size()};
}
} // namespace

// 先锁住源，再调用原 MapArchiveSession 的 volume/fileId/SHA 门禁；
// ReadVerifiedFile 读取期间也持有自己的同类只读租约。恢复读取完成即释放此锁，
// 后续打包使用拥有的完整字节，不能因恢复任务长时间阻塞用户写回原图。
struct ProjectSaveJob::SourceLease {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit SourceLease(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        if (handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_DISK) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
    ~SourceLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    [[nodiscard]] bool Valid() const noexcept { return handle != INVALID_HANDLE_VALUE; }
#else
    explicit SourceLease(const std::filesystem::path&) {}
    [[nodiscard]] bool Valid() const noexcept { return false; }
#endif
};

ProjectSaveJob::ProjectSaveJob() = default;
ProjectSaveJob::~ProjectSaveJob() { ReleaseCommitLeases(); }

void ProjectSaveJob::ReleaseRunLeases() noexcept {
    // 即使已切图，旧解压快照和旧恢复解包目录的最后释放也发生在 worker。
    archive_.reset();
    directory_.reset();
    overlays_.clear();
    imports_.clear();
    nativeBaselines_.clear();
    manifest_ = {};
    if (recovery_) ReleaseCommitLeases();
}

void ProjectSaveJob::ReleaseCommitLeases() noexcept {
    // manual 同步链在 Commit 才结束租约；先关 HANDLE，再释放目录 owner。
    sourceLease_.reset();
    restoredFiles_.reset();
    mapWorkspace_.reset();
}

void EditorProject::RunProjectSave(ProjectSaveJob& job) {
    auto expected = ProjectSaveJob::Phase::Prepared;
    if (!job.phase_.compare_exchange_strong(expected, ProjectSaveJob::Phase::Running,
        std::memory_order_acq_rel)) return;
    // 所有退出路径都先销毁大输入/来源强引用，再将任务标为完成；调用方此后才能 Commit。
    struct Finish {
        ProjectSaveJob& job;
        ~Finish() {
            const bool cancelled = job.options_.control && job.options_.control->Complete();
            job.ReleaseRunLeases();
            if (cancelled) {
                job.outcome_.value.reset();
                // 正常取消状态在 Run 的结果归一处已分配好，不在析构中分配。
                job.phase_.store(ProjectSaveJob::Phase::Cancelled, std::memory_order_release);
            } else job.phase_.store(ProjectSaveJob::Phase::Completed, std::memory_order_release);
        }
    } finish{job};
    try {
        job.outcome_ = [&job]() -> Result<ProjectSaveResult> {
            const auto* control = job.options_.control.get();
            const auto stopped = [control](ProjectSaveStage stage) { return control && !control->Checkpoint(stage); };
            if (stopped(ProjectSaveStage::Read)) return {{}, ProjectSaveCancelledStatus().diagnostics};
            std::vector<MapArchiveMember> files;
            std::uint64_t total = 0;
            const auto append = [&files, &total](std::string name, std::vector<std::byte> payload) {
                if (payload.size() > kProjectBytes - total) return false;
                total += payload.size();
                files.push_back({std::move(name), std::move(payload)});
                return true;
            };
            if (job.archive_) {
                job.sourceLease_ = std::make_unique<ProjectSaveJob::SourceLease>(job.sourceArchivePath_);
                if (!job.sourceLease_->Valid()) return Failed(tr("源地图读取失败或被其他写入占用"));
                if (!job.archive_->VerifySourceUnchanged(control).Succeeded())
                    return Failed(tr("源地图已变化，不能将当前编辑附到另一份基线"));
                auto bytes = ReadVerifiedFile(job.sourceArchivePath_, job.sourceSha256_, kProjectBytes, control);
                if (!bytes.Succeeded() || !bytes.value) return Failed(tr("源地图读取失败、内容变化或超过容量限制"));
                if (!append("baseline/source.w3x", std::move(*bytes.value))) return Failed(tr("工程超过容量限制"));
                // source/member 语义来自同一个冻结快照，验证完原成员以后才释放读锁。
            } else {
                if (!job.directory_ || !job.directory_->VerifyUnchanged(control).Succeeded())
                    return Failed(tr("源工作区已变化或缺少来源快照"));
                for (const auto& sourceFile : job.directory_->Snapshot().files) {
                    if (stopped(ProjectSaveStage::Read)) return {{}, ProjectSaveCancelledStatus().diagnostics};
                    const auto absolute = job.sourceRoot_ / sourceFile.relativePath;
                    std::error_code ec;
                    if (std::filesystem::is_symlink(absolute, ec) || ec)
                        return Failed(tr("工作区来源不可达或含符号链接"));
                    auto bytes = ReadVerifiedFile(absolute, sourceFile.sha256, 256ull * 1024 * 1024, control);
                    if (!bytes.Succeeded() || !bytes.value
                        || !append("baseline/workspace/" + QString::fromStdWString(sourceFile.relativePath.generic_wstring()).toStdString(), std::move(*bytes.value)))
                        return Failed(tr("工作区读取失败、被占用、内容变化或工程超过容量限制"));
                }
            }
            for (const auto& baseline : job.nativeBaselines_) {
                if (stopped(ProjectSaveStage::Verify)) return {{}, ProjectSaveCancelledStatus().diagnostics};
                if (baseline.archiveMemberPresent) {
                    const auto original = job.archive_->ReadMember(baseline.member, control);
                    if (!original.Succeeded() || !original.value || !EqualProjectSaveBytes(*original.value, baseline.expected, control))
                        return Failed(tr("克隆来源的 native 基线与实际源成员不同或不可读"));
                } else if (baseline.directoryMember) {
                    if (!job.archive_) {
                        const auto name = "baseline/workspace/" + *baseline.directoryMember;
                        const auto original = std::find_if(files.begin(), files.end(), [&name](const auto& member) { return member.name == name; });
                        if (original == files.end() || !EqualProjectSaveBytes(original->data, baseline.expected, control))
                            return Failed(tr("克隆来源的 native 基线与实际源成员不同或不可读"));
                    } else {
                        // 沿用归档打开时已捕获的投影资格；不借用可变 EditorProject。
                        const auto original = ReadVerifiedFile(job.sourceRoot_ / std::filesystem::path(*baseline.directoryMember), baseline.directorySha256, 256ull * 1024 * 1024, control);
                        if (!original.Succeeded() || !original.value || !EqualProjectSaveBytes(*original.value, baseline.expected, control))
                            return Failed(tr("克隆来源的 native 基线与实际源成员不同或不可读"));
                    }
                }
            }
            if (job.recovery_) job.sourceLease_.reset();
            for (auto& member : job.overlays_) {
                if (stopped(ProjectSaveStage::Build)) return {{}, ProjectSaveCancelledStatus().diagnostics};
                if (!append(std::move(member.name), std::move(member.data))) return Failed(tr("工程超过容量限制"));
            }
            for (const auto& source : job.imports_) {
                auto payload = ReadFrozenPendingImport(source.entry, source.sha256, control);
                if (!payload.Succeeded() || !payload.value
                    || !append(source.member, std::move(*payload.value)))
                    return Failed(tr("导入文件读取失败、被占用、内容变化或工程超过容量限制"));
            }
            // 只核对冻结的导入来源，不访问 UI/文档/Undo；晚到的 UI 编辑留给下一份快照。
            for (const auto& source : job.imports_) {
                const auto hash = FingerprintPendingImport(source.entry, control);
                if (!hash.Succeeded() || !hash.value || *hash.value != source.sha256)
                    return Failed(tr("导入文件在工程保存期间发生变化"));
            }
            if (!job.archive_ && (!job.directory_ || !job.directory_->VerifyUnchanged(control).Succeeded()))
                return Failed(tr("源工作区在工程保存期间发生变化"));
            if (!append("project.json", Bytes(QJsonDocument(job.manifest_).toJson(QJsonDocument::Compact))))
                return Failed(tr("工程超过容量限制"));
            return ProjectStore::Save(job.target_, files, job.options_);
        }();
    } catch (const std::exception&) {
        job.outcome_ = Failed(tr("工程保存发生内部异常，当前编辑基线保持不变"));
    } catch (...) {
        job.outcome_ = Failed(tr("工程保存发生未知内部异常，当前编辑基线保持不变"));
    }
    // Complete 与最终请求使用同一个 CAS；即使取消恰在普通失败返回时获胜，也准确回报取消。
    if (job.options_.control && job.options_.control->Complete()) {
        job.outcome_.value.reset();
        if (std::none_of(job.outcome_.diagnostics.begin(), job.outcome_.diagnostics.end(), [](const Diagnostic& item) {
            return item.code == kProjectSaveCancelledCode;
        })) {
            job.outcome_ = std::move(job.cancelledOutcome_);
        }
    }
}

} // namespace War3Map::QtProject
