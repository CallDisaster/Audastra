#include "ProjectSearchCapture.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Services/CodeCheckProject.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace War3Map;
namespace {
double Milliseconds(const QElapsedTimer& timer) { return static_cast<double>(timer.nsecsElapsed()) / 1000000.0; }
QString Utf8(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
QString FailureText(const std::vector<Diagnostic>& diagnostics) {
    QString output;
    for (std::size_t i = 0; i < (std::min)(diagnostics.size(), std::size_t{4}); ++i)
        output += QString::fromStdString(diagnostics[i].code + ": " + diagnostics[i].message) + QLatin1Char('\n');
    return output;
}
struct Receipt {
    QJsonObject data{{"schema", 1}, {"evidence", "Qt CPU ProjectSearchCapture and SearchProjectPage only"},
        {"passed", false}, {"main_window_verified", false}, {"gpu_verified", false},
        {"native_editor_verified", false}, {"runtime_dll_loaded", false}, {"map_copy_created", false}};
    std::map<QString, std::vector<double>> timings;
    ~Receipt() {
        QJsonObject operations;
        for (const auto& [name, samples] : timings) {
            auto sorted = samples; std::sort(sorted.begin(), sorted.end());
            QJsonArray values; for (const auto value : samples) values.push_back(value);
            operations.insert(name, QJsonObject{{"samples_ms", values}, {"count", static_cast<int>(samples.size())},
                {"maximum_ms", sorted.empty() ? 0.0 : sorted.back()},
                {"median_ms", sorted.empty() ? 0.0 : sorted[sorted.size() / 2]}});
        }
        data.insert("operations", operations);
        const auto json = QJsonDocument(data).toJson(QJsonDocument::Compact);
        std::fputs("PROJECT_SEARCH_CAPTURE_REAL_RECEIPT ", stdout);
        (void)std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
        std::fputc('\n', stdout); std::fflush(stdout);
    }
};
QJsonObject Stats(const QtApp::ProjectSearchCaptureStats& stats) {
    const auto n = [](auto value) { return QString::number(static_cast<qulonglong>(value)); };
    return {{"prepare_calls", n(stats.prepareCalls)}, {"type_preparations", n(stats.typePreparations)},
        {"type_cache_hits", n(stats.typeCacheHits)}, {"asset_preparations", n(stats.assetPreparations)},
        {"asset_cache_hits", n(stats.assetCacheHits)}, {"asset_merge_calls", n(stats.assetMergeCalls)},
        {"object_preparations", n(stats.objectPreparations)}, {"object_cache_hits", n(stats.objectCacheHits)},
        {"map_preparations", n(stats.mapPreparations)}, {"map_cache_hits", n(stats.mapCacheHits)},
        {"map_snapshot_calls", n(stats.mapSnapshotCalls)}, {"asset_input_rows", n(stats.assetInputRows)},
        {"asset_path_bytes_charged", n(stats.assetPathBytes)}, {"ambiguous_retained_assets", n(stats.ambiguousAssets)},
        {"catalog_sources", n(stats.catalogSourceRows)}, {"catalog_definitions", n(stats.catalogDefinitions)},
        {"catalog_bytes_charged", n(stats.catalogBytes)}, {"types_ready", stats.typesReady},
        {"catalog_manifest_sha256", QString::fromStdString(stats.catalogManifestSha256)},
        {"type_reason", QString::fromStdString(stats.typeReason)}, {"asset_state", static_cast<int>(stats.assetState)},
        {"asset_reason", QString::fromStdString(stats.assetReason)}, {"map_state", static_cast<int>(stats.mapState)},
        {"map_reason", QString::fromStdString(stats.mapReason)}, {"map_snapshot_raw_bytes_charged", n(stats.mapSnapshotBytesCharged)},
        {"map_unique_raw_bytes", n(stats.mapUniqueBytes)}};
}
QJsonArray Coverage(const Services::ProjectSearchPage& page) {
    QJsonArray result;
    for (const auto& item : page.coverage) result.push_back(QJsonObject{
        {"category", Utf8(Services::ProjectSearchCategoryName(item.category))},
        {"state", static_cast<int>(item.state)}, {"examined", QString::number(item.examined)},
        {"matches", QString::number(item.matches)}, {"scan_truncated", item.scanTruncated},
        {"results_truncated", item.resultsTruncated}});
    return result;
}
// 当前 typed 原始字节逐个散列；不复制归档 Body，均在实际 Prepare/query 计时外。
Result<std::string> DocumentDigest(const MapDocument& document) {
    std::string summary;
    for (const auto& member : document.Members()) {
        summary += std::to_string(static_cast<unsigned>(member.kind)) + ":" + member.memberName + ":"
            + std::to_string(static_cast<unsigned>(member.state)) + ":";
        for (const auto& source : member.sources) {
            auto hashed = Sha256(source.data); if (!hashed.Succeeded() || !hashed.value) return hashed;
            summary += source.name + ":" + std::to_string(source.data.size()) + ":" + *hashed.value + ";";
        }
    }
    return Sha256Text(summary);
}
struct UndoState {
    const QUndoStack* owner = nullptr;
    int count = 0, index = 0, clean = 0;
    bool canUndo = false, canRedo = false;
    bool operator==(const UndoState&) const = default;
};
std::vector<UndoState> UndoStates(const QtProject::EditorProject& project) {
    std::vector<UndoState> result;
    const auto add = [&result](const QUndoStack* stack) {
        if (stack) result.push_back({stack, stack->count(), stack->index(), stack->cleanIndex(), stack->canUndo(), stack->canRedo()});
    };
    add(project.mapDataUndoStack_); add(project.triggerUndoStack_); add(project.assetUndoStack_);
    for (const auto* stack : project.objectUndoStacks_) add(stack);
    return result;
}
// 与 Root staged ProjectSearchActions 同步供给方式相同；不执行地图，不补读声明基础库。
Result<Services::ProjectSearchPage> Query(QtApp::ProjectSearchCapture& capture,
    const QtProject::EditorProject& project, std::string_view revision, std::string_view query) {
    auto borrowed = capture.Borrow(project, revision);
    if (!borrowed.Succeeded() || !borrowed.value) return {std::nullopt, std::move(borrowed.diagnostics)};
    auto context = std::move(*borrowed.value);
    context.triggers = project.triggerDocument_.get();
    if (context.triggers) context.triggerState = Services::ProjectSearchState::Ready;
    const auto code = project.CurrentCodeCheckIndex(); // 实际accepted binding gate，费用计入查询。
    if (code) {
        context.codeDeclarations = &code->declarations; context.codeBinding = code->contextBinding;
        context.codeState = code->contextComplete && code->declarations.completed && code->externalSourcesComplete
            ? Services::ProjectSearchState::Ready : Services::ProjectSearchState::Partial;
    }
    // 未准备 standard catalog，明示Unavailable；不在查询内自动打开游戏MPQ或安装目录。
    Services::ProjectSearchOptions options; options.limit = 128;
    return Services::SearchProjectPage(context, revision, query, options, 40);
}
} // namespace

class ProjectSearchCaptureRealTests final : public QObject {
    Q_OBJECT
public:
    ProjectSearchCaptureRealTests(QString map, QString ui, double limit)
        : map_(std::move(map)), ui_(std::move(ui)), limitMs_(limit) {}
private slots:
    void OwnedLargeMapColdPrepareAndThreeWarmQueries();
private:
    QString map_, ui_;
    double limitMs_ = 1000;
};

void ProjectSearchCaptureRealTests::OwnedLargeMapColdPrepareAndThreeWarmQueries() {
    Receipt receipt;
    receipt.data.insert("operation_limit_ms", limitMs_); receipt.data.insert("timing_budget_enforced", true);
    receipt.data.insert("warm_query_count", 3); receipt.data.insert("stage", "map-open");
    const QFileInfo source(map_), uiSource(ui_);
    QVERIFY2(source.isAbsolute() && source.isFile() && source.size() >= 300ll * 1024 * 1024,
        "FORGE_NAVIGATION_OWNED_MAP must be an existing absolute author-owned large map copy (at least 300 MiB)");
    QVERIFY2(uiSource.isAbsolute() && uiSource.isDir(), "FORGE_YDWE_ROOT must be an existing absolute readonly UI catalog source");
    receipt.data.insert("sample_bytes", QString::number(source.size()));
    QElapsedTimer timer; timer.start();
    auto opened = MapArchiveSession::Open(std::filesystem::path(map_.toStdWString()));
    receipt.data.insert("map_open_ms", Milliseconds(timer));
    QVERIFY2(opened.Succeeded() && opened.value, qPrintable(FailureText(opened.diagnostics)));
    const auto sourceSha = opened.value->SourceSha256(); receipt.data.insert("sample_sha256", QString::fromStdString(sourceSha));
    receipt.data.insert("archive_has_opaque_members", opened.value->HasOpaqueMembers());
    receipt.data.insert("archive_named_members", QString::number(opened.value->Members().size()));
    std::uint64_t decodedBytes = 0; for (const auto& member : opened.value->Members()) decodedBytes += member.data.size();
    receipt.data.insert("archive_owned_decoded_bytes", QString::number(decodedBytes));

    receipt.data.insert("stage", "explicit-source-capture"); timer.restart();
    const std::vector<std::string> names{"war3map.wtg", "war3map.wct", "war3map.j",
        "war3map.w3u", "war3map.w3t", "war3map.w3a", "war3map.w3b", "war3map.w3d", "war3map.w3h", "war3map.w3q"};
    auto members = opened.value->CaptureExactMembers(names);
    receipt.data.insert("exact_source_capture_ms", Milliseconds(timer));
    QVERIFY2(members.Succeeded() && members.value && members.value->size() == names.size(), qPrintable(FailureText(members.diagnostics)));
    const auto& wtg = (*members.value)[0]; const auto& wct = (*members.value)[1]; const auto& script = (*members.value)[2];
    QVERIFY2(wtg.present && wct.present && script.present && !script.bytes.empty(),
        "This case requires actual WTG/WCT and war3map.j; missing sources are not replaced by synthetic code");
    const auto scriptSha = Sha256(script.bytes); QVERIFY(scriptSha.Succeeded() && scriptSha.value);
    receipt.data.insert("map_script_sha256", QString::fromStdString(*scriptSha.value));
    receipt.data.insert("map_script_bytes", QString::number(script.bytes.size()));

    receipt.data.insert("stage", "readonly-ui-and-typed-bind"); timer.restart();
    auto catalog = Trigger::UiCatalog::Load(std::filesystem::path(ui_.toStdWString()));
    QVERIFY2(catalog.Succeeded() && catalog.value, qPrintable(FailureText(catalog.diagnostics)));
    auto parsed = Trigger::WtgWctCodec{}.Parse(wtg.bytes, wct.bytes, *catalog.value);
    QVERIFY2(parsed.Succeeded() && parsed.value, qPrintable(FailureText(parsed.diagnostics)));
    QtProject::EditorProject project;
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath(); project.projectRoot_ = project.sourceArchivePath_.parent_path();
    project.activeUiDataRoot_ = project.uiDataRoot_ = std::filesystem::path(ui_.toStdWString());
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    // NativeFileName定义的真实类别顺序，不把7个文件人为当作standard catalog。
    for (std::size_t i = 0; i < 7; ++i) {
        const auto type = static_cast<ObjectData::ObjectType>(i);
        const auto name = ObjectData::NativeFileName(type);
        const auto found = std::find_if(members.value->begin() + 3, members.value->end(), [&](const auto& member) { return member.name == name; });
        QVERIFY(found != members.value->end());
        if (!found->present) continue;
        auto native = ObjectData::NativeObjectCodec(type).Parse(found->bytes);
        QVERIFY2(native.Succeeded() && native.document, qPrintable(FailureText(native.diagnostics)));
        project.nativeObjectDocuments_[i].reset(dynamic_cast<ObjectData::NativeObjectDocument*>(native.document.release()));
        QVERIFY(project.nativeObjectDocuments_[i]);
    }
    project.ResetEditTracking(); QVERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    receipt.data.insert("source_bind_ms", Milliseconds(timer));
    QVERIFY(project.triggerDocument_->HasTrustedSourceSeal() && !project.triggerDocument_->triggers.empty());
    receipt.data.insert("ui_catalog_sha256", QString::fromStdString(project.uiCatalog_->Sha256()));
    receipt.data.insert("wtg_sha256", Utf8(project.triggerDocument_->SourceWtgSha256()));
    receipt.data.insert("wct_sha256", Utf8(project.triggerDocument_->SourceWctSha256()));
    receipt.data.insert("actual_triggers", QString::number(project.triggerDocument_->triggers.size()));
    receipt.data.insert("actual_arguments", QString::number(project.triggerDocument_->argumentNodes.size()));

    receipt.data.insert("stage", "owned-code-index-setup"); timer.restart();
    // 实际共同检查器只读当前map JASS。缺common/blizzard，索引资格明确Partial，不执行代码或运行库。
    const auto snapshot = project.CaptureCodeCheckSnapshot(); QVERIFY(snapshot && !snapshot->binding.empty());
    Services::CodeCheckRequest request; request.language = Services::CodeLanguage::Jass;
    request.source = {"war3map.j", std::string(reinterpret_cast<const char*>(script.bytes.data()), script.bytes.size())};
    request.context.binding = snapshot->binding; request.context.complete = false;
    request.context.reason = "Actual owned map declarations; common/blizzard and external sources not prepared";
    request.generation = project.editRevision_.Generation(); request.jass.threads = 1;
    request.jass.maxSourceWorkers = request.jass.maxBodyWorkers = 1;
    auto checked = std::make_shared<const Services::CodeCheckResult>(Services::CheckCode(request));
    project.AdoptCodeCheckIndex(checked);
    const auto code = project.CurrentCodeCheckIndex();
    receipt.data.insert("code_index_setup_ms", Milliseconds(timer));
    QVERIFY2(code && code.get() == checked.get() && code->completed && code->declarations.completed
        && !code->declarations.symbols.empty(), "Actual CheckCode declaration index was not accepted with current project binding");
    QVERIFY(!code->contextComplete && !code->semanticChecked);
    receipt.data.insert("code_state", "Partial: actual map declarations only, no common/blizzard semantic claim");
    receipt.data.insert("code_binding", QString::fromStdString(code->contextBinding));
    receipt.data.insert("actual_code_declarations", QString::number(code->declarations.symbols.size()));
    std::string query;
    for (const auto& symbol : code->declarations.symbols)
        if (symbol.name.size() >= 2 && symbol.name.size() <= 128 && symbol.source == "war3map.j") { query = symbol.name; break; }
    QVERIFY2(!query.empty(), "No actual map declaration name suitable for a bounded representative query");
    receipt.data.insert("query", QString::fromStdString(query));

    const auto originalDigest = DocumentDigest(*project.mapDocument_); QVERIFY(originalDigest.Succeeded() && originalDigest.value);
    const auto originalEncoded = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY2(originalEncoded.Succeeded() && originalEncoded.value, qPrintable(FailureText(originalEncoded.diagnostics)));
    QVERIFY(originalEncoded.value->wtg == wtg.bytes && originalEncoded.value->wct == wct.bytes);
    const auto originalUndo = UndoStates(project);
    const auto originalScopes = project.editRevision_.Current();
    const auto generation = project.editRevision_.Generation(), revisionNumber = project.editRevision_.Revision();
    const auto mapRevision = project.mapEdits_->Revision(), sessionIdentity = project.mapEdits_->SessionIdentity();
    const auto journalSize = project.editRevision_.Journal().size();
    const auto* const archiveOwner = project.archiveSession_.get(); const auto* const mapOwner = project.mapDocument_.get();
    const auto* const editOwner = project.mapEdits_.get(); const auto* const triggerOwner = project.triggerDocument_.get();
    const std::string revision = "owned-search/" + std::to_string(generation) + "/" + std::to_string(revisionNumber) + "/" + sourceSha;
    QtApp::ProjectSearchCapture capture;

    receipt.data.insert("stage", "first-prepare"); timer.restart();
    const auto cold = capture.Prepare(project, "owned-search-real", generation, revision);
    receipt.timings["first_prepare"].push_back(Milliseconds(timer)); receipt.data.insert("cold_stats", Stats(capture.Stats()));
    QVERIFY2(cold.Succeeded(), qPrintable(FailureText(cold.diagnostics)));
    const auto borrowed = capture.Borrow(project, revision); QVERIFY(borrowed.Succeeded() && borrowed.value && borrowed.value->mapData);
    QVERIFY(capture.Stats().typesReady);
    QCOMPARE(borrowed.value->mapDataBinding.sourceSha256, sourceSha); QCOMPARE(borrowed.value->mapDataBinding.generation, generation);
    QCOMPARE(borrowed.value->mapDataBinding.revision, mapRevision);
    const auto* const mapSnapshot = borrowed.value->mapData; const auto* const types = borrowed.value->types;
    const auto preparedDocument = mapSnapshot->OwnedDocument(); QVERIFY(preparedDocument);
    const auto preparedDigest = DocumentDigest(*preparedDocument); QVERIFY(preparedDigest.Succeeded() && preparedDigest.value);
    QCOMPARE(*preparedDigest.value, *originalDigest.value); // 正文SHA核对不计入Prepare或query。

    receipt.data.insert("stage", "first-query"); timer.restart();
    const auto firstPage = Query(capture, project, revision, query);
    receipt.timings["first_borrow_query"].push_back(Milliseconds(timer));
    QVERIFY2(firstPage.Succeeded() && firstPage.value, qPrintable(FailureText(firstPage.diagnostics)));
    receipt.data.insert("coverage", Coverage(*firstPage.value)); receipt.data.insert("window_sha256", QString::fromStdString(firstPage.value->windowSha256));
    receipt.data.insert("page_rows", QString::number(firstPage.value->rows.size()));
    QVERIFY(std::any_of(firstPage.value->rows.begin(), firstPage.value->rows.end(), [&](const auto& row) {
        return row.category == Services::ProjectSearchCategory::CodeSymbol && row.codeSource == std::optional<std::string>{"war3map.j"}
            && row.codeBinding == code->contextBinding && row.contextRevision == revision;
    }));
    const auto coldStats = capture.Stats();
    receipt.data.insert("stage", "three-warm-queries");
    for (int i = 0; i < 3; ++i) {
        QElapsedTimer combined; combined.start(); timer.restart();
        const auto prepared = capture.Prepare(project, "owned-search-real", generation, revision);
        receipt.timings["warm_prepare"].push_back(Milliseconds(timer));
        timer.restart(); const auto page = Query(capture, project, revision, query);
        receipt.timings["warm_borrow_query"].push_back(Milliseconds(timer));
        receipt.timings["warm_total_prepare_borrow_query"].push_back(Milliseconds(combined));
        QVERIFY2(prepared.Succeeded(), qPrintable(FailureText(prepared.diagnostics)));
        QVERIFY2(page.Succeeded() && page.value, qPrintable(FailureText(page.diagnostics)));
        QCOMPARE(page.value->windowSha256, firstPage.value->windowSha256); QCOMPARE(page.value->rows.size(), firstPage.value->rows.size());
        const auto current = capture.Borrow(project, revision); QVERIFY(current.Succeeded() && current.value);
        QCOMPARE(current.value->mapData, mapSnapshot); QCOMPARE(current.value->types, types);
    }
    const auto& stats = capture.Stats(); receipt.data.insert("warm_stats", Stats(stats));
    QCOMPARE(stats.typePreparations, coldStats.typePreparations); QCOMPARE(stats.assetPreparations, coldStats.assetPreparations);
    QCOMPARE(stats.objectPreparations, coldStats.objectPreparations); QCOMPARE(stats.mapPreparations, coldStats.mapPreparations);
    QCOMPARE(stats.assetMergeCalls, coldStats.assetMergeCalls); QCOMPARE(stats.mapSnapshotCalls, coldStats.mapSnapshotCalls);
    QCOMPARE(stats.typeCacheHits, coldStats.typeCacheHits + 3); QCOMPARE(stats.assetCacheHits, coldStats.assetCacheHits + 3);
    QCOMPARE(stats.objectCacheHits, coldStats.objectCacheHits + 3); QCOMPARE(stats.mapCacheHits, coldStats.mapCacheHits + 3);

    receipt.data.insert("stage", "untimed-source-and-document-reverification"); timer.restart();
    QVERIFY(project.archiveSession_.get() == archiveOwner && project.mapDocument_.get() == mapOwner && project.mapEdits_.get() == editOwner
        && project.triggerDocument_.get() == triggerOwner);
    QCOMPARE(project.mapEdits_->SessionIdentity(), sessionIdentity); QCOMPARE(project.mapEdits_->Revision(), mapRevision);
    QCOMPARE(project.editRevision_.Generation(), generation); QCOMPARE(project.editRevision_.Revision(), revisionNumber);
    QVERIFY(project.editRevision_.Current() == originalScopes && project.editRevision_.Journal().size() == journalSize);
    QVERIFY(UndoStates(project) == originalUndo); QVERIFY(project.pendingImports_.empty() && project.assetEdits_.empty());
    const auto afterDigest = DocumentDigest(*project.mapDocument_); QVERIFY(afterDigest.Succeeded() && afterDigest.value);
    QCOMPARE(*afterDigest.value, *originalDigest.value);
    const auto afterEncoded = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(afterEncoded.Succeeded() && afterEncoded.value && afterEncoded.value->wtg == originalEncoded.value->wtg
        && afterEncoded.value->wct == originalEncoded.value->wct);
    const auto sourceUnchanged = project.archiveSession_->VerifySourceUnchanged();
    receipt.data.insert("source_and_document_reverification_ms", Milliseconds(timer));
    QVERIFY2(sourceUnchanged.Succeeded(), qPrintable(FailureText(sourceUnchanged.diagnostics)));
    receipt.data.insert("source_unchanged", true); receipt.data.insert("document_unchanged", true); receipt.data.insert("undo_unchanged", true);
    receipt.data.insert("typed_document_sha256", QString::fromStdString(*originalDigest.value));
    receipt.data.insert("stage", "timing-budget");
    for (const auto& [name, samples] : receipt.timings) {
        QVERIFY(!samples.empty());
        QVERIFY2(*std::max_element(samples.begin(), samples.end()) <= limitMs_,
            qPrintable(name + QStringLiteral(" exceeded FORGE_PROJECT_SEARCH_MAX_OPERATION_MS; maximum, not mean, is the gate")));
    }
    receipt.data.insert("stage", "complete"); receipt.data.insert("passed", true);
}

int main(int argc, char** argv) {
    const auto map = qEnvironmentVariable("FORGE_NAVIGATION_OWNED_MAP"), ui = qEnvironmentVariable("FORGE_YDWE_ROOT");
    if (map.isEmpty() || ui.isEmpty()) {
        std::fputs("PROJECT_SEARCH_CAPTURE_REAL_RECEIPT {\"schema\":1,\"skipped\":true,\"reason\":\"FORGE_NAVIGATION_OWNED_MAP and FORGE_YDWE_ROOT are required readonly source environments\"}\n", stdout);
        return 77;
    }
    double limit = 1000; const auto configured = qEnvironmentVariable("FORGE_PROJECT_SEARCH_MAX_OPERATION_MS");
    if (!configured.isEmpty()) {
        bool valid = false; limit = configured.toDouble(&valid);
        if (!valid || !std::isfinite(limit) || limit <= 0 || limit > 1000) {
            std::fputs("FORGE_PROJECT_SEARCH_MAX_OPERATION_MS may only tighten the finite positive 1000 ms gate\n", stderr); return 2;
        }
    }
    QTemporaryDir settings(QDir::tempPath() + QStringLiteral("/AudastraProjectSearchReal-XXXXXX"));
    if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ProjectSearchCaptureRealTests"));
    QCoreApplication application(argc, argv); // 无QApplication、Widget、plugin或图形后端。
    ProjectSearchCaptureRealTests tests(map, ui, limit); return QTest::qExec(&tests, argc, argv);
}
#include "ProjectSearchCaptureRealTests.moc"
