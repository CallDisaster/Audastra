#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Services/MapSizeAnalysis.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

using namespace War3Map;
using namespace War3Map::Services;

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

#ifdef _WIN32
std::optional<std::wstring> Environment(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    Require(required <= 32768, "environment value exceeds the bounded path/gate length");
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    Require(copied != 0 && copied < required, "environment value changed during capture");
    value.resize(copied);
    return value;
}
std::optional<std::filesystem::path> SamplePath() {
    const auto value = Environment(L"FORGE_NAVIGATION_OWNED_MAP");
    return value ? std::optional<std::filesystem::path>{std::filesystem::path(*value)} : std::nullopt;
}
std::optional<double> OperationGate() {
    const auto value = Environment(L"FORGE_MAP_SIZE_MAX_OPERATION_MS");
    if (!value) return {};
    wchar_t* end = nullptr; errno = 0;
    const double result = std::wcstod(value->c_str(), &end);
    Require(end != value->c_str() && end == value->c_str() + value->size() && errno != ERANGE
        && std::isfinite(result) && result > 0.0 && result <= 600000.0,
        "FORGE_MAP_SIZE_MAX_OPERATION_MS must be finite, positive and at most 600000");
    return result;
}
#else
std::optional<std::filesystem::path> SamplePath() {
    const auto* value = std::getenv("FORGE_NAVIGATION_OWNED_MAP");
    return value && *value ? std::optional<std::filesystem::path>{std::filesystem::path(value)} : std::nullopt;
}
std::optional<double> OperationGate() {
    const auto* value = std::getenv("FORGE_MAP_SIZE_MAX_OPERATION_MS");
    if (value == nullptr || *value == '\0') return {};
    char* end = nullptr; errno = 0;
    const double result = std::strtod(value, &end);
    Require(end != value && *end == '\0' && errno != ERANGE && std::isfinite(result)
        && result > 0.0 && result <= 600000.0,
        "FORGE_MAP_SIZE_MAX_OPERATION_MS must be finite, positive and at most 600000");
    return result;
}
#endif

const char* StateName(MapSizeState state) {
    switch (state) {
    case MapSizeState::Ready: return "ready";
    case MapSizeState::Partial: return "partial";
    case MapSizeState::Unknown: return "unknown";
    }
    throw std::runtime_error("invalid map-size state");
}
Json Number(std::optional<std::uint64_t> value) {
    return value ? Json(*value) : Json(nullptr);
}
Json TotalsJson(const MapSizeTotals& totals) {
    Json categories = Json::array();
    for (const auto& item : totals.categories) categories.push_back({
        {"category", std::string(MapSizeCategoryName(item.category))}, {"knownBytes", Number(item.knownBytes)},
        {"reportedUnverifiedBytes", Number(item.reportedUnverifiedBytes)}, {"knownRows", item.knownRows},
        {"reportedUnverifiedRows", item.reportedUnverifiedRows}, {"unknownRows", item.unknownRows}});
    return {{"state", StateName(totals.state)}, {"knownLogicalSubtotal", Number(totals.knownLogicalSubtotal)},
        {"reportedUnverifiedSubtotal", Number(totals.reportedUnverifiedSubtotal)}, {"knownRows", totals.knownRows},
        {"reportedUnverifiedRows", totals.reportedUnverifiedRows}, {"unknownRows", totals.unknownRows},
        {"sumOverflow", totals.sumOverflow}, {"categories", std::move(categories)}};
}
Json ReportJson(const MapSizeReport& report) {
    Json top = Json::array(), diagnostics = Json::array();
    for (const auto& row : report.top20) top.push_back({
        {"identity", row.identity}, {"originalPath", row.originalPath}, {"currentPath", row.currentPath},
        {"category", std::string(MapSizeCategoryName(row.category))}, {"originalLogicalBytes", Number(row.originalLogicalBytes)},
        {"currentLogicalBytes", Number(row.currentLogicalBytes)}, {"trust", static_cast<unsigned>(row.trust)},
        {"origin", static_cast<unsigned>(row.origin)}, {"nonAsciiIdentity", row.nonAsciiIdentity},
        {"managedMember", row.managedMember}, {"navigationAllowed", row.navigationAllowed}});
    for (const auto& item : report.diagnostics) diagnostics.push_back(item.code);
    return {{"contextRevision", report.contextRevision}, {"sourceKind", "archive-members"},
        {"state", StateName(report.state)}, {"baseline", TotalsJson(report.baseline)},
        {"effective", TotalsJson(report.effective)}, {"top20WithinProcessedWindow", std::move(top)},
        {"topWindowComplete", report.topWindowComplete}, {"inputTruncated", report.inputTruncated},
        {"outputTruncated", report.outputTruncated}, {"effectiveUnknown", report.effectiveUnknown},
        {"generatedScriptSizeUnknown", report.generatedScriptSizeUnknown},
        {"examinedMetadataRows", report.examinedMetadataRows}, {"processedPathBytes", report.processedPathBytes},
        {"invalidPathRows", report.invalidPathRows}, {"ambiguousRows", report.ambiguousRows},
        {"nonAsciiRows", report.nonAsciiRows}, {"deletedRows", report.deletedRows},
        {"opaqueMemberCount", report.opaqueMemberCount}, {"opaqueMemberCountUnknown", report.opaqueMemberCountUnknown},
        {"outputBudgetBytes", report.outputBudgetBytes}, {"diagnosticCodes", std::move(diagnostics)}};
}

