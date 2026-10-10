#include "ObjectColourPreviewSource.h"
#include "EditorProject.h"
#include "Widgets/WarcraftTextPreview.h"
#include "Theme/ThemeManager.h"
#include "Fonts/FontLoader.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Text/WarcraftText.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

namespace {
using namespace War3Map;
using UiKit::WarcraftTextSourceState;
QString DisplayUtf8(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
double Milliseconds(const QElapsedTimer& timer) { return static_cast<double>(timer.nsecsElapsed()) / 1000000.0; }
QString Diagnostics(const std::vector<Diagnostic>& diagnostics) {
    QString result;
    for (std::size_t i = 0; i < (std::min)(diagnostics.size(), std::size_t{4}); ++i)
        result += DisplayUtf8(diagnostics[i].code + ": " + diagnostics[i].message).left(1024) + QLatin1Char('\n');
    return result;
}
QJsonValue WorkingSet(bool peak = false) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{}; counters.cb = static_cast<DWORD>(sizeof(counters));
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        static_cast<DWORD>(sizeof(counters)))) return QString::number(static_cast<qulonglong>(
            peak ? counters.PeakWorkingSetSize : counters.WorkingSetSize));
#else
    (void)peak;
#endif
    return QJsonValue(QJsonValue::Null);
}
struct WtsReceipt {
    QJsonObject data{{"schema", 1}, {"evidence", "real owned map WTS, production readonly capture/N1 and Qt preview, CPU only"},
        {"passed", false}, {"source_written", false}, {"map_copy_created", false}, {"capture_calls", 0},
        {"cold_open_in_foreground_gate", false}, {"main_window_verified", false}, {"gpu_verified", false},
        {"native_editor_verified", false}, {"game_verified", false}, {"real_gbk_verified", false}};
    std::map<QString, std::vector<double>> timings;
    ~WtsReceipt() {
        QJsonObject stages;
        for (const auto& [name, samples] : timings) {
            QJsonArray values; for (const auto value : samples) values.push_back(value);
            stages.insert(name, QJsonObject{{"samples_ms", values}, {"count", static_cast<int>(samples.size())},
                {"maximum_ms", samples.empty() ? 0.0 : *std::max_element(samples.begin(), samples.end())}});
        }
        data.insert("foreground_operations", stages); data.insert("rss_final_bytes", WorkingSet());
        data.insert("process_peak_rss_bytes_including_cold_open", WorkingSet(true));
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
        std::fputs("OBJECT_COLOUR_PREVIEW_REAL_RECEIPT ", stdout);
        (void)std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
        std::fputc('\n', stdout); std::fflush(stdout);
    }
};
void Failure(WtsReceipt& receipt, const QString& message) {
    const auto bounded = (receipt.data.value("stage").toString() + QStringLiteral(": ") + message).left(4096);
    receipt.data.insert("failure_reason", bounded); const auto text = bounded.toUtf8();
    std::fputs("OBJECT_COLOUR_PREVIEW_REAL_FAILURE ", stdout);
    (void)std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stdout);
    std::fputc('\n', stdout); std::fflush(stdout);
}
template<class Callback> auto Measure(WtsReceipt& receipt, const QString& name, Callback&& callback) {
    receipt.data.insert("stage", name); QElapsedTimer timer; timer.start();
    if constexpr (std::is_void_v<std::invoke_result_t<Callback>>) {
        std::invoke(std::forward<Callback>(callback));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        receipt.timings[name].push_back(Milliseconds(timer));
    } else {
        auto result = std::invoke(std::forward<Callback>(callback));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        receipt.timings[name].push_back(Milliseconds(timer)); return result;
    }
}
struct ActualEntry {
    std::uint32_t id = 0;
    std::size_t index = 0, rawBytes = 0;
    Trigger::TextEncoding encoding = Trigger::TextEncoding::Utf8;
    QString decoded;
    std::string visible;
};
QString EncodingName(Trigger::TextEncoding encoding) {
    switch (encoding) {
    case Trigger::TextEncoding::Utf8: return QStringLiteral("UTF8");
    case Trigger::TextEncoding::Utf8Bom: return QStringLiteral("UTF8-BOM");
    case Trigger::TextEncoding::Gb18030: return QStringLiteral("GB18030 (production GBK-compatible decoder)");
    case Trigger::TextEncoding::Latin1: return QStringLiteral("Latin1 preserved-byte fallback");
    }
    return QStringLiteral("Unknown");
}
// 只从已经载入的真实表挑唯一身份；不制造正文。准备费用独立于待测产品 Capture。
std::vector<ActualEntry> SelectActualEntries(const MapData::StringTableDocument& table, QJsonObject& statistics) {
    constexpr std::size_t maxRecords = 65536, maxSource = 16U * 1024U * 1024U, maxBody = 1024U * 1024U;
    if (table.entries.size() > maxRecords) return {};
    std::size_t rawBytes = 0;
    std::map<std::uint32_t, std::size_t> counts;
    for (const auto& entry : table.entries) {
        if (entry.value.size() > maxSource - rawBytes) return {};
        rawBytes += entry.value.size(); ++counts[entry.id];
    }
    statistics.insert("parsed_records", QString::number(table.entries.size()));
    statistics.insert("raw_body_bytes_charged", QString::number(rawBytes));
    std::optional<ActualEntry> largest, gbk;
    std::size_t gbkRecords = 0, budgetExcluded = 0;
    for (std::size_t index = 0; index < table.entries.size(); ++index) {
        const auto& entry = table.entries[index];
        if (counts.at(entry.id) != 1 || entry.value.empty()) continue;
        if (entry.value.size() > maxBody) { ++budgetExcluded; continue; }
        const auto decoded = Trigger::DecodeSourceText(std::as_bytes(std::span(entry.value.data(), entry.value.size())));
        if (decoded.encoding == Trigger::TextEncoding::Gb18030) ++gbkRecords;
        const auto text = DisplayUtf8(decoded.text);
        if (text.size() > 256 * 1024 || decoded.text.size() > maxBody) { ++budgetExcluded; continue; }
        const auto parsed = Text::ParseWarcraftText(decoded.text);
        if (!parsed.Complete()) { ++budgetExcluded; continue; }
        ActualEntry selected{entry.id, index, entry.value.size(), decoded.encoding, text, parsed.plainText};
        if (!largest || selected.rawBytes > largest->rawBytes) largest = selected;
        if (selected.encoding == Trigger::TextEncoding::Gb18030 && (!gbk || selected.rawBytes > gbk->rawBytes)) gbk = std::move(selected);
    }
    statistics.insert("unique_gbk_compatible_records_observed", QString::number(gbkRecords));
    statistics.insert("preview_budget_or_parser_excluded_records", QString::number(budgetExcluded));
    std::vector<ActualEntry> result;
    if (largest) result.push_back(*largest);
    if (gbk && (!largest || gbk->id != largest->id)) result.push_back(*gbk);
    return result;
}
} // namespace

