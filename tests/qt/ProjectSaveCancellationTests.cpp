#include "EditorProject.h"
#include "ProjectSaveJob.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Workspace.h"
#include "War3MapCore/MapArchiveSession.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace War3Map;
using namespace War3Map::QtProject;
namespace {
constexpr std::size_t kMiB = 1024 * 1024;
bool HasCode(const Result<ProjectSaveResult>& result, std::string_view code) {
    return !result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& item) { return item.code == code; });
}
std::filesystem::path Path(const QString& value) { return std::filesystem::path(value.toStdWString()); }
void Edit(EditorProject& project, std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text));
    PendingImport entry{"owned.txt", {}, static_cast<qint64>(text.size())};
    entry.ownedPayload = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
    project.pendingImports_ = {std::move(entry)};
    (void)project.ObserveEditState("original owned import edit");
}
bool Setup(EditorProject& project, const QString& directory) {
    QFile source(directory + "/source.bin");
    if (!source.open(QIODevice::WriteOnly)) return false;
    QByteArray block(static_cast<qsizetype>(kMiB), Qt::Uninitialized); std::uint32_t seed = 0x82341033;
    for (auto& byte : block) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; byte = static_cast<char>(seed & 0xff); }
    for (unsigned index = 0; index < 8; ++index) if (source.write(block) != block.size()) return false;
    source.close();
    auto opened = DirectoryWorkspace::Open(Path(directory)); if (!opened.Succeeded()) return false;
    project.projectRoot_ = Path(directory);
    project.directorySnapshot_ = std::make_unique<DirectoryWorkspace>(std::move(*opened.value));
    // 夹具切换的是新会话，不能把上一会话的同字节导入作为新保存基线。
    project.pendingImports_.clear();
    project.ResetEditTracking();
    Edit(project, "frozen edit");
    return project.editTrackingReady_ && project.editRevision_.IsDirty(EditSaveTarget::Project);
}
struct Gate {
    ProjectSaveStage target; std::uint64_t threshold;
    std::mutex mutex; std::condition_variable changed; bool reached = false, released = false;
    void Progress(ProjectSaveStage stage, std::uint64_t bytes) {
        if (stage != target || bytes < threshold) return;
        std::unique_lock lock(mutex);
        if (reached) return;
        reached = true; changed.notify_all();
        // 测试交接握手有界；不依赖 sleep 与机器吞吐决定取消发生阶段。
        if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; }))
            throw std::runtime_error("owned cancellation gate timed out");
    }
    bool Await() { std::unique_lock lock(mutex); return changed.wait_for(lock, std::chrono::seconds(10), [&] { return reached; }); }
    void Release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
};
struct ReleaseGate { Gate& gate; ~ReleaseGate() { gate.Release(); } };
}

class ProjectSaveCancellationTests final : public QObject {
    Q_OBJECT
private slots:
    void RunningReadAndWriteCancellationPreservesPreviousPointAndEdits();
    void RecoveryCancellationDoesNotConsumeManualTicket();
    void CancelledOldGenerationReleasesOnlyItsOwnedSource();
    void LateCancelDoesNotMisreportAndRecoveryNeverCleans();
    void PreparedCancelAndManualDefaultsStayCompatible();
};

