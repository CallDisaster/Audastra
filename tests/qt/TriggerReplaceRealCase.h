#pragma once

// 真实验收私有助手：仅抽取既有选择、原字段比较和测量实现，不提供生产编辑入口。
#include "TriggerReplaceDialog.h"
#include "TriggerModule.h"
#include "EditorProject.h"
#include "Theme/ThemeManager.h"
#include "Fonts/FontLoader.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Text/Utf8Scalar.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif


namespace War3Map::Tests::TriggerReplaceReal {
using namespace War3Map;
inline double Milliseconds(const QElapsedTimer& timer) { return static_cast<double>(timer.nsecsElapsed()) / 1000000.0; }
inline QString Utf8Display(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
inline QString FailureText(const std::vector<Diagnostic>& diagnostics) {
    QString text;
    for (std::size_t i = 0; i < (std::min)(diagnostics.size(), std::size_t{4}); ++i)
        text += Utf8Display(diagnostics[i].code + ": " + diagnostics[i].message) + QLatin1Char('\n');
    return text;
}
inline std::optional<std::uint64_t> RssBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = static_cast<DWORD>(sizeof(counters));
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
        static_cast<DWORD>(sizeof(counters)))) return static_cast<std::uint64_t>(counters.WorkingSetSize);
#endif
    return std::nullopt;
}
inline QJsonValue Rss() {
    const auto value = RssBytes();
    return value ? QJsonValue(QString::number(static_cast<qulonglong>(*value))) : QJsonValue(QJsonValue::Null);
}
struct Receipt {
    QJsonObject data{{"schema", 1}, {"evidence", "real Qt TriggerModule and shared replacement service, CPU only"},
        {"passed", false}, {"main_window_verified", false}, {"gpu_verified", false}, {"native_editor_verified", false},
        {"game_verified", false}, {"map_copy_created", false}, {"source_written", false}};
    std::map<QString, std::vector<double>> timings;
    ~Receipt() {
        QJsonObject stages;
        for (const auto& [name, values] : timings) {
            auto sorted = values; std::sort(sorted.begin(), sorted.end());
            QJsonArray samples; for (const auto value : values) samples.push_back(value);
            stages.insert(name, QJsonObject{{"samples_ms", samples}, {"count", static_cast<int>(values.size())},
                {"maximum_ms", sorted.empty() ? 0.0 : sorted.back()},
                {"median_ms", sorted.empty() ? 0.0 : sorted[sorted.size() / 2]}});
        }
        data.insert("foreground_operations", stages);
        data.insert("rss_final_bytes", Rss());
        const auto json = QJsonDocument(data).toJson(QJsonDocument::Compact);
        std::fputs("TRIGGER_REPLACE_REAL_RECEIPT ", stdout);
        (void)std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
        std::fputc('\n', stdout); std::fflush(stdout);
    }
};
inline void ObserveFailure(Receipt& receipt, const QString& reason) {
    const auto bounded = (receipt.data.value("stage").toString() + QStringLiteral(": ") + reason).left(4096);
    receipt.data.insert("failure_reason", bounded);
    const auto bytes = bounded.toUtf8();
    std::fputs("TRIGGER_REPLACE_REAL_FAILURE ", stdout);
    (void)std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), stdout);
    std::fputc('\n', stdout); std::fflush(stdout);
}
template<class Callback>
auto Measure(Receipt& receipt, const QString& stage, Callback&& callback) {
    QElapsedTimer timer; timer.start();
    if constexpr (std::is_void_v<std::invoke_result_t<Callback>>) {
        std::invoke(std::forward<Callback>(callback));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        receipt.timings[stage].push_back(Milliseconds(timer));
    } else {
        auto result = std::invoke(std::forward<Callback>(callback));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        receipt.timings[stage].push_back(Milliseconds(timer));
        return result;
    }
}
template<class T> T* Control(QWidget& owner, const char* name) { return owner.findChild<T*>(QString::fromLatin1(name)); }

struct VariableChoice {
    StableId targetId = 0;
    std::string targetName, type, needle;
    std::map<StableId, Services::TriggerReplaceHit> rows;
    std::size_t sourceEcas = 0;
};