// 与真实 Qt adapter 的归档入口相同：只借已有成员取 name/size，最多4097行、路径1MiB。
// 第4097行允许正式服务说明其默认4096行窗口截断；不复制正文、不补读 attributes/signature。
MapSizeContext CaptureMetadata(const MapArchiveSession& archive) {
    MapSizeContext context;
    context.contextRevision = "map-size-real/archive:" + archive.SourceSha256() + "/opened-1/metadata-only";
    context.membersComplete = true;
    context.opaqueMemberCount = static_cast<std::uint64_t>(archive.OpaqueMemberCount());
    context.opaqueMemberCountUnknown = archive.HasOpaqueMembers() && archive.OpaqueMemberCount() == 0;
    constexpr std::size_t rowLimit = 4097, pathLimit = 2048, totalPathLimit = 1024 * 1024;
    std::size_t pathBytes = 0;
    const auto accept = [&](std::string_view path) {
        if (context.members.size() == rowLimit || path.size() > pathLimit || path.size() > totalPathLimit - pathBytes)
            return false;
        pathBytes += path.size(); return true;
    };
    for (const auto& member : archive.Members()) {
        if (!accept(member.name)) { context.membersComplete = false; break; }
        context.members.push_back({member.name, static_cast<std::uint64_t>(member.data.size())});
    }
    for (const auto& name : archive.MetadataMembers()) {
        if (!accept(name)) { context.membersComplete = false; break; }
        context.members.push_back({name, std::nullopt});
    }
    return context;
}

void CheckReport(const MapSizeContext& context, const MapSizeAnalysisOptions& options, const MapSizeReport& report) {
    Require(report.contextRevision == context.contextRevision && report.sourceKind == MapSizeSourceKind::ArchiveMembers,
        "analysis must retain the actual owned archive context");
    Require(report.examinedMetadataRows <= options.maxMetadataRows && report.processedPathBytes <= options.maxTotalPathBytes
        && report.outputBudgetBytes <= options.maxResultBytes && report.top20.size() <= 20, "real analysis respects all metadata/output limits");
    Require(report.baseline.knownRows != 0 && !report.top20.empty(),
        "the owned-map measurement must actually analyze at least one known-size member");
    Require(!report.effectiveUnknown && !report.generatedScriptSizeUnknown && report.deletedRows == 0,
        "fresh owned archive has no speculative edit overlay");
    Require(report.opaqueMemberCount == context.opaqueMemberCount && report.opaqueMemberCountUnknown == context.opaqueMemberCountUnknown,
        "opaque members remain explicit in the report");
    auto baseline = TotalsJson(report.baseline), effective = TotalsJson(report.effective);
    baseline.erase("state"); effective.erase("state");
    Require(baseline == effective && report.baseline.reportedUnverifiedRows == 0,
        "without edits all actual subtotal/category/row values agree and no reported external sizes are invented");
    if (!context.membersComplete || context.opaqueMemberCount != 0 || context.opaqueMemberCountUnknown
        || report.inputTruncated || report.outputTruncated || report.invalidPathRows != 0 || report.ambiguousRows != 0
        || report.nonAsciiRows != 0 || report.effective.unknownRows != 0)
        Require(report.state != MapSizeState::Ready && !report.topWindowComplete, "an incomplete real window is not claimed to cover the entire map");
    for (std::size_t index = 0; index < report.top20.size(); ++index) {
        const auto& row = report.top20[index];
        Require(row.origin == MapSizeRowOrigin::Original && row.trust == MapSizeByteTrust::OwnedMetadata
            && row.currentPath == row.originalPath && row.originalLogicalBytes == row.currentLogicalBytes
            && row.currentLogicalBytes.has_value(), "top rows retain actual original owned sizes");
        const auto found = std::find_if(context.members.begin(), context.members.end(), [&](const auto& member) {
            return member.path == row.originalPath && member.logicalBytes == row.originalLogicalBytes;
        });
        Require(found != context.members.end(), "top row is bound to actual captured member metadata");
        // 大小未知的 attributes 等可令统计 Partial，但不使已闭合来源中的另一条可信身份失效。
        if (row.nonAsciiIdentity || row.managedMember || !context.membersComplete || report.inputTruncated
            || context.opaqueMemberCount != 0 || context.opaqueMemberCountUnknown)
            Require(!row.navigationAllowed, "an unclosed source or managed/non-ASCII row does not authorize resource navigation");
        if (index != 0) {
            const auto& previous = report.top20[index - 1];
            Require(*previous.currentLogicalBytes > *row.currentLogicalBytes
                || (*previous.currentLogicalBytes == *row.currentLogicalBytes && previous.identity < row.identity),
                "actual top window is ordered by size and stable identity");
        }
    }
}
void Emit(const Json& receipt) {
    std::cout << "MAP_SIZE_REAL_RECEIPT " << receipt.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
}
} // namespace