#define WTS_VERIFY2(condition, message) do { \
    const bool observedPassed = static_cast<bool>(condition); \
    if (!observedPassed) Failure(receipt, QString::fromUtf8(message)); \
    QVERIFY2(observedPassed, message); \
} while (false)
#define WTS_VERIFY(condition) WTS_VERIFY2(condition, #condition)

static void InitializeStyleResources() { Q_INIT_RESOURCE(war3map_uikit_style); }

class ObjectColourPreviewRealTests final : public QObject {
    Q_OBJECT
public:
    ObjectColourPreviewRealTests(QString map, QString sha, double limit)
        : map_(std::move(map)), sha_(std::move(sha)), limitMs_(limit) {}
private slots:
    void OwnedMapOneCaptureWarmResolverAndDebouncedPreview();
private:
    QString map_, sha_;
    double limitMs_ = 1000;
};

void ObjectColourPreviewRealTests::OwnedMapOneCaptureWarmResolverAndDebouncedPreview() {
    using namespace War3Map;
    WtsReceipt receipt; receipt.data.insert("stage", "readonly-owned-map-open");
    receipt.data.insert("operation_limit_ms", limitMs_); receipt.data.insert("rss_initial_bytes", WorkingSet());
    receipt.data.insert("timing_gate", "maximum of each post-bind foreground stage; no mean gate");
    receipt.data.insert("snapshot_scope", "one production current Strings Snapshot in Capture; no test Snapshot probe");
    const QFileInfo source(map_);
    WTS_VERIFY2(source.isAbsolute() && source.isFile() && source.size() >= 300ll * 1024 * 1024,
        "FORGE_NAVIGATION_OWNED_MAP must name an existing absolute owned map copy of at least300MiB");
    receipt.data.insert("map_bytes", QString::number(source.size()));
    QElapsedTimer setup; setup.start();
    auto opened = MapArchiveSession::Open(std::filesystem::path(map_.toStdWString()));
    receipt.data.insert("cold_map_open_ms", Milliseconds(setup));
    WTS_VERIFY2(opened.Succeeded() && opened.value, qPrintable(Diagnostics(opened.diagnostics)));
    const auto mapSha = opened.value->SourceSha256(); receipt.data.insert("map_sha256", DisplayUtf8(mapSha));
    if (!sha_.isEmpty()) WTS_VERIFY(DisplayUtf8(mapSha) == sha_);
    setup.restart(); receipt.data.insert("stage", "accepted-readonly-map-data-bind");
    QtProject::EditorProject project;
    project.archiveSession_ = std::make_shared<MapArchiveSession>(std::move(*opened.value));
    project.sourceArchivePath_ = project.archiveSession_->SourcePath(); project.projectRoot_ = project.sourceArchivePath_.parent_path();
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);
    WTS_VERIFY(project.mapDataUndoStack_ && project.AssetUndoStack()); project.ResetEditTracking();
    WTS_VERIFY(project.editTrackingReady_ && !project.editTrackingFailed_);
    receipt.data.insert("map_data_bind_ms_outside_foreground_gate", Milliseconds(setup));
    const auto& member = project.mapDocument_->Member(MapDataKind::Strings);
    WTS_VERIFY(project.mapDocument_->Strings() && member.sources.size() == 1
        && (member.state == MapDataState::Ready || member.state == MapDataState::Partial));
    const auto& table = *project.mapDocument_->Strings();
    const auto wtsSha = Sha256(member.sources.front().data); WTS_VERIFY(wtsSha.Succeeded() && wtsSha.value);
    const auto encoded = table.Serialize(); WTS_VERIFY(encoded.Succeeded() && encoded.value && *encoded.value == member.sources.front().data);
    receipt.data.insert("wts_source_sha256", DisplayUtf8(*wtsSha.value)); receipt.data.insert("wts_source_bytes", QString::number(member.sources.front().data.size()));
    receipt.data.insert("wts_member_state", member.state == MapDataState::Ready ? "Ready" : "Partial");
    const auto generation = project.editRevision_.Generation(), revision = project.editRevision_.Revision();
    const auto session = project.mapEdits_->SessionIdentity(), mapRevision = project.mapEdits_->Revision();
    receipt.data.insert("captured_source_stamp", QJsonObject{{"edit_generation", QString::number(generation)},
        {"edit_revision", QString::number(revision)}, {"map_session", QString::number(session)}, {"map_revision", QString::number(mapRevision)}});
    receipt.data.insert("product_limits", QJsonObject{{"source_bytes", "16777216"}, {"records", "65536"},
        {"single_body_bytes", "1048576"}, {"editor_utf16_units", "262144"}});
    const auto mapUndoCount = project.mapDataUndoStack_->count(), mapUndoIndex = project.mapDataUndoStack_->index();
    const auto assetUndoCount = project.AssetUndoStack()->count(), assetUndoIndex = project.AssetUndoStack()->index();
    setup.restart(); receipt.data.insert("stage", "select-current-real-wts-identities");
    QJsonObject selectionStatistics; const auto selected = SelectActualEntries(table, selectionStatistics);
    receipt.data.insert("selection", selectionStatistics); receipt.data.insert("selection_ms_outside_foreground_gate", Milliseconds(setup));
    WTS_VERIFY2(!selected.empty(), "No unique nonempty actual current WTS entry fits the unchanged production preview limits");
    receipt.data.insert("rss_before_capture_bytes", WorkingSet());
    auto resolver = Measure(receipt, QStringLiteral("current_wts_capture_once"), [&] {
        receipt.data.insert("capture_calls", 1); return Modules::CaptureObjectColourPreviewSource(project);
    });
    WTS_VERIFY(static_cast<bool>(resolver)); receipt.data.insert("rss_after_capture_bytes", WorkingSet());
    QJsonArray observedEntries; bool realGbkVerified = false; std::size_t resolverCalls = 0;
    for (const auto& entry : selected) {
        const auto reference = QStringLiteral("TRIGSTR_") + QString::number(entry.id);
        const auto stageSuffix = QStringLiteral("_") + QString::number(entry.id);
        for (int sample = 0; sample < 3; ++sample) {
            const auto resolved = Measure(receipt, QStringLiteral("warm_resolver") + stageSuffix, [&] { return resolver(reference); });
            WTS_VERIFY2(resolved.state == WarcraftTextSourceState::Available, qPrintable(resolved.explanation));
            WTS_VERIFY(resolved.text == entry.decoded);
        }
        QPlainTextEdit draft; draft.setPlainText(reference);
        UiKit::WarcraftTextPreview preview; preview.resize(800, 340);
        const auto watchedResolver = [&](const QString& text) { ++resolverCalls; return resolver(text); };
        const auto beforeBind = resolverCalls;
        Measure(receipt, QStringLiteral("widget_bind_initial_refresh") + stageSuffix, [&] { preview.Bind(&draft, {}, true, watchedResolver); });
        WTS_VERIFY(resolverCalls == beforeBind + 1);
        WTS_VERIFY(preview.Preview()->toPlainText() == DisplayUtf8(entry.visible));
        for (int sample = 0; sample < 3; ++sample) {
            Measure(receipt, QStringLiteral("widget_warm_refresh") + stageSuffix, [&] { preview.Refresh(); });
            WTS_VERIFY(preview.Preview()->toPlainText() == DisplayUtf8(entry.visible)); WTS_VERIFY(draft.toPlainText() == reference);
        }
        for (int sample = 0; sample < 3; ++sample) {
            const auto beforeDebounce = resolverCalls;
            const auto completed = Measure(receipt, QStringLiteral("widget_debounced_refresh_including100ms") + stageSuffix, [&] {
                draft.setPlainText(QLatin1Char('"') + reference + QLatin1Char('"')); draft.setPlainText(reference);
                QElapsedTimer deadline; deadline.start();
                while (resolverCalls == beforeDebounce && Milliseconds(deadline) <= limitMs_) QTest::qWait(1);
                return resolverCalls == beforeDebounce + 1;
            });
            WTS_VERIFY2(completed, "Real single-shot debounce did not produce exactly one resolver/Refresh within its unchanged finite gate");
            WTS_VERIFY(preview.Preview()->toPlainText() == DisplayUtf8(entry.visible)); WTS_VERIFY(draft.toPlainText() == reference);
        }
        realGbkVerified = realGbkVerified || entry.encoding == Trigger::TextEncoding::Gb18030;
        observedEntries.push_back(QJsonObject{{"wts_id", QString::number(entry.id)}, {"raw_body_bytes", QString::number(entry.rawBytes)},
            {"decoded_utf16_units", static_cast<double>(entry.decoded.size())}, {"visible_utf8_bytes", QString::number(entry.visible.size())},
            {"source_encoding", EncodingName(entry.encoding)}, {"identity_source", "actual unique current WTS parsed entry"}});
    }
    receipt.data.insert("observed_entries", observedEntries); receipt.data.insert("real_gbk_verified", realGbkVerified);
    receipt.data.insert("gbk_evidence_boundary", realGbkVerified ? "actual selected map WTS GB18030/GBK bytes decoded by production N1"
        : "no admissible actual GBK entry selected; existing original GBK unit fixture is separate evidence, never real-map proof");
    const auto reference = QStringLiteral("TRIGSTR_") + QString::number(selected.front().id);
    QPlainTextEdit draft; draft.setPlainText(reference); UiKit::WarcraftTextPreview preview;
    Measure(receipt, QStringLiteral("source_invalidation_fixture_bind"), [&] { preview.Bind(&draft, {}, true, resolver); });
    const auto previous = preview.Preview()->toPlainText(); WTS_VERIFY(previous == DisplayUtf8(selected.front().visible));
    project.sourceChangeSuspected_ = true; // 只改本测试工程状态，不写源图、配置或运行库。
    const auto stale = Measure(receipt, QStringLiteral("source_invalidated_resolver"), [&] { return resolver(reference); });
    WTS_VERIFY(stale.state == WarcraftTextSourceState::Stale);
    Measure(receipt, QStringLiteral("source_invalidated_widget_refresh"), [&] { preview.Refresh(); });
    WTS_VERIFY(preview.StatusText().contains(QStringLiteral("失效")) && preview.Preview()->toPlainText() == previous);
    WTS_VERIFY(draft.toPlainText() == reference);
    project.sourceChangeSuspected_ = false;
    const auto stillStale = Measure(receipt, QStringLiteral("cleared_flag_does_not_revive_old_source"), [&] { return resolver(reference); });
    WTS_VERIFY(stillStale.state == WarcraftTextSourceState::Stale);
    receipt.data.insert("stage", "readonly-source-and-history-closure");
    WTS_VERIFY(project.editRevision_.Generation() == generation && project.editRevision_.Revision() == revision);
    WTS_VERIFY(project.mapEdits_->SessionIdentity() == session && project.mapEdits_->Revision() == mapRevision && !project.mapEdits_->IsDirty());
    WTS_VERIFY(project.mapDataUndoStack_->count() == mapUndoCount && project.mapDataUndoStack_->index() == mapUndoIndex);
    WTS_VERIFY(project.AssetUndoStack()->count() == assetUndoCount && project.AssetUndoStack()->index() == assetUndoIndex);
    const auto finalEncoded = table.Serialize(); WTS_VERIFY(finalEncoded.Succeeded() && finalEncoded.value && *finalEncoded.value == member.sources.front().data);
    const auto finalWtsSha = Sha256(member.sources.front().data); WTS_VERIFY(finalWtsSha.Succeeded() && finalWtsSha.value && *finalWtsSha.value == *wtsSha.value);
    setup.restart(); const auto unchanged = project.archiveSession_->VerifySourceUnchanged();
    receipt.data.insert("final_source_sha_verify_ms_outside_foreground_gate", Milliseconds(setup));
    WTS_VERIFY2(unchanged.Succeeded(), qPrintable(Diagnostics(unchanged.diagnostics)));
    receipt.data.insert("source_unchanged", true); receipt.data.insert("undo_unchanged", true); receipt.data.insert("wts_raw_roundtrip_exact", true);
    receipt.data.insert("stage", "maximum-latency-gate");
    for (const auto& [name, samples] : receipt.timings) {
        WTS_VERIFY(!samples.empty());
        WTS_VERIFY2(*std::max_element(samples.begin(), samples.end()) <= limitMs_,
            qPrintable(name + QStringLiteral(" exceeded the maximum1000ms foreground gate; do not widen it")));
    }
    receipt.data.insert("capture_calls", 1); receipt.data.insert("stage", "complete"); receipt.data.insert("passed", true);
}