inline bool ExactNeedle(std::string_view value) {
    if (value.empty() || value.size() > 2048) return false;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto scalar = Text::Detail::ReadUtf8Scalar(value, offset);
        if (!scalar.Valid() || scalar.codepoint == 0) return false;
        offset += scalar.bytes;
    }
    return true;
}

// 仅为这份真实验收输入设完整 IR/目录准备上限，不替代生产 planner 的精确费用/类型门。
// bulk API 自身最多256项；原图已通过 Serialize 和 exact-source 校验，再做有界只读资格准备。
inline bool AdmitCaseInput(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    QJsonObject& statistics, QString& reason) {
    Services::TriggerReplaceLimits limits;
    std::size_t records = 0, edges = 0, bytes = 0, definitions = 0;
    std::uintmax_t catalogBytes = 0;
    const auto add = [](std::size_t& used, std::size_t amount, std::size_t maximum) {
        if (amount > maximum - used) return false;
        used += amount; return true;
    };
    const auto raw = [&](const Trigger::RawText& value) {
        return add(bytes, value.text.size(), limits.maxScanTextBytes)
            && add(bytes, value.originalBytes.size(), limits.maxScanTextBytes);
    };
    const auto origin = [&](const OriginSpan& value) {
        return value.source.native().size() <= limits.maxScanTextBytes / sizeof(std::filesystem::path::value_type)
            && add(bytes, value.source.native().size() * sizeof(std::filesystem::path::value_type), limits.maxScanTextBytes)
            && add(bytes, value.fingerprint.size(), limits.maxScanTextBytes);
    };
    const auto fail = [&] {
        reason = QStringLiteral("Actual IR/catalog exceeds bounded case preparation records/edges/text; no bulk inspection performed.");
        return false;
    };
    if (document.variables.size() > limits.maxTargetRecords || catalog.Manifest().size() > 1024) return fail();
    for (const auto size : {document.categories.size(), document.variables.size(), document.triggers.size(),
        document.ecaNodes.size(), document.argumentNodes.size()}) if (!add(records, size, limits.maxGraphRecords)) return fail();
    for (const auto& source : catalog.Manifest()) {
        if (source.size > 16U * 1024U * 1024U - catalogBytes || source.definitionCount > 32768 - definitions) return fail();
        catalogBytes += source.size; definitions += source.definitionCount;
    }
    for (const auto& value : document.categories) if (!raw(value.name) || !origin(value.origin)) return fail();
    for (const auto& value : document.variables)
        if (!raw(value.name) || !raw(value.typeName) || !raw(value.value) || !origin(value.origin)) return fail();
    for (const auto& value : document.triggers)
        if (!raw(value.name) || !raw(value.description) || !raw(value.customText) || !origin(value.origin)
            || !add(edges, value.ecas.size(), limits.maxGraphEdges)) return fail();
    for (const auto& value : document.ecaNodes)
        if (!raw(value.name) || !origin(value.origin) || !add(bytes, value.uiLayer.size(), limits.maxScanTextBytes)
            || !add(bytes, value.uiSignature.size(), limits.maxScanTextBytes)
            || value.uiSource.native().size() > limits.maxScanTextBytes / sizeof(std::filesystem::path::value_type)
            || !add(bytes, value.uiSource.native().size() * sizeof(std::filesystem::path::value_type), limits.maxScanTextBytes)
            || !add(edges, value.arguments.size(), limits.maxGraphEdges)
            || !add(edges, value.children.size(), limits.maxGraphEdges)) return fail();
    for (const auto& value : document.argumentNodes)
        if (!raw(value.value) || !origin(value.origin) || !add(edges,
            static_cast<std::size_t>(value.callEca.has_value()) + static_cast<std::size_t>(value.indexArgument.has_value()),
            limits.maxGraphEdges)) return fail();
    if (!raw(document.wct.customComment) || !raw(document.wct.customCode)) return fail();
    statistics.insert("preparation_ir_records", QString::number(records));
    statistics.insert("preparation_ir_edges", QString::number(edges));
    statistics.insert("preparation_ir_text_bytes", QString::number(bytes));
    statistics.insert("preparation_catalog_bytes", QString::number(catalogBytes));
    statistics.insert("preparation_catalog_definitions", QString::number(definitions));
    return true;
}