void ProjectSaveCancellationTests::RunningReadAndWriteCancellationPreservesPreviousPointAndEdits() {
    for (const auto stage : {ProjectSaveStage::Read, ProjectSaveStage::Write}) {
        QTemporaryDir source, destination; QVERIFY(source.isValid() && destination.isValid());
        EditorProject project; QVERIFY(Setup(project, source.path()));
        const auto sourceSha = Sha256File(Path(source.filePath("source.bin"))); QVERIFY(sourceSha.Succeeded());
        const auto target = Path(destination.filePath("recovery.forge"));
        const auto previous = project.SaveProjectToPath(target, true); QVERIFY(previous.Succeeded());
        const auto baseline = project.editRevision_.Baseline(EditSaveTarget::Project);
        QVERIFY(baseline.has_value());
        Gate gate{stage, kMiB}; auto control = std::make_shared<ProjectSaveControl>([&](auto observed, auto bytes) { gate.Progress(observed, bytes); });
        auto prepared = project.PrepareProjectSave(target, true, previous.value->sha256, control); QVERIFY(prepared.Succeeded());
        auto job = std::move(*prepared.value);
        std::jthread worker([&] { EditorProject::RunProjectSave(*job); }); ReleaseGate guard{gate};
        QVERIFY(gate.Await());
        QVERIFY(project.RequestProjectSaveCancellation(*job));
        Edit(project, "late editing remains dirty");
        const auto revision = project.editRevision_.Revision(); const auto scopes = project.editRevision_.Current();
        gate.Release(); worker.join();
        QVERIFY(job->Completed() && job->Cancelled() && !job->PublicationStarted());
        QVERIFY(HasCode(job->Outcome(), kProjectSaveCancelledCode));
        QVERIFY(HasCode(project.CommitProjectSave(*job), kProjectSaveCancelledCode));
        QCOMPARE(project.editRevision_.Revision(), revision); QCOMPARE(project.editRevision_.Current(), scopes);
        QCOMPARE(project.editRevision_.Baseline(EditSaveTarget::Project)->scopes, baseline->scopes);
        QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project));
        QCOMPARE(Sha256File(target).value, std::optional(previous.value->sha256));
        QCOMPARE(Sha256File(Path(source.filePath("source.bin"))).value, sourceSha.value);
        auto reopened = MapArchiveSession::Open(target); QVERIFY(reopened.Succeeded());
        QVERIFY(reopened.value->HasMember("baseline/workspace/source.bin"));
        QVERIFY(!project.RequestProjectSaveCancellation(*job));
    }
}

void ProjectSaveCancellationTests::RecoveryCancellationDoesNotConsumeManualTicket() {
    QTemporaryDir source, destination; EditorProject project; QVERIFY(Setup(project, source.path()));
    Gate gate{ProjectSaveStage::Write, kMiB}; auto control = std::make_shared<ProjectSaveControl>([&](auto stage, auto bytes) { gate.Progress(stage, bytes); });
    auto recovery = project.PrepareProjectSave(Path(destination.filePath("recovery.forge")), true, {}, control); QVERIFY(recovery.Succeeded());
    auto job = std::move(*recovery.value);
    std::jthread worker([&] { EditorProject::RunProjectSave(*job); }); ReleaseGate guard{gate};
    QVERIFY(gate.Await());
    auto manual = project.PrepareProjectSave(Path(destination.filePath("manual.forge"))); QVERIFY(manual.Succeeded());
    QVERIFY(!(*manual.value)->RequestCancellation());
    QVERIFY(project.RequestProjectSaveCancellation(*job)); gate.Release(); worker.join();
    QVERIFY(HasCode(project.CommitProjectSave(*job), kProjectSaveCancelledCode));
    QVERIFY(!project.editRevision_.BeginSave(EditSaveTarget::Project).Succeeded()); // 原 manual 票据仍由自己的任务持有。
    QVERIFY(project.CancelPreparedProjectSave(**manual.value).Succeeded());
    auto ticket = project.editRevision_.BeginSave(EditSaveTarget::Project); QVERIFY(ticket.Succeeded());
    QVERIFY(project.editRevision_.CompleteSave(*ticket.value, false).Succeeded());
}

