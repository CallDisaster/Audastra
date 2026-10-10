#include "War3MapCore/Services/MapSizeAnalysis.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
MapSizeContext Context() {
    MapSizeContext context; context.contextRevision = "synthetic-project/open-3/asset-7/script-2";
    context.membersComplete = true; return context;
}
MapSizeReport Analyze(const MapSizeContext& context, const MapSizeAnalysisOptions& options = {}) {
    auto result = AnalyzeMapSize(context, context.contextRevision, options);
    Require(result.Succeeded() && result.value.has_value(), "synthetic metadata analysis succeeds");
    return std::move(*result.value);
}
const MapSizeCategoryTotals& Part(const MapSizeTotals& totals, MapSizeCategory category) {
    return totals.categories[static_cast<std::size_t>(category)];
}
const MapSizeRow& Row(const MapSizeReport& report, std::string_view path) {
    const auto found = std::find_if(report.top20.begin(), report.top20.end(), [&](const auto& row) { return row.currentPath == path; });
    Require(found != report.top20.end(), "expected exact current path appears in bounded top20"); return *found;
}
bool Reason(const MapSizeReport& report, std::string_view code) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code && diagnostic.severity == DiagnosticSeverity::Warning && !diagnostic.message.empty();
    });
}
void CategoriesAndStableTop20() {
    auto context = Context();
    for (std::size_t index = 0; index < 30; ++index) {
        const auto number = std::string(index < 10 ? "0" : "") + std::to_string(index);
        context.members.push_back({"Textures/t" + number + ".blp", 100});
    }
    context.members.push_back({"Units/unit.mdx", 2}); context.members.push_back({"Sounds/hit.wav", 1});
    context.members.push_back({"war3map.j", 5}); context.members.push_back({"utility.lua", 0});
    context.members.push_back({"war3map.imp", 17});
    const auto report = Analyze(context);
    Require(report.state == MapSizeState::Ready && report.baseline.state == MapSizeState::Ready && report.effective.state == MapSizeState::Ready,
        "complete named logical metadata and unchanged overlay are ready");
    Require(report.baseline.knownLogicalSubtotal == 3025 && report.effective.knownLogicalSubtotal == 3025,
        "subtotal is logical member bytes including one actual IMP member");
    Require(Part(report.baseline, MapSizeCategory::Texture).knownRows == 30 && Part(report.baseline, MapSizeCategory::Texture).knownBytes == 3000,
        "textures category counts real member metadata once");
    Require(Part(report.baseline, MapSizeCategory::Model).knownBytes == 2 && Part(report.baseline, MapSizeCategory::Sound).knownBytes == 1,
        "models and sounds use extension classifications");
    Require(Part(report.baseline, MapSizeCategory::Script).knownRows == 2 && Part(report.baseline, MapSizeCategory::Script).knownBytes == 5,
        "actual zero byte script is present known zero rather than missing");
    Require(Part(report.baseline, MapSizeCategory::Other).knownRows == 1 && Part(report.baseline, MapSizeCategory::Other).knownBytes == 17,
        "IMP table is one member, no imported-reference pseudo files");
    Require(report.top20.size() == 20 && report.topWindowComplete && !report.outputTruncated, "fixed top20 window is complete for provided known metadata");
    for (std::size_t index = 0; index < 20; ++index) {
        const auto number = std::string(index < 10 ? "0" : "") + std::to_string(index);
        Require(report.top20[index].currentPath == "Textures/t" + number + ".blp" && report.top20[index].identity == "member/TEXTURES\\T" + number + ".BLP",
            "equal sizes sort by stable identity while preserving original path spelling");
        Require(report.top20[index].navigationAllowed && report.top20[index].currentLogicalBytes == 100, "ordinary unambiguous source allows resource activation metadata");
    }
    std::reverse(context.members.begin(), context.members.end());
    const auto reordered = Analyze(context);
    for (std::size_t index = 0; index < 20; ++index) Require(reordered.top20[index].identity == report.top20[index].identity, "tie order does not depend on input row order");
}
void OverlayReplacementRenameDeleteAndOriginalBinding() {
    auto context = Context();
    context.members = {{"a.mdx", 100}, {"b.mdx", 200}, {"old.wav", 30}, {"war3map.imp", 17}, {"war3map.j", 10}};
    context.assetEdits = {{"a.mdx", "b.mdx"}, {"b.mdx", "Textures/second.blp"}, {"old.wav", ""}};
    context.pending = {{"b.mdx", 300, MapSizeByteTrust::ReportedUnverified}, {"New/unit.mdx", 50, MapSizeByteTrust::OwnedMetadata}};
    const auto report = Analyze(context);
    Require(report.baseline.knownLogicalSubtotal == 357 && report.effective.knownLogicalSubtotal == 277,
        "baseline unchanged while renamed/deleted/owned current contributions are applied");
    Require(report.effective.reportedUnverifiedSubtotal == 300 && report.effective.reportedUnverifiedRows == 1,
        "reported external replacement stays separate from known logical subtotal");
    Require(report.deletedRows == 1 && report.state == MapSizeState::Partial && Reason(report, "MAP_SIZE_PENDING_REPORTED"), "external metadata is explicitly unverified and removal is counted");
    const auto& renamed = Row(report, "Textures/second.blp");
    Require(renamed.originalPath == "b.mdx" && renamed.originalLogicalBytes == 200 && renamed.currentLogicalBytes == 200
        && renamed.identity == "member/B.MDX" && renamed.category == MapSizeCategory::Texture && renamed.origin == MapSizeRowOrigin::Renamed,
        "simultaneous rename binds original b not the target of a earlier rename");
    const auto& replacement = Row(report, "b.mdx");
    Require(replacement.originalPath == "a.mdx" && replacement.originalLogicalBytes == 100 && replacement.currentLogicalBytes == 300
        && replacement.trust == MapSizeByteTrust::ReportedUnverified && replacement.origin == MapSizeRowOrigin::PendingReplacement,
        "pending replacement attaches current target while retaining original source identity");
    const auto& owned = Row(report, "New/unit.mdx");
    Require(owned.originalPath.empty() && !owned.originalLogicalBytes && owned.currentLogicalBytes == 50 && owned.trust == MapSizeByteTrust::OwnedMetadata,
        "owned in-memory import size is known without reading its body");
    Require(!Row(report, "war3map.j").navigationAllowed && Row(report, "war3map.j").managedMember,
        "managed script can be displayed but cannot use resource Reveal");
    Require(context.members[0].path == "a.mdx" && context.members[0].logicalBytes == 100 && context.assetEdits[0].to == "b.mdx",
        "pure analysis does not mutate source metadata or asset edit state");
}
void DuplicateAndMissingSourceRefuseGuessing() {
    auto context = Context(); context.members = {{"a.blp", 10}, {"A.BLP", 20}, {"ok.mdx", 30}};
    auto report = Analyze(context);
    Require(report.baseline.knownLogicalSubtotal == 30 && report.baseline.unknownRows == 2 && report.top20.size() == 1
        && report.ambiguousRows == 2 && Reason(report, "MAP_SIZE_CANONICAL_DUPLICATE"), "duplicate canonical sources are not first-row selection or double counted");
    context = Context(); context.members = {{"a.mdx", 10}, {"b.mdx", 20}};
    context.assetEdits = {{"a.mdx", "b.mdx"}}; report = Analyze(context);
    Require(report.baseline.knownLogicalSubtotal == 30 && report.effective.knownLogicalSubtotal == 0 && report.effective.unknownRows == 2
        && report.top20.empty(), "rename destination collision suppresses both effective values only");
    context = Context(); context.members = {{"a.mdx", 10}};
    context.pending = {{"a.mdx", 20, MapSizeByteTrust::OwnedMetadata}, {"A.MDX", 30, MapSizeByteTrust::OwnedMetadata}};
    report = Analyze(context);
    Require(report.effective.knownLogicalSubtotal == 0 && report.effective.unknownRows == 2 && report.top20.empty(), "duplicate pending replacements cannot pick or sum one source");
    context = Context(); context.members = {{"a.mdx", 10}}; context.assetEdits = {{"a.mdx", "x.mdx"}, {"A.MDX", "y.mdx"}};
    report = Analyze(context);
    Require(report.baseline.knownLogicalSubtotal == 10 && report.effective.unknownRows == 1 && report.top20.empty()
        && Reason(report, "MAP_SIZE_EDIT_SOURCE_AMBIGUOUS"), "duplicate source edit identities refuse local current value");
    context = Context(); context.members = {{"a.mdx", 10}}; context.assetEdits = {{"missing.mdx", "x.mdx"}};
    report = Analyze(context);
    Require(report.effectiveUnknown && report.effective.state == MapSizeState::Unknown && !report.effective.knownLogicalSubtotal
        && report.top20.empty() && report.baseline.knownLogicalSubtotal == 10, "missing edit source keeps baseline proof and refuses current guessed total");
}
void UnicodeAndDirectoryScopeAreMetadataOnly() {
    auto context = Context(); context.sourceKind = MapSizeSourceKind::DirectorySnapshot;
    context.members = {{"资源/界面.blp", 100}, {"resource/a.blp", 200}, {"a.blp", 300}, {"table/unit.ini", 40}};
    const auto report = Analyze(context);
    Require(report.sourceKind == MapSizeSourceKind::DirectorySnapshot && report.baseline.knownLogicalSubtotal == 640
        && report.effective.knownLogicalSubtotal == 640 && report.nonAsciiRows == 1, "valid UTF8 path counts known logical bytes in frozen directory metadata");
    const auto& unicode = Row(report, "资源/界面.blp");
    Require(unicode.nonAsciiIdentity && !unicode.navigationAllowed && unicode.currentLogicalBytes == 100 && unicode.category == MapSizeCategory::Texture,
        "nonASCII inability to safely navigate does not turn known byte count unknown");
    Require(report.state == MapSizeState::Partial && Reason(report, "MAP_SIZE_NON_ASCII_IDENTITY") && !report.topWindowComplete,
        "Unicode comparison coverage limitation is explicit");
    Require(Row(report, "resource/a.blp").identity != Row(report, "a.blp").identity && report.top20.size() == 4,
        "directory resource prefix remains original identity, no archive alias or merge");
    Require(Row(report, "table/unit.ini").managedMember && !Row(report, "table/unit.ini").navigationAllowed,
        "directory LNI object metadata is a managed member not asset Reveal");
    context = Context(); context.members = {{"模型/unit.mdx", 80}}; context.assetEdits = {{"模型/unit.mdx", "unit.mdx"}};
    context.pending = {{"unit.mdx", 90, MapSizeByteTrust::OwnedMetadata}};
    const auto renamed = Analyze(context);
    Require(renamed.effective.knownLogicalSubtotal == 90 && Row(renamed, "unit.mdx").nonAsciiIdentity
        && !Row(renamed, "unit.mdx").navigationAllowed && Row(renamed, "unit.mdx").originalPath == "模型/unit.mdx",
        "ASCII target and owned replacement do not clear original Unicode identity limitation");
}
void UnknownOpaqueGeneratedAndAbsentSources() {
    auto context = Context(); context.members = {{"war3map.j", 100}, {"utility.lua", 50}, {"a.blp", 20}, {"unknown.mdx", {}}};
    context.generatedScriptSizeUnknown = true; context.opaqueMemberCount = 2;
    auto report = Analyze(context);
    Require(report.baseline.knownLogicalSubtotal == 170 && report.effective.knownLogicalSubtotal == 70
        && Part(report.effective, MapSizeCategory::Script).unknownRows == 1 && report.effective.unknownRows == 2,
        "old generated script is retained only in baseline while utility script size stays known");
    Require(report.opaqueMemberCount == 2 && report.state == MapSizeState::Partial && Reason(report, "MAP_SIZE_OPAQUE_MEMBERS")
        && Reason(report, "MAP_SIZE_GENERATED_SCRIPT_UNKNOWN") && Reason(report, "MAP_SIZE_UNKNOWN_BYTES"), "unknown causes are classified without inferred body sizes");
    Require(report.top20.size() == 2 && Row(report, "utility.lua").currentLogicalBytes == 50 && !Row(report, "a.blp").navigationAllowed,
        "unknown size rows do not fake top rank or opaque full-coverage activation");
    context = Context(); report = Analyze(context);
    Require(report.state == MapSizeState::Ready && report.baseline.knownLogicalSubtotal == 0 && report.top20.empty(), "proven empty captured named scope has known zero");
    context.membersComplete = false; context.opaqueMemberCountUnknown = true; report = Analyze(context);
    Require(report.state == MapSizeState::Unknown && !report.topWindowComplete, "unavailable empty scope is unknown rather than proven absent");
    context = Context(); context.generatedScriptSizeUnknown = true; report = Analyze(context);
    Require(Part(report.effective, MapSizeCategory::Script).unknownRows == 1 && report.effective.state == MapSizeState::Unknown,
        "unlisted generated script records one explicit unknown, not invented zero bytes");
    context = Context(); context.sourceKind = MapSizeSourceKind::DirectorySnapshot;
    context.members = {{"map/scripts/war3map.lua", 80}, {"scripts/utility.lua", 6}}; context.generatedScriptSizeUnknown = true;
    report = Analyze(context);
    Require(report.baseline.knownLogicalSubtotal == 86 && report.effective.knownLogicalSubtotal == 6
        && Part(report.effective, MapSizeCategory::Script).unknownRows == 1 && report.top20.size() == 1,
        "directory map prefix marks corresponding generated script unknown without aliasing other script identities");
    context = Context(); context.members = {{"a.mdx", 5}}; context.overlayComplete = false; report = Analyze(context);
    Require(report.baseline.knownLogicalSubtotal == 5 && report.effectiveUnknown && !report.effective.knownLogicalSubtotal && report.top20.empty(),
        "incomplete overlay never labels unchanged baseline as current size");
}
void Checked64BitKnownAndReportedSums() {
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    auto context = Context(); context.members = {{"a.mdx", maximum}, {"b.blp", 1}};
    auto report = Analyze(context);
    Require(report.baseline.sumOverflow && !report.baseline.knownLogicalSubtotal && report.effective.sumOverflow && !report.effective.knownLogicalSubtotal
        && Part(report.baseline, MapSizeCategory::Model).knownBytes == maximum && Part(report.baseline, MapSizeCategory::Texture).knownBytes == 1
        && Reason(report, "MAP_SIZE_SUM_OVERFLOW"), "checked total overflow does not erase individually valid category sums or wrap total");
    context.members = {{"a.mdx", maximum}, {"b.mdx", 1}}; report = Analyze(context);
    Require(!Part(report.baseline, MapSizeCategory::Model).knownBytes && !report.baseline.knownLogicalSubtotal, "checked category overflow returns null");
    context = Context(); context.pending = {{"a.wav", maximum, MapSizeByteTrust::ReportedUnverified}, {"b.wav", 1, MapSizeByteTrust::ReportedUnverified}};
    report = Analyze(context);
    Require(report.effective.sumOverflow && !report.effective.reportedUnverifiedSubtotal && !Part(report.effective, MapSizeCategory::Sound).reportedUnverifiedBytes
        && report.effective.knownLogicalSubtotal == 0, "unverified sum overflow remains separate from known subtotal");
}
void GlobalRowsPathsAndOutputBudgets() {
    auto context = Context();
    for (std::size_t index = 0; index < 5000; ++index) context.members.push_back({"Texture/t" + std::to_string(index) + ".blp", index == 4999 ? 1000000u : 1u});
    auto report = Analyze(context);
    Require(report.examinedMetadataRows == 4096 && report.baseline.knownRows == 4096 && report.inputTruncated && !report.topWindowComplete
        && report.baseline.knownLogicalSubtotal == 4096 && report.top20.front().currentLogicalBytes == 1,
        "default 4096 prefix is partial and cannot claim unseen largest file or full-map subtotal");
    MapSizeAnalysisOptions expanded; expanded.maxMetadataRows = 32768; report = Analyze(context, expanded);
    Require(report.state == MapSizeState::Ready && report.examinedMetadataRows == 5000 && report.top20.front().currentLogicalBytes == 1000000,
        "explicit bounded larger metadata window can cover an existing large catalog without body reads");
    context = Context(); context.members = {{"a.mdx", 10}, {"b.mdx", 20}};
    context.assetEdits = {{"a.mdx", "x.mdx"}}; context.pending = {{"y.mdx", 5, MapSizeByteTrust::OwnedMetadata}};
    MapSizeAnalysisOptions limited; limited.maxMetadataRows = 3; report = Analyze(context, limited);
    Require(report.examinedMetadataRows == 3 && report.inputTruncated && report.effectiveUnknown && !report.effective.knownLogicalSubtotal
        && report.baseline.knownLogicalSubtotal == 30, "global row budget includes edits and pending rather than independent windows");
    context = Context(); context.members = {{"a.mdx", 10}, {"b.mdx", 20}}; limited = {}; limited.maxTotalPathBytes = 5;
    report = Analyze(context, limited);
    Require(report.processedPathBytes == 5 && report.inputTruncated && report.baseline.knownLogicalSubtotal == 10 && !report.topWindowComplete,
        "total path bytes stop before processing additional identity while keeping positive prefix evidence");
    context = Context(); context.members = {{std::string(2100, 'a') + ".blp", 500}, {"ok.mdx", 5}, {std::string("bad") + '\xc0' + '\xaf' + ".blp", 10}};
    report = Analyze(context);
    Require(report.invalidPathRows == 2 && report.baseline.knownLogicalSubtotal == 5 && report.baseline.unknownRows == 2 && report.top20.size() == 1,
        "overlong and invalid UTF8 paths are not truncated or replacement-decoded into new identities");
    context = Context(); context.members = {{std::string(200, 'a') + ".mdx", 200}, {"small.blp", 1}};
    limited = {}; limited.maxResultBytes = 4000; report = Analyze(context, limited);
    Require(report.outputTruncated && report.top20.empty() && !report.topWindowComplete && report.outputBudgetBytes <= limited.maxResultBytes
        && Reason(report, "MAP_SIZE_OUTPUT_TRUNCATED"), "output budget preserves fixed summaries and a billed truncation reason without skipping a higher-ranked row");
    limited.maxResultBytes = 1;
    Require(!AnalyzeMapSize(context, context.contextRevision, limited).Succeeded(), "too-small fixed-report output budget fails without oversized report");
}
void RevisionOwnershipAndInvalidInputs() {
    auto context = Context(); context.members = {{"a.mdx", 50}};
    const auto report = Analyze(context);
    context.members[0].path = "new.mdx"; context.members[0].logicalBytes = 99; context.contextRevision = "next-open";
    Require(report.contextRevision != context.contextRevision && report.top20[0].currentPath == "a.mdx" && report.top20[0].currentLogicalBytes == 50,
        "report owns identity and size and does not retain borrowed spans across host changes");
    Require(!AnalyzeMapSize(context, report.contextRevision).Succeeded(), "stale expected revision cannot query a new project context");
    context.contextRevision = std::string("bad") + '\xc0';
    Require(!AnalyzeMapSize(context, context.contextRevision).Succeeded(), "invalid UTF8 context binding rejects query");
    context = Context(); context.sourceKind = static_cast<MapSizeSourceKind>(255);
    Require(!AnalyzeMapSize(context, context.contextRevision).Succeeded(), "unknown source type is not silently archive metadata");
    context = Context(); context.pending = {{"a.mdx", 40, static_cast<MapSizeByteTrust>(255)}};
    const auto invalidTrust = Analyze(context);
    Require(invalidTrust.effective.knownLogicalSubtotal == 0 && invalidTrust.effective.reportedUnverifiedSubtotal == 0
        && invalidTrust.effective.unknownRows == 1 && invalidTrust.top20.empty() && Reason(invalidTrust, "MAP_SIZE_PENDING_TRUST_INVALID"),
        "unknown trust enum cannot turn hostile metadata into known or unverified numeric tally");
    MapSizeAnalysisOptions invalid; invalid.maxMetadataRows = 32769;
    Require(!AnalyzeMapSize(context, context.contextRevision, invalid).Succeeded(), "row budget hard maximum cannot be disabled");
}
}
int main() {
    try {
        CategoriesAndStableTop20(); OverlayReplacementRenameDeleteAndOriginalBinding(); DuplicateAndMissingSourceRefuseGuessing();
        UnicodeAndDirectoryScopeAreMetadataOnly(); UnknownOpaqueGeneratedAndAbsentSources(); Checked64BitKnownAndReportedSums();
        GlobalRowsPathsAndOutputBudgets(); RevisionOwnershipAndInvalidInputs();
        std::cout << "map size analysis metadata identity and budget tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