// 普通 ParameterValue Exact Query 先列出可勾选原值；仅据公开结果挑256个候选，
// 再一次 bulk 核 actual expectedType/owner。任何局部拒绝保留且终止，不换针或删掉所选项。
inline std::optional<VariableChoice> ChooseNeedle(const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const Trigger::TriggerTypeSystem& types, std::string revision,
    std::string_view needle, std::string_view requestedTarget, QJsonObject& statistics, QString& reason, bool& qualificationStarted) {
    constexpr std::size_t maxRows = 4096, maxPages = 32, maxOwnedText = 8 * 1024 * 1024;
    const auto reject = [&](QString message) -> std::optional<VariableChoice> {
        reason = message.left(4096); statistics.insert("selection_reason", reason); return std::nullopt;
    };
    Services::TriggerReplaceLimits limits; limits.pageLimit = 128;
    Services::TriggerReplaceContext context;
    context.revision = std::move(revision); context.document = &document; context.catalog = &catalog; context.types = &types;
    Services::TriggerReplaceQuery query;
    query.mode = Services::TriggerReplaceMode::ParameterValue; query.match = Services::TriggerReplaceMatch::Exact; query.needle = needle;
    statistics.insert("query_mode", "ParameterValue"); statistics.insert("query_match", "Exact");
    statistics.insert("query_needle", Utf8Display(needle)); statistics.insert("source_variable_identity_filter", false);
    std::map<std::string, std::vector<Services::TriggerReplaceHit>> groups;
    std::set<StableId> unique; std::size_t examined = 0, ownedText = 0, pages = 0, queryReadOnly = 0;
    std::optional<Services::TriggerReplaceCursor> cursor;
    std::optional<Services::TriggerReplaceQueryProof> proof;
    for (; pages < maxPages;) {
        const auto page = Services::QueryTriggerReplace(context, context.revision, query, limits, cursor ? &*cursor : nullptr);
        if (!page.Succeeded() || !page.value) return reject(QStringLiteral("Readonly exact query rejected: ") + FailureText(page.diagnostics));
        if (proof && *proof != page.value->proof) return reject(QStringLiteral("Readonly exact query changed proof between pages."));
        proof = page.value->proof; ++pages;
        statistics.insert("coverage_state", static_cast<int>(page.value->coverage.state));
        statistics.insert("total_matches", page.value->coverage.totalMatches
            ? QJsonValue(QString::number(*page.value->coverage.totalMatches)) : QJsonValue(QJsonValue::Null));
        for (const auto& hit : page.value->rows) {
            if (++examined > maxRows) return reject(QStringLiteral("Readonly exact query hit budget exceeded."));
            for (const auto* value : {&hit.rowKey, &hit.oldValue, &hit.triggerName, &hit.functionName, &hit.declaredType,
                &hit.displayValue, &hit.reasonCode, &hit.reason}) {
                if (value->size() > maxOwnedText - ownedText) return reject(QStringLiteral("Readonly exact query owned text budget exceeded."));
                ownedText += value->size();
            }
            if (!hit.argumentId || hit.eligibility != Services::TriggerReplaceEligibility::PreviewRequired
                || hit.oldValue != needle) { ++queryReadOnly; continue; }
            if (!unique.insert(*hit.argumentId).second) return reject(QStringLiteral("Query repeats an actual parameter identity."));
            groups[hit.declaredType].push_back(hit); // 此时只是列表组，尚未取得 expectedType 资格。
        }
        cursor = page.value->next; if (!cursor) break;
    }
    statistics.insert("readonly_preparation_query_pages", QString::number(pages));
    statistics.insert("readonly_preparation_rows_examined", QString::number(examined));
    statistics.insert("readonly_preparation_prefix_only", cursor.has_value());
    statistics.insert("query_readonly_or_display_only_rows", QString::number(queryReadOnly));
    QJsonObject counts; std::size_t reportedTypes = 0;
    for (const auto& [type, hits] : groups) {
        if (reportedTypes++ == 32) break;
        counts.insert(Utf8Display(type).left(128), QString::number(hits.size()));
    }
    statistics.insert("declared_type_query_counts_not_qualification", counts);
    auto targets = Services::QueryTriggerReplacementOptions(context, context.revision,
        Services::TriggerReplacementKind::Variable, requestedTarget, limits);
    if (!targets.Succeeded() || !targets.value) return reject(QStringLiteral("Readonly target query rejected: ") + FailureText(targets.diagnostics));
    std::optional<Services::TriggerReplacementOption> target;
    std::vector<Services::TriggerReplaceHit> selected;
    auto options = *targets.value; std::size_t targetPages = 0;
    while (++targetPages <= 256) {
        for (const auto& option : options.rows) {
            if (!requestedTarget.empty() && option.display != requestedTarget) continue;
            const auto group = groups.find(option.typeName); if (group == groups.end()) continue;
            std::vector<Services::TriggerReplaceHit> candidate;
            for (const auto& hit : group->second) {
                if (hit.oldValue == option.display || hit.oldValue == "udg_" + option.display) continue;
                candidate.push_back(hit); if (candidate.size() == 256) break;
            }
            if (candidate.size() == 256) { target = option; selected = std::move(candidate); break; }
        }
        if (target || !options.next) break;
        const auto next = Services::QueryTriggerReplacementOptions(context, context.revision,
            Services::TriggerReplacementKind::Variable, requestedTarget, limits, &*options.next);
        if (!next.Succeeded() || !next.value) return reject(QStringLiteral("Readonly target page rejected: ") + FailureText(next.diagnostics));
        options = *next.value;
    }
    if (!target || !proof) return reject(QStringLiteral("No actual256-row exact parameter-value case and distinct existing same-type scalar target within the owned query window."));
    std::vector<StableId> ids; std::vector<std::string> keys;
    for (const auto& hit : selected) { ids.push_back(*hit.argumentId); keys.push_back(hit.rowKey); }
    qualificationStarted = true; QElapsedTimer timer; timer.start();
    const auto guards = Trigger::CaptureScalarArgumentReplacementGuards(document, catalog, ids);
    statistics.insert("bulk_guard_preparation_ms_outside_ui_gate", Milliseconds(timer));
    statistics.insert("bulk_guard_request_count", QString::number(ids.size()));
    statistics.insert("bulk_guard_actual_api_hard_limit", 256);
    if (!guards.Succeeded() || !guards.value) return reject(QStringLiteral("Actual bulk qualification rejected: ") + FailureText(guards.diagnostics));
    if (guards.value->size() != selected.size()) return reject(QStringLiteral("Actual bulk qualification did not preserve every requested identity."));
    std::size_t failedGuards = 0, mismatchedGuards = 0; QJsonArray failures;
    std::map<std::string, std::size_t> actualTypes;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        const auto& hit = selected[i]; const auto& guard = (*guards.value)[i];
        if (!guard.guard) {
            ++failedGuards;
            if (failures.size() < 4) failures.push_back(QJsonObject{{"argument", QString::number(guard.argumentId)},
                {"reason", FailureText(guard.diagnostics).left(1024)}});
            continue;
        }
        ++actualTypes[guard.guard->expectedType];
        if (guard.argumentId != *hit.argumentId || guard.guard->ownerEcaId != hit.ecaId
            || guard.guard->sourceTriggerId != hit.triggerId || guard.guard->expectedType != target->typeName
            || !types.Accepts(guard.guard->expectedType, target->typeName)) {
            ++mismatchedGuards;
            if (failures.size() < 4) failures.push_back(QJsonObject{{"argument", QString::number(guard.argumentId)},
                {"reason", "actual bulk expected type or owner differs from the selected query row/target"},
                {"actual_expected_type", Utf8Display(guard.guard->expectedType).left(128)},
                {"selected_target_type", Utf8Display(target->typeName).left(128)},
                {"actual_eca", QString::number(guard.guard->ownerEcaId)}, {"query_eca", QString::number(hit.ecaId)}});
        }
    }
    QJsonObject expectedCounts;
    for (const auto& [type, count] : actualTypes) expectedCounts.insert(Utf8Display(type).left(128), QString::number(count));
    statistics.insert("actual_expected_type_counts", expectedCounts);
    statistics.insert("bulk_guard_failed_count", QString::number(failedGuards));
    statistics.insert("bulk_guard_owner_or_type_mismatch_count", QString::number(mismatchedGuards));
    statistics.insert("bulk_guard_failure_summary", failures);
    if (failedGuards || mismatchedGuards) return reject(QStringLiteral("The selected256-row bulk qualification has refused or incompatible items; no rejected selected row was removed and no alternate case attempted."));
    statistics.insert("planner_started", true); timer.restart();
    const auto prepared = Services::PrepareTriggerReplacement(context, context.revision, query, *proof, keys, target->replacement, limits);
    statistics.insert("planner_preparation_ms_outside_ui_gate", Milliseconds(timer));
    if (!prepared.Succeeded() || !prepared.value) return reject(QStringLiteral("Readonly case planner rejected: ") + FailureText(prepared.diagnostics));
    if (!prepared.value->preview || prepared.value->rows.size() != 256) {
        QString failuresText; std::size_t count = 0;
        for (const auto& row : prepared.value->rows) if (!row.eligible && count++ < 4)
            failuresText += Utf8Display(row.reasonCode + ": " + row.reason) + QLatin1Char('\n');
        return reject(QStringLiteral("Actual planner did not qualify all256 selected rows; no selected row removed. ") + failuresText);
    }
    VariableChoice choice; choice.targetId = target->replacement.variableId; choice.targetName = target->display;
    choice.type = target->typeName; choice.needle = needle; std::set<StableId> owners;
    for (const auto& row : prepared.value->rows) {
        if (!row.eligible || !row.hit.argumentId || row.expectedType != choice.type)
            return reject(QStringLiteral("Actual planner changed the qualified identity or expected type."));
        if (!choice.rows.emplace(*row.hit.argumentId, row.hit).second)
            return reject(QStringLiteral("Actual planner repeats a selected parameter identity."));
        owners.insert(row.hit.ecaId);
    }
    choice.sourceEcas = owners.size();
    statistics.insert("planner_qualified_rows", QString::number(choice.rows.size()));
    statistics.insert("distinct_source_eca_owners", QString::number(choice.sourceEcas));
    return choice;
}