void ProjectSaveCancellationTests::CancelledOldGenerationReleasesOnlyItsOwnedSource() {
    QTemporaryDir destination, nextSource; EditorProject project;
    auto owner = std::make_shared<QTemporaryDir>(); QVERIFY(owner->isValid()); const auto oldPath = owner->path();
    QVERIFY(Setup(project, oldPath)); project.mapWorkspace_ = owner; owner.reset();
    Gate gate{ProjectSaveStage::Read, kMiB}; auto control = std::make_shared<ProjectSaveControl>([&](auto stage, auto bytes) { gate.Progress(stage, bytes); });
    auto prepared = project.PrepareProjectSave(Path(destination.filePath("old.forge")), true, {}, control); QVERIFY(prepared.Succeeded());
    auto job = std::move(*prepared.value); const auto generation = job->Generation();
    std::jthread worker([&] { EditorProject::RunProjectSave(*job); }); ReleaseGate guard{gate}; QVERIFY(gate.Await());
    project.directorySnapshot_.reset(); project.projectRoot_.clear(); project.mapWorkspace_.reset();
    QVERIFY(QFileInfo::exists(oldPath)); // worker lease 保住自己读取中的旧来源。
    QVERIFY(Setup(project, nextSource.path())); QVERIFY(project.editRevision_.Generation() != generation);
    const auto nextSourceSha = Sha256File(Path(nextSource.filePath("source.bin"))); QVERIFY(nextSourceSha.Succeeded());
    auto ticket = project.editRevision_.BeginSave(EditSaveTarget::Project); QVERIFY(ticket.Succeeded());
    const auto nextScopes = project.editRevision_.Current();
    QVERIFY(project.RequestProjectSaveCancellation(*job)); gate.Release(); worker.join();
    QVERIFY(!QFileInfo::exists(oldPath)); // 最后 owner 与读取 HANDLE 在 worker 结束时释放。
    QVERIFY(HasCode(project.CommitProjectSave(*job), kProjectSaveCancelledCode));
    QCOMPARE(project.editRevision_.Current(), nextScopes); QVERIFY(QFileInfo::exists(nextSource.filePath("source.bin")));
    QCOMPARE(Sha256File(Path(nextSource.filePath("source.bin"))).value, nextSourceSha.value);
    QVERIFY(!project.editRevision_.BeginSave(EditSaveTarget::Project).Succeeded());
    QVERIFY(project.editRevision_.CompleteSave(*ticket.value, false).Succeeded());
    QVERIFY(!std::filesystem::exists(job->Target()));
}

void ProjectSaveCancellationTests::LateCancelDoesNotMisreportAndRecoveryNeverCleans() {
    QTemporaryDir source, destination; EditorProject project; QVERIFY(Setup(project, source.path()));
    bool attempted = false, accepted = true; ProjectSaveJob* active = nullptr;
    auto control = std::make_shared<ProjectSaveControl>([&](auto stage, auto) {
        if (stage == ProjectSaveStage::Publication) { attempted = true; accepted = active->RequestCancellation(); }
    });
    auto prepared = project.PrepareProjectSave(Path(destination.filePath("point.forge")), true, {}, control); QVERIFY(prepared.Succeeded());
    auto job = std::move(*prepared.value); active = job.get();
    EditorProject::RunProjectSave(*job);
    QVERIFY(attempted && !accepted && job->PublicationStarted() && !job->Cancelled());
    QVERIFY(job->Outcome().Succeeded()); QVERIFY(project.CommitProjectSave(*job).Succeeded());
    QVERIFY(project.editRevision_.IsDirty(EditSaveTarget::Project)); QVERIFY(project.projectFilePath_.empty());
    auto opened = MapArchiveSession::Open(job->Target()); QVERIFY(opened.Succeeded());
}

void ProjectSaveCancellationTests::PreparedCancelAndManualDefaultsStayCompatible() {
    QTemporaryDir source, destination; EditorProject project; QVERIFY(Setup(project, source.path()));
    auto cancelled = project.PrepareProjectSave(Path(destination.filePath("cancelled.forge")), true); QVERIFY(cancelled.Succeeded());
    QVERIFY(project.CancelPreparedProjectSave(**cancelled.value).Succeeded());
    EditorProject::RunProjectSave(**cancelled.value);
    QVERIFY((*cancelled.value)->Cancelled() && HasCode(project.CommitProjectSave(**cancelled.value), kProjectSaveCancelledCode));
    QVERIFY(!std::filesystem::exists((*cancelled.value)->Target()));
    QVERIFY(!project.PrepareProjectSave(Path(destination.filePath("invalid.forge")), false, {}, std::make_shared<ProjectSaveControl>()).Succeeded());
    const auto saved = project.SaveProjectToPath(Path(destination.filePath("manual.forge"))); QVERIFY(saved.Succeeded());
    QVERIFY(!project.editRevision_.IsDirty(EditSaveTarget::Project));
    QCOMPARE(project.projectFileSha256_, saved.value->sha256);
}

QTEST_GUILESS_MAIN(ProjectSaveCancellationTests)
#include "ProjectSaveCancellationTests.moc"
