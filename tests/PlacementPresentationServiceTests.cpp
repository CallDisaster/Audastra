#include "War3MapCore/Services/PlacementPresentationService.h"
#include "War3MapCore/Hash.h"
#include <iostream>
#include <stdexcept>
using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::vector<std::byte> Tga(unsigned width, unsigned height) {
    std::vector<std::byte> bytes(18 + width * height * 3, std::byte{0});
    bytes[2] = std::byte{2}; bytes[12] = static_cast<std::byte>(width); bytes[14] = static_cast<std::byte>(height);
    bytes[16] = std::byte{24}; return bytes;
}
void RequireSamePresentation(const Result<PlacementPresentationRow>& left, const Result<PlacementPresentationRow>& right) {
    const auto diagnostics = [](const std::vector<Diagnostic>& a, const std::vector<Diagnostic>& b) {
        Require(a.size() == b.size(), "batch changed diagnostic count");
        for (std::size_t i = 0; i < a.size(); ++i)
            Require(a[i].severity == b[i].severity && a[i].subsystem == b[i].subsystem && a[i].code == b[i].code
                && a[i].message == b[i].message && !a[i].origin && !b[i].origin, "batch changed fixture diagnostic");
    };
    Require(left.Succeeded() == right.Succeeded() && left.value.has_value() == right.value.has_value(), "batch changed success/value state");
    diagnostics(left.diagnostics, right.diagnostics);
    if (!left.value) return;
    const auto& a = *left.value; const auto& b = *right.value;
    Require(a.object.type == b.object.type && a.object.rawcode == b.object.rawcode && a.owner == b.owner && a.version == b.version
        && a.resourceIdentity == b.resourceIdentity && a.inputIdentity == b.inputIdentity && a.kind == b.kind
        && a.classificationReason == b.classificationReason && a.footprintReason == b.footprintReason
        && a.footprintState == b.footprintState && a.textureSha256 == b.textureSha256, "batch changed current row facts or identity");
    const auto fact = [](const auto& x, const auto& y) {
        Require(x.state == y.state && x.value == y.value && x.reason == y.reason, "batch borrowed another input fact");
    };
    fact(a.fields.category, b.fields.category); fact(a.fields.modelPath, b.fields.modelPath);
    fact(a.fields.pathingTexture, b.fields.pathingTexture); fact(a.fields.useClickHelper, b.fields.useClickHelper);
    Require(a.fields.origins.size() == b.fields.origins.size(), "batch changed field origin count");
    for (std::size_t i = 0; i < a.fields.origins.size(); ++i) {
        const auto& x = a.fields.origins[i]; const auto& y = b.fields.origins[i];
        Require(x.field == y.field && x.objectRawcode == y.objectRawcode && x.sourceId == y.sourceId
            && x.sourcePath == y.sourcePath && x.byteOffset == y.byteOffset, "batch changed ordered field provenance");
    }
    Require(a.textureSource.has_value() == b.textureSource.has_value() && a.footprint.has_value() == b.footprint.has_value(), "batch changed optional resource facts");
    if (a.textureSource) {
        const auto& x = *a.textureSource; const auto& y = *b.textureSource;
        Require(x.canonicalPath == y.canonicalPath && x.layer == y.layer && x.priority == y.priority
            && x.sourceId == y.sourceId && x.sourcePath == y.sourcePath, "batch changed resource source");
    }
    if (a.footprint) {
        const auto& x = *a.footprint; const auto& y = *b.footprint;
        Require(x.pixelWidth == y.pixelWidth && x.pixelHeight == y.pixelHeight && x.worldWidth == y.worldWidth
            && x.worldHeight == y.worldHeight && x.nativeAnchorVerified == y.nativeAnchorVerified
            && x.nativeRotationVerified == y.nativeRotationVerified && x.nativeObjectScaleVerified == y.nativeObjectScaleVerified,
            "batch changed footprint facts");
    }
    diagnostics(a.resourceDiagnostics, b.resourceDiagnostics);
}
void BatchSerializationPreservesEveryCurrentInput(PlacementPresentationInput input, PlacementPresentationContext context) {
    context.resourceReader = [](std::string_view path, std::size_t) {
        return Result<Resources::ResolvedResource>::Success({{std::string(path), Resources::ResourceLayer::Game,
            300, "unchanged-source", "unchanged.tga"}, Tga(2, 2)});
    };
    PlacementPresentationBatch batch(context);
    const auto compare = [&](const PlacementPresentationInput& sample) {
        auto result = batch.Query(context.version, context.resourceIdentity, sample);
        RequireSamePresentation(result, QueryPlacementPresentation(context, context.version, context.resourceIdentity, sample));
        return result;
    };
    const auto first = compare(input);
    // 独立固定样例锁住v1序列化字节格式；普通查询与批次不能一起悄悄改格式。
    Require(first.Succeeded() && first.value->inputIdentity == "1c318cd8b0169301db2c62a98a8d4252da4a13550230ca6e53c9eb269c5ea3eb",
        "placement input v1 canonical format changed");
    const std::vector<std::function<void(PlacementPresentationInput&)>> changes{
        [](auto& x) { x.owner.reset(); },
        [](auto& x) { ++x.owner->index; },
        [](auto& x) { x.owner->creationId = -8; },
        [](auto& x) { x.owner->rawcode = "Z002"; },
        [](auto& x) { x.owner->file = MapData::PlacementFile::Units; x.object.type = ObjectData::ObjectType::Unit; },
        [](auto& x) { x.object.rawcode = "Z002"; },
        [](auto& x) { x.object.type = ObjectData::ObjectType::Doodad; },
        [](auto& x) { x.fields.category.state = PlacementFieldState::Absent; },
        [](auto& x) { x.fields.category.value = "B"; },
        [](auto& x) { x.fields.category.reason = "new category reason"; },
        [](auto& x) { x.fields.modelPath.state = PlacementFieldState::Opaque; },
        [](auto& x) { x.fields.modelPath.value = "other\\model.mdx"; },
        [](auto& x) { x.fields.modelPath.reason = std::string("new\0reason", 10); },
        [](auto& x) { x.fields.pathingTexture.state = PlacementFieldState::Unavailable; },
        [](auto& x) { x.fields.pathingTexture.value = "PATHTEXTURES\\2X2DEFAULT.TGA"; },
        [](auto& x) { x.fields.pathingTexture.value = "PathTextures/2x2Default.tga"; },
        [](auto& x) { x.fields.pathingTexture.value = "_"; },
        [](auto& x) { x.fields.pathingTexture.value = "../invalid.tga"; },
        [](auto& x) { x.fields.pathingTexture.value += std::string("\0tail", 5); },
        [](auto& x) { x.fields.pathingTexture.reason = "new path reason"; },
        [](auto& x) { x.fields.useClickHelper.state = PlacementFieldState::Opaque; },
        [](auto& x) { x.fields.useClickHelper.value = false; },
        [](auto& x) { x.fields.useClickHelper.state = PlacementFieldState::Unavailable; x.fields.useClickHelper.value.reset(); },
        [](auto& x) { x.fields.useClickHelper.reason = "new helper reason"; },
        [](auto& x) { std::swap(x.fields.origins[0].field, x.fields.origins[1].field); },
        [](auto& x) { x.fields.origins[0].objectRawcode = "XTpb"; },
        [](auto& x) { x.fields.origins[0].sourceId = "new-source"; },
        [](auto& x) { x.fields.origins[0].sourcePath = "new-source.slk"; },
        [](auto& x) { ++*x.fields.origins[0].byteOffset; },
        [](auto& x) { x.fields.origins[0].byteOffset.reset(); },
        [](auto& x) { x.fields.origins.push_back(x.fields.origins[0]); },
        [](auto& x) { x.fields.origins[0].sourceId.clear(); },
        [](auto& x) { x.fields.useClickHelper.value.reset(); },
        [](auto& x) { x.fields.modelPath.reason.assign(4097, 'x'); },
    };
    for (const auto& change : changes) {
        auto changed = input; change(changed);
        auto actual = compare(changed);
        if (actual.Succeeded()) Require(actual.value->inputIdentity != first.value->inputIdentity, "changed input reused cached identity");
        RequireSamePresentation(actual, compare(changed));
        RequireSamePresentation(first, compare(input));
    }
    for (unsigned part = 0; part != 7; ++part) {
        auto changedContext = context;
        switch (part) {
        case 0: changedContext.version.document.documentId += "-next"; break;
        case 1: ++changedContext.version.document.generation; break;
        case 2: ++changedContext.version.document.revision; break;
        case 3: changedContext.version.document.sourceSha256 += "-next"; break;
        case 4: ++changedContext.version.catalogRevision; break;
        case 5: ++changedContext.version.editorRevision; break;
        default: changedContext.resourceIdentity += "-next"; break;
        }
        auto changed = input; changed.version = changedContext.version;
        Require(!batch.Query(changedContext.version, changedContext.resourceIdentity, changed).Succeeded(), "cached prefix bypassed version/source validation");
        PlacementPresentationBatch fresh(changedContext);
        auto actual = fresh.Query(changedContext.version, changedContext.resourceIdentity, changed);
        RequireSamePresentation(actual, QueryPlacementPresentation(changedContext, changedContext.version, changedContext.resourceIdentity, changed));
        Require(actual.Succeeded() && actual.value->inputIdentity != first.value->inputIdentity, "new context borrowed another batch prefix");
    }
}
void BatchSerializationBudgetIsNotAnObjectLimit(PlacementPresentationInput input, PlacementPresentationContext context) {
    context.resourceReader = [](std::string_view path, std::size_t) {
        return Result<Resources::ResolvedResource>::Success({{std::string(path), Resources::ResourceLayer::Game,
            300, "unchanged-source", "unchanged.tga"}, Tga(2, 2)});
    };
    for (auto* fact : {&input.fields.category, &input.fields.modelPath, &input.fields.pathingTexture}) fact->reason.assign(4096, 'r');
    input.fields.useClickHelper.reason.assign(4096, 'b');
    PlacementPresentationBatch batch(context);
    // 512份不同对象的原字段+规范片段超过8MiB，后半段必须仍按普通查询继续。
    for (unsigned i = 0; i != 512; ++i) {
        auto code = std::to_string(i); code.insert(0, 4 - code.size(), '0');
        input.object.rawcode = code; input.owner->rawcode = code; input.owner->index = i;
        const auto actual = batch.Query(context.version, context.resourceIdentity, input);
        Require(actual.Succeeded(), "input serialization cache imposed a map object limit");
        if (i == 0 || i == 255 || i == 511)
            RequireSamePresentation(actual, QueryPlacementPresentation(context, context.version, context.resourceIdentity, input));
    }
    input.fields.pathingTexture.value = "new/path/after-budget.tga";
    RequireSamePresentation(batch.Query(context.version, context.resourceIdentity, input),
        QueryPlacementPresentation(context, context.version, context.resourceIdentity, input));
    input.fields.pathingTexture.value = "new/../invalid.tga";
    RequireSamePresentation(batch.Query(context.version, context.resourceIdentity, input),
        QueryPlacementPresentation(context, context.version, context.resourceIdentity, input));
    input.fields.modelPath.reason.push_back('x');
    const auto rejected = batch.Query(context.version, context.resourceIdentity, input);
    Require(!rejected.Succeeded(), "full cache disabled per-row validation");
    RequireSamePresentation(rejected, QueryPlacementPresentation(context, context.version, context.resourceIdentity, input));
}
void BatchKeepsCurrentRowsAndFrozenResourceFacts(PlacementPresentationInput input, PlacementPresentationContext context) {
    unsigned reads = 0;
    auto bytes = Tga(2, 3);
    context.resourceReader = [&](std::string_view path, std::size_t) {
        ++reads;
        return Result<Resources::ResolvedResource>::Success({{std::string(path), Resources::ResourceLayer::MapImport,
            500, "batch-map-import", "real-source.tga"}, bytes});
    };
    PlacementPresentationBatch batch(context);
    PlacementPresentationQueryTimings timings;
    auto first = batch.Query(context.version, context.resourceIdentity, input, &timings);
    Require(first.Succeeded() && reads == 1 && first.value->footprint->worldHeight == 96, "batch failed initial actual capture");
    Require(timings.totalNs > 0 && timings.identityCanonicalNs > 0 && timings.identityHashNs > 0
        && timings.totalNs >= timings.validateNs + timings.rowCopyNs + timings.identityCanonicalNs
            + timings.identityHashNs + timings.resourceNs + timings.footprintNs,
        "optional query timing overlapped phases or did not distinguish canonical input from hashing");
    ++input.owner->index; ++input.owner->creationId;
    ++*input.fields.origins.front().byteOffset;
    input.fields.modelPath.value = "CurrentModel.mdx";
    auto second = batch.Query(context.version, context.resourceIdentity, input);
    const auto oracle = QueryPlacementPresentation(context, context.version, context.resourceIdentity, input);
    Require(second.Succeeded() && oracle.Succeeded() && reads == 2 && second.value->owner == input.owner
        && second.value->version == input.version && second.value->fields.modelPath.value == "CurrentModel.mdx"
        && second.value->fields.origins.front().byteOffset == input.fields.origins.front().byteOffset
        && second.value->inputIdentity == oracle.value->inputIdentity && second.value->inputIdentity != first.value->inputIdentity
        && second.value->textureSha256 == first.value->textureSha256, "resource reuse relabelled an old owner/field row");
    auto invalid = input; invalid.owner->file = MapData::PlacementFile::Units;
    auto stale = context.version; ++stale.document.revision;
    const auto rejected = batch.Query(context.version, context.resourceIdentity, invalid, &timings);
    Require(!rejected.Succeeded() && timings.totalNs > 0 && timings.rowCopyNs == 0
        && timings.identityCanonicalNs == 0 && timings.identityHashNs == 0 && timings.resourceNs == 0 && timings.footprintNs == 0,
        "early rejected query retained phase timings from an older completed row");
    Require(!rejected.Succeeded()
        && !batch.Query(stale, context.resourceIdentity, input).Succeeded()
        && !batch.Query(context.version, "changed-source", input).Succeeded() && reads == 2,
        "cached texture bypassed current owner/version/source gates");
    // 批次持有已读取字节；外部当前性由宿主guard验证，另一次捕获不能借用该批次。
    bytes = Tga(4, 5);
    const auto owned = batch.Query(context.version, context.resourceIdentity, input);
    Require(owned.Succeeded() && owned.value->textureSha256 == first.value->textureSha256 && reads == 2,
        "batch no longer owns its captured texture");
    ++context.version.document.revision; input.version = context.version; context.resourceIdentity += "-new";
    PlacementPresentationBatch next(context);
    const auto changed = next.Query(context.version, context.resourceIdentity, input);
    Require(changed.Succeeded() && reads == 3 && changed.value->version == input.version
        && changed.value->textureSha256 != first.value->textureSha256 && changed.value->footprint->worldHeight == 160,
        "new batch borrowed old source facts or old revision identity");
    context.resourceReader = [&](std::string_view, std::size_t) {
        ++reads; return Result<Resources::ResolvedResource>::Failure("Fixture", "EXACT_HIGH_SOURCE_DENIED", "high source denied; no fallback");
    };
    PlacementPresentationBatch denied(context);
    const auto failure = denied.Query(context.version, context.resourceIdentity, input);
    ++input.owner->index;
    const auto repeated = denied.Query(context.version, context.resourceIdentity, input);
    Require(failure.Succeeded() && repeated.Succeeded() && reads == 4
        && repeated.value->footprintState == PlacementFootprintState::Unavailable && !repeated.value->textureSource
        && repeated.value->resourceDiagnostics.front().code == "EXACT_HIGH_SOURCE_DENIED"
        && failure.value->inputIdentity != repeated.value->inputIdentity, "cached denial lost source failure or current owner identity");
}
void BatchRechecksPerRowOutputBudgets(PlacementPresentationInput input, PlacementPresentationContext context) {
    PlacementPresentationLimits limits; limits.maxLogicalBytes = 10000;
    unsigned reads = 0;
    context.resourceReader = [&](std::string_view path, std::size_t) {
        ++reads;
        return Result<Resources::ResolvedResource>::Success({{std::string(path), Resources::ResourceLayer::Game,
            300, "actual-source", std::string(4000, 's')}, Tga(2, 2)});
    };
    PlacementPresentationBatch batch(context, limits);
    Require(batch.Query(context.version, context.resourceIdentity, input).Succeeded(), "small row did not fit location budget fixture");
    input.fields.modelPath.reason.assign(4096, 'r');
    const auto cached = batch.Query(context.version, context.resourceIdentity, input);
    const auto ordinary = QueryPlacementPresentation(context, context.version, context.resourceIdentity, input, limits);
    Require(!cached.Succeeded() && !ordinary.Succeeded() && reads == 2
        && cached.diagnostics.front().code == "PLACEMENT_PRESENTATION_OUTPUT_BUDGET"
        && cached.diagnostics.front().code == ordinary.diagnostics.front().code, "cached footprint bypassed current row location/output budget");
    input.fields.modelPath.reason.clear();
    context.resourceReader = [&](std::string_view path, std::size_t) {
        ++reads;
        auto result = Result<Resources::ResolvedResource>::Success({{std::string(path), Resources::ResourceLayer::Game,
            300, "actual-source", "actual.tga"}, Tga(2, 2)});
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "Fixture", "SOURCE_WARNING", std::string(4096, 'w')});
        return result;
    };
    PlacementPresentationBatch diagnosticBatch(context, limits);
    Require(diagnosticBatch.Query(context.version, context.resourceIdentity, input).Succeeded(), "small row did not fit diagnostic budget fixture");
    input.fields.modelPath.reason.assign(4096, 'r');
    const auto diagnosticCached = diagnosticBatch.Query(context.version, context.resourceIdentity, input);
    const auto diagnosticOrdinary = QueryPlacementPresentation(context, context.version, context.resourceIdentity, input, limits);
    Require(!diagnosticCached.Succeeded() && !diagnosticOrdinary.Succeeded() && reads == 4
        && diagnosticCached.diagnostics.front().code == "PLACEMENT_PRESENTATION_DIAGNOSTIC_BUDGET"
        && diagnosticCached.diagnostics.front().code == diagnosticOrdinary.diagnostics.front().code,
        "cached reader warning bypassed current row diagnostic budget");
}
void BatchBudgetStopsCachingWithoutRejectingLaterObjects(PlacementPresentationInput input, PlacementPresentationContext context) {
    auto bytes = Tga(2, 2); bytes.resize(1024 * 1024);
    unsigned reads = 0;
    context.resourceReader = [&](std::string_view path, std::size_t) {
        ++reads;
        return Result<Resources::ResolvedResource>::Success({{std::string(path), Resources::ResourceLayer::Game,
            300, "actual-source", std::string(path)}, bytes});
    };
    PlacementPresentationBatch batch(context);
    for (unsigned i = 0; i < 66; ++i) {
        input.fields.pathingTexture.value = "PathTextures\\Budget" + std::to_string(i) + ".tga";
        input.owner->index = i;
        auto row = batch.Query(context.version, context.resourceIdentity, input);
        Require(row.Succeeded() && row.value->footprintState == PlacementFootprintState::Available,
            "resource cache budget imposed an object count limit");
    }
    Require(reads == 66, "batch resource fixture did not read each distinct path");
    Require(batch.Query(context.version, context.resourceIdentity, input).Succeeded() && reads == 67,
        "batch retained owned payload beyond its cumulative byte budget");
    input.fields.pathingTexture.value = "PathTextures\\Budget0.tga";
    Require(batch.Query(context.version, context.resourceIdentity, input).Succeeded() && reads == 67,
        "uncached tail destroyed an earlier valid frozen fact");
}
}
int main() try {
    const WorldSelectionVersion version{{"fixture-map", 1, 7, "fixture-map-sha"}, 3, 4};
    PlacementPresentationInput input{{ObjectData::ObjectType::Destructable, "Z001"},
        MapData::PlacementId{MapData::PlacementFile::Doodads, 15, "Z001", 8}, version, {}};
    input.fields.category = {PlacementFieldState::Known, "P", ""};
    input.fields.useClickHelper = {PlacementFieldState::Known, true, ""};
    input.fields.modelPath = {PlacementFieldState::Known, "Doodads\\Terrain\\LOSBlocker\\LOSBlocker", ""};
    input.fields.pathingTexture = {PlacementFieldState::Known, "PathTextures\\2x2Default.tga", ""};
    for (const auto field : {"bcat", "buch", "bfil", "bptx"}) input.fields.origins.push_back({field, "YTpb", "game-base", "Units\\DestructableData.slk", 90});
    auto texture = Tga(2, 2); int reads = 0; bool deny = false, wrongPath = false;
    PlacementPresentationContext context{version, "frozen-resource-stack-7", [&](std::string_view path, std::size_t maxBytes) {
        ++reads; Require(maxBytes == 1024 * 1024, "reader did not receive bounded budget");
        if (deny) return Result<Resources::ResolvedResource>::Failure("Fixture", "READ_DENIED", "high priority source unreadable");
        return Result<Resources::ResolvedResource>::Success({{wrongPath ? "other.tga" : std::string(path),
            Resources::ResourceLayer::Game, 300, "game-base", "PathTextures\\2x2Default.tga"}, texture});
    }};
    const auto query = [&] { return QueryPlacementPresentation(context, version, context.resourceIdentity, input); };
    auto first = query(); Require(first.Succeeded() && first.value->kind == PlacementPresentationKind::PathingBlockerHelper
        && first.value->footprintState == PlacementFootprintState::Available && first.value->footprint->worldWidth == 64
        && first.value->footprint->worldHeight == 64 && first.value->textureSha256 == Sha256(texture).value
        && first.value->owner == input.owner && !first.value->footprint->nativeAnchorVerified, "inherited blocker size/source identity failed");
    BatchKeepsCurrentRowsAndFrozenResourceFacts(input, context);
    BatchRechecksPerRowOutputBudgets(input, context);
    BatchBudgetStopsCachingWithoutRejectingLaterObjects(input, context);
    BatchSerializationPreservesEveryCurrentInput(input, context);
    BatchSerializationBudgetIsNotAnObjectLimit(input, context);
    // 快速分类保留完整输入门禁；即便最终为普通模型，也不能跳过owner、来源和预算检查。
    const auto sameClassification = [&](const PlacementPresentationInput& sample,
        const WorldSelectionVersion& expected, std::string_view source, const PlacementPresentationLimits& limits = {}) {
        const auto before = reads;
        const auto classified = ClassifyPlacementPresentation(context, expected, source, sample, limits);
        Require(reads == before, "classification unexpectedly read a texture");
        const auto complete = QueryPlacementPresentation(context, expected, source, sample, limits);
        Require(classified.Succeeded() == complete.Succeeded(), "classification bypassed or added an input gate");
        if (classified.Succeeded()) Require(*classified.value == complete.value->kind, "classification differs from retained complete row");
        else {
            Require(classified.diagnostics.size() == complete.diagnostics.size(), "classification discarded an input diagnostic");
            for (std::size_t i = 0; i < classified.diagnostics.size(); ++i) {
                const auto& a = classified.diagnostics[i]; const auto& b = complete.diagnostics[i];
                Require(a.severity == b.severity && a.subsystem == b.subsystem && a.code == b.code && a.message == b.message,
                    "classification changed the existing input diagnostic");
            }
        }
    };
    sameClassification(input, version, context.resourceIdentity);
    auto classifiedInput = input;
    classifiedInput.fields.category.value = "B"; sameClassification(classifiedInput, version, context.resourceIdentity);
    classifiedInput.fields.category = {PlacementFieldState::Opaque, {}, "original opaque field"};
    sameClassification(classifiedInput, version, context.resourceIdentity);
    classifiedInput = input; classifiedInput.fields.useClickHelper.value = false;
    sameClassification(classifiedInput, version, context.resourceIdentity);
    classifiedInput.fields.useClickHelper = {PlacementFieldState::Unavailable, {}, "missing helper fact"};
    sameClassification(classifiedInput, version, context.resourceIdentity);
    for (const auto type : {ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item, ObjectData::ObjectType::Doodad}) {
        classifiedInput = input; classifiedInput.object.type = type;
        classifiedInput.owner->file = type == ObjectData::ObjectType::Doodad ? MapData::PlacementFile::Doodads : MapData::PlacementFile::Units;
        sameClassification(classifiedInput, version, context.resourceIdentity);
    }
    classifiedInput = input; classifiedInput.object.type = ObjectData::ObjectType::Unit;
    sameClassification(classifiedInput, version, context.resourceIdentity); // 非辅助对象仍拒绝错误owner文件。
    classifiedInput = input; classifiedInput.owner->rawcode = "too-long";
    sameClassification(classifiedInput, version, context.resourceIdentity);
    classifiedInput = input; classifiedInput.fields.origins.clear(); sameClassification(classifiedInput, version, context.resourceIdentity);
    classifiedInput = input; classifiedInput.fields.useClickHelper.value.reset(); sameClassification(classifiedInput, version, context.resourceIdentity);
    classifiedInput = input; classifiedInput.fields.modelPath.value.assign(4097, 'x'); sameClassification(classifiedInput, version, context.resourceIdentity);
    auto classifiedVersion = version; ++classifiedVersion.document.revision;
    sameClassification(input, classifiedVersion, context.resourceIdentity);
    sameClassification(input, version, "different-resource-stack");
    PlacementPresentationLimits classifiedLimits; classifiedLimits.maxOrigins = 3;
    sameClassification(input, version, context.resourceIdentity, classifiedLimits);
    classifiedLimits = {}; ++classifiedLimits.maxTextureBytes;
    sameClassification(input, version, context.resourceIdentity, classifiedLimits);
    Require(query().value->inputIdentity == first.value->inputIdentity, "same frozen input identity was nondeterministic");
    ++*input.fields.origins[0].byteOffset; Require(query().value->inputIdentity != first.value->inputIdentity, "field source offset omitted from identity");
    texture = Tga(4, 4); auto large = query(); Require(large.Succeeded() && large.value->footprint->worldWidth == 128, "real header size was inferred from 2x2 filename");
    input.fields.modelPath.value.clear(); Require(query().value->kind == PlacementPresentationKind::PathingBlockerHelper, "helper classification incorrectly depended on empty model");
    const auto beforeDefaults = reads; input.fields.category.value = "B";
    Require(query().value->kind == PlacementPresentationKind::Default && query().value->footprintState == PlacementFootprintState::NotApplicable
        && reads == beforeDefaults, "empty model or path alone classified/read a blocker"); input.fields.category.value = "P";
    input.fields.useClickHelper.value = false; Require(query().value->kind == PlacementPresentationKind::Default && reads == beforeDefaults, "helper override ignored");
    input.fields.useClickHelper.state = PlacementFieldState::Opaque; auto opaque = query();
    Require(opaque.Succeeded() && opaque.value->kind == PlacementPresentationKind::Unknown && reads == beforeDefaults, "opaque helper guessed or read texture");
    input.fields.useClickHelper = {PlacementFieldState::Known, true, ""}; input.fields.pathingTexture.value = "_";
    Require(query().value->footprintState == PlacementFootprintState::Absent && reads == beforeDefaults, "explicit absent footprint read resource");
    input.fields.pathingTexture.value = "PathTextures\\2x2Default.tga"; deny = true; auto unreadable = query();
    Require(unreadable.Succeeded() && reads == beforeDefaults + 1 && unreadable.value->footprintState == PlacementFootprintState::Unavailable
        && unreadable.value->resourceDiagnostics.front().code == "READ_DENIED" && !unreadable.value->textureSource, "reader failure lowered source or became missing"); deny = false;
    texture.clear(); auto empty = query(); Require(empty.value->footprintState == PlacementFootprintState::Unavailable
        && empty.value->resourceDiagnostics.front().code == "PLACEMENT_FOOTPRINT_RESOURCE_EMPTY", "empty texture became unknown");
    texture = Tga(2, 2); texture.pop_back(); auto truncated = query(); Require(truncated.value->footprintState == PlacementFootprintState::Unavailable
        && truncated.value->resourceDiagnostics.front().code == "PLACEMENT_FOOTPRINT_PAYLOAD_INVALID", "supported truncated TGA became unsupported");
    texture.resize(texture.size() + 26, std::byte{0}); const std::string signature = "TRUEVISION-XFILE.";
    for (std::size_t i = 0; i < signature.size(); ++i) texture[texture.size() - 18 + i] = static_cast<std::byte>(signature[i]);
    Require(query().value->resourceDiagnostics.front().code == "PLACEMENT_FOOTPRINT_PAYLOAD_INVALID", "TGA footer concealed truncated pixel payload");
    texture = Tga(0, 2); Require(query().value->resourceDiagnostics.front().code == "PLACEMENT_FOOTPRINT_DIMENSION_INVALID", "zero dimension accepted");
    texture = Tga(2, 2); for (std::size_t byte = 12; byte != 16; ++byte) texture[byte] = std::byte{255};
    Require(query().value->resourceDiagnostics.front().code == "PLACEMENT_FOOTPRINT_PIXEL_BUDGET", "large dimensions overflowed budget");
    texture = Tga(2, 2); texture[2] = std::byte{10}; Require(query().value->footprintState == PlacementFootprintState::Unsupported, "unimplemented RLE guessed");
    wrongPath = true; Require(!query().Succeeded(), "different returned resource accepted"); wrongPath = false;
    auto stale = version; ++stale.document.revision; const auto beforeStale = reads;
    Require(!QueryPlacementPresentation(context, stale, context.resourceIdentity, input).Succeeded()
        && !QueryPlacementPresentation(context, version, "changed-stack", input).Succeeded() && reads == beforeStale, "stale input performed IO");
    PlacementPresentationLimits bounded; bounded.maxOrigins = 3; Require(!QueryPlacementPresentation(context, version, context.resourceIdentity, input, bounded).Succeeded(), "owned origin budget ignored");
    input.owner->file = MapData::PlacementFile::Units; Require(!query().Succeeded(), "incompatible owner file accepted"); input.owner->file = MapData::PlacementFile::Doodads;
    input.fields.origins.clear(); Require(!query().Succeeded(), "Known field without provenance accepted");
    std::cout << "placement presentation facts: inherited/override/opaque=covered, pixels32=64/128, native-footprint=unverified\n"; return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