// Exact 查询针只来自真实原参数值。频数用于排序，不能替代 Query、bulk 或 planner 的资格。
inline std::optional<VariableChoice> Choose(const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, std::string revision, std::string_view requestedNeedle,
    std::string_view requestedTarget, QJsonObject& statistics, QString& reason) {
    constexpr std::size_t maximumNeedles = 8, maximumValues = 32768, maximumOwnedText = 8 * 1024 * 1024;
    statistics.insert("variable_records", QString::number(document.variables.size()));
    statistics.insert("argument_records", QString::number(document.argumentNodes.size()));
    statistics.insert("query_mode", "ParameterValue"); statistics.insert("query_match", "Exact");
    const auto reject = [&](QString message) -> std::optional<VariableChoice> {
        reason = message.left(4096); statistics.insert("selection_reason", reason); return std::nullopt;
    };
    if ((!requestedNeedle.empty() && !ExactNeedle(requestedNeedle)) || !AdmitCaseInput(document, catalog, statistics, reason))
        return reject(reason.isEmpty() ? QStringLiteral("Explicit exact parameter-value needle is invalid UTF8/NUL/length.") : reason);
    std::map<std::string, std::size_t> frequencies; std::size_t ownedBytes = 0, invalidValues = 0;
    for (const auto& argument : document.argumentNodes) {
        if (argument.insertCall || argument.insertIndex || argument.callEca || argument.indexArgument
            || (argument.type != 0 && argument.type != 1 && argument.type != 3) || !ExactNeedle(argument.value.text)) { ++invalidValues; continue; }
        auto existing = frequencies.find(argument.value.text);
        if (existing == frequencies.end()) {
            if (frequencies.size() == maximumValues || argument.value.text.size() > maximumOwnedText - ownedBytes)
                return reject(QStringLiteral("Exact-value preparation exceeds32768 values/8MiB owned metadata."));
            ownedBytes += argument.value.text.size(); existing = frequencies.emplace(argument.value.text, 0).first;
        }
        ++existing->second;
    }
    struct Candidate { std::string needle; std::size_t rawCount = 0; };
    std::vector<Candidate> candidates;
    if (!requestedNeedle.empty()) {
        const auto found = frequencies.find(std::string(requestedNeedle));
        candidates.push_back({std::string(requestedNeedle), found == frequencies.end() ? 0 : found->second});
    } else for (const auto& [raw, count] : frequencies) if (count >= 256) {
        candidates.push_back({raw, count});
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            return left.rawCount != right.rawCount ? left.rawCount > right.rawCount : left.needle < right.needle;
        });
        if (candidates.size() > maximumNeedles) candidates.pop_back();
    }
    statistics.insert("automatic_needle", requestedNeedle.empty());
    statistics.insert("metadata_unique_scalar_values", QString::number(frequencies.size()));
    statistics.insert("metadata_value_bytes_owned", QString::number(ownedBytes));
    statistics.insert("metadata_complex_empty_or_invalid_values_excluded", QString::number(invalidValues));
    statistics.insert("needle_candidate_limit", QString::number(maximumNeedles));
    statistics.insert("owned_hit_limit_per_needle", 4096); statistics.insert("page_limit_per_needle", 32);
    statistics.insert("bulk_calls_max", 1); statistics.insert("planner_calls_max", 1);
    if (candidates.empty()) return reject(QStringLiteral("Actual scalar parameter values contain no bounded nonempty exact-query needle with256 metadata occurrences."));
    Trigger::TriggerTypeSystem types(catalog); QJsonArray attempts;
    for (const auto& candidate : candidates) {
        QJsonObject attempt{{"needle", Utf8Display(candidate.needle)}, {"metadata_raw_count_not_qualification", QString::number(candidate.rawCount)}};
        bool qualificationStarted = false;
        auto result = ChooseNeedle(document, catalog, types, revision, candidate.needle, requestedTarget, attempt, reason, qualificationStarted);
        attempt.insert("qualification_started", qualificationStarted); attempt.insert("qualified", result.has_value()); attempts.push_back(attempt);
        statistics.insert("needle_attempts", attempts);
        if (result) { statistics.insert("query_needle", Utf8Display(result->needle)); return result; }
        if (qualificationStarted) return reject(QStringLiteral("The first actual bulk/planner case rejected; no alternate needle or selected subset attempted. ") + reason);
    }
    return reject(QStringLiteral("No legitimate256-row case after bounded normal exact parameter-value queries. ") + reason);
}