int main(int argc, char** argv) {
    const auto map = qEnvironmentVariable("FORGE_NAVIGATION_OWNED_MAP");
    if (map.isEmpty()) { std::fputs("OBJECT_COLOUR_PREVIEW_REAL_RECEIPT {\"schema\":1,\"skipped\":true,\"reason\":\"FORGE_NAVIGATION_OWNED_MAP required\"}\n", stdout); return 77; }
    double limit = 1000; const auto requested = qEnvironmentVariable("FORGE_WTS_PREVIEW_MAX_OPERATION_MS");
    if (!requested.isEmpty()) {
        bool valid = false; limit = requested.toDouble(&valid);
        if (!valid || !std::isfinite(limit) || limit <= 0 || limit > 1000) {
            std::fputs("FORGE_WTS_PREVIEW_MAX_OPERATION_MS may only tighten a finite positive1000ms gate\n", stderr); return 2;
        }
    }
    const auto sha = qEnvironmentVariable("FORGE_WTS_PREVIEW_MAP_SHA256");
    if (!sha.isEmpty() && (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](QChar c) {
        return (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
    }))) { std::fputs("FORGE_WTS_PREVIEW_MAP_SHA256 must be lowercase64-hex\n", stderr); return 2; }
    QTemporaryDir settings(QDir::tempPath() + QStringLiteral("/AudastraWtsPreviewReal-XXXXXX")); if (!settings.isValid()) return 2;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AudastraOwnedTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ObjectColourPreviewRealTests"));
    QApplication application(argc, argv);
    if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
        std::fputs("ObjectColourPreviewRealTests requires QT_QPA_PLATFORM=offscreen\n", stderr); return 2;
    }
    auto* fusion = QStyleFactory::create(QStringLiteral("Fusion")); if (!fusion) return 2;
    QApplication::setStyle(fusion); InitializeStyleResources();
    War3Map::UiKit::ApplyApplicationFont(War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory()));
    War3Map::UiKit::ThemeManager theme; theme.ApplyTheme(QStringLiteral("graphite"));
    if (application.styleSheet().isEmpty()) { std::fputs("Production graphite resources did not apply\n", stderr); return 2; }
    ObjectColourPreviewRealTests tests(map, sha, limit); return QTest::qExec(&tests, argc, argv);
}

#include "ObjectColourPreviewRealTests.moc"