int main() {
    Json receipt{{"unit", "map-size-real"}, {"evidence", "actual readonly archive metadata and CPU service only"},
        {"mapEnvironment", "FORGE_NAVIGATION_OWNED_MAP"}, {"operationGateEnvironment", "FORGE_MAP_SIZE_MAX_OPERATION_MS"},
        {"hostCallbackVerified", false}, {"qtUiVerified", false}, {"gpuVerified", false}, {"nativeEditorVerified", false},
        {"mapCopied", false}, {"bodyCopiedForAnalysis", false}, {"queryDiskIO", false}, {"queryBodyHash", false},
        {"statisticsScope", "captured named logical member bytes; not the compressed physical archive size"}};
    try {
        const auto path = SamplePath();
        if (!path) { receipt["status"] = "skipped"; receipt["reason"] = "owned map environment is not set"; Emit(receipt); return 77; }
        const auto gate = OperationGate(); receipt["operationGateMs"] = gate ? Json(*gate) : Json(nullptr);
        receipt["phase"] = "open-owned-archive";
        const auto physicalBytes = std::filesystem::file_size(*path);
        const auto openStart = Clock::now();
        auto opened = MapArchiveSession::Open(*path);
        receipt["archiveOpenMs"] = Milliseconds(openStart);
        if (!opened.Succeeded() || !opened.value) {
            Json codes = Json::array(); for (const auto& diagnostic : opened.diagnostics) codes.push_back(diagnostic.code);
            receipt["openDiagnosticCodes"] = std::move(codes);
            throw std::runtime_error("actual owned archive could not be opened");
        }
        auto archive = std::move(*opened.value); // 单一归档拥有解码快照；不复制项目或 Body。
        const auto sampleSha = archive.SourceSha256();
        Require(sampleSha.size() == 64, "opened archive must expose its actual SHA256");
        receipt["sampleSha256"] = sampleSha; receipt["samplePhysicalBytes"] = physicalBytes;
        receipt["sourceNamedMemberCount"] = archive.Members().size();
        receipt["sourceMetadataMemberCount"] = archive.MetadataMembers().size();
        receipt["phase"] = "prepare-owned-metadata";
        const auto prepareStart = Clock::now();
        const auto context = CaptureMetadata(archive);
        receipt["metadataPrepareMs"] = Milliseconds(prepareStart);
        receipt["capturedMetadataRows"] = context.members.size(); receipt["captureMembersComplete"] = context.membersComplete;
        Require(!context.members.empty(), "real archive must provide named metadata to analyze");
        MapSizeAnalysisOptions options;
        receipt["limits"] = {{"maxMetadataRows", options.maxMetadataRows}, {"maxPathBytes", options.maxPathBytes},
            {"maxTotalPathBytes", options.maxTotalPathBytes}, {"maxResultBytes", options.maxResultBytes}};
        std::array<double, 3> elapsed{}; std::optional<Json> stableReport;
        receipt["phase"] = "analyze-actual-service-three-times";
        for (std::size_t round = 0; round < elapsed.size(); ++round) {
            const auto start = Clock::now();
            auto result = AnalyzeMapSize(context, context.contextRevision, options);
            elapsed[round] = Milliseconds(start); // 仅服务调用；结果检查/JSON与源重核不混入热路径。
            Require(result.Succeeded() && result.value.has_value(), "actual metadata analysis must return a typed report");
            CheckReport(context, options, *result.value);
            auto rendered = ReportJson(*result.value);
            if (stableReport) Require(*stableReport == rendered, "all three actual reports must be deterministic");
            else stableReport = std::move(rendered);
        }
        auto ordered = elapsed; std::sort(ordered.begin(), ordered.end());
        receipt["analysisSamplesMs"] = elapsed; receipt["analysisMedianMs"] = ordered[1]; receipt["analysisMaximumMs"] = ordered.back();
        receipt["report"] = std::move(*stableReport);
        receipt["phase"] = "verify-original-source-after-measurement";
        const auto verifyStart = Clock::now();
        const auto unchanged = archive.VerifySourceUnchanged();
        receipt["sourceReverificationMs"] = Milliseconds(verifyStart);
        Require(unchanged.Succeeded() && archive.SourceSha256() == sampleSha && std::filesystem::file_size(*path) == physicalBytes,
            "owned source identity and full SHA must remain unchanged");
        receipt["sourceUnchanged"] = true;
        const bool accepted = !gate || ordered.back() <= *gate;
        receipt["phase"] = "complete"; receipt["status"] = accepted ? "passed" : "failed";
        receipt["performanceGateApplied"] = gate.has_value();
        receipt["performanceGatePassed"] = gate ? Json(accepted) : Json(nullptr);
        receipt["performanceClaim"] = gate ? "actual CPU service maximum satisfies the explicit gate if passed"
            : "timing data only; no response-time guarantee was requested";
        Emit(receipt);
        return accepted ? 0 : 1;
    } catch (const std::exception& error) {
        receipt["status"] = "failed"; receipt["reason"] = error.what(); Emit(receipt); return 1;
    }
}