inline std::optional<StableId> DisplayedArgumentId(const QString& location) {
    const QString marker = QStringLiteral(" · 参数 ");
    const auto start = location.lastIndexOf(marker);
    if (start < 0) return std::nullopt;
    const auto value = location.mid(start + marker.size());
    bool valid = false; const auto id = value.toULongLong(&valid);
    return valid && id && QString::number(id) == value ? std::optional<StableId>(id) : std::nullopt;
}
inline bool EqualArgument(const Trigger::ArgumentNode& left, const Trigger::ArgumentNode& right) {
    return left.id == right.id && left.type == right.type && left.insertCall == right.insertCall && left.callEca == right.callEca
        && left.insertIndex == right.insertIndex && left.indexArgument == right.indexArgument
        && left.value.text == right.value.text && left.value.encoding == right.value.encoding
        && left.value.originalBytes == right.value.originalBytes;
}
inline bool ArgumentsMatch(const std::vector<Trigger::ArgumentNode>& original, const Trigger::TriggerDocument& current,
    const std::set<StableId>& selected, std::string_view replacement, bool applied) {
    if (original.size() != current.argumentNodes.size() || selected.size() != 256) return false;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < original.size(); ++i) {
        const auto& before = original[i]; const auto& after = current.argumentNodes[i];
        if (!applied || !selected.contains(before.id)) {
            if (!EqualArgument(before, after)) return false;
        } else {
            if (before.id != after.id || after.type != 1 || before.insertCall != after.insertCall || before.callEca != after.callEca
                || before.insertIndex != after.insertIndex || before.indexArgument != after.indexArgument
                || after.value.text != replacement || after.value.text == before.value.text) return false;
            ++changed;
        }
    }
    return changed == (applied ? std::size_t{256} : std::size_t{0});
}
template<class T> QString ObservedValue(const T& value) {
    if constexpr (std::is_same_v<T, QString>) return value.left(1024);
    else if constexpr (std::is_same_v<T, std::string>) return Utf8Display(value).left(1024);
    else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) return QString::number(static_cast<qlonglong>(value));
    else if constexpr (std::is_integral_v<T>) return QString::number(static_cast<qulonglong>(value));
    else return QStringLiteral("<typed value>");
}

} // namespace War3Map::Tests::TriggerReplaceReal
