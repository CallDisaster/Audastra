#include "War3MapCore/Services/PlacementPresentationService.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <map>
#include <span>

namespace War3Map::Services {
namespace {
using Output = Result<PlacementPresentationRow>;
struct QueryPhaseTimer {
    using Clock = std::chrono::steady_clock;
    std::uint64_t* output;
    Clock::time_point started;
    explicit QueryPhaseTimer(std::uint64_t* destination)
        : output(destination), started(destination ? Clock::now() : Clock::time_point{}) {}
    void Finish() noexcept {
        if (!output) return;
        *output += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
        output = nullptr;
    }
    ~QueryPhaseTimer() { Finish(); }
};
bool ValidState(PlacementFieldState state) {
    return static_cast<unsigned>(state) <= static_cast<unsigned>(PlacementFieldState::Opaque);
}
bool OwnerTypeConsistent(const PlacementPresentationInput& input) {
    if (!input.owner) return true;
    switch (input.object.type) {
    case ObjectData::ObjectType::Unit: case ObjectData::ObjectType::Item:
        return input.owner->file == MapData::PlacementFile::Units;
    case ObjectData::ObjectType::Doodad: case ObjectData::ObjectType::Destructable:
        return input.owner->file == MapData::PlacementFile::Doodads;
    default: return false;
    }
}
bool HasOrigin(const PlacementPresentationFields& fields, std::string_view field) {
    return std::any_of(fields.origins.begin(), fields.origins.end(), [&](const auto& origin) {
        return origin.field == field && origin.objectRawcode.size() == 4
            && !origin.sourceId.empty() && !origin.sourcePath.empty();
    });
}
std::optional<std::size_t> InputBytes(const PlacementPresentationInput& input,
    std::string_view resourceIdentity, const PlacementPresentationLimits& limits) {
    std::size_t total = sizeof(PlacementPresentationRow) + 1024; // identity与固定分类/占地诊断文本预留。
    const auto text = [&](std::string_view value) {
        if (value.size() > limits.maxFieldBytes || value.size() > limits.maxLogicalBytes - std::min(total, limits.maxLogicalBytes)) return false;
        total += value.size(); return true;
    };
    if (!text(input.object.rawcode) || !text(resourceIdentity) || !text(input.version.document.documentId)
        || !text(input.version.document.sourceSha256) || (input.owner && !text(input.owner->rawcode))) return {};
    for (const auto* fact : {&input.fields.category, &input.fields.modelPath, &input.fields.pathingTexture})
        if (!ValidState(fact->state) || !text(fact->value) || !text(fact->reason)) return {};
    if (!ValidState(input.fields.useClickHelper.state) || !text(input.fields.useClickHelper.reason)
        || input.fields.origins.size() > limits.maxOrigins) return {};
    for (const auto& origin : input.fields.origins) {
        if (sizeof(origin) > limits.maxLogicalBytes - std::min(total, limits.maxLogicalBytes)) return {};
        total += sizeof(origin);
        if (!text(origin.field) || !text(origin.objectRawcode) || !text(origin.sourceId) || !text(origin.sourcePath)) return {};
    }
    return total <= limits.maxLogicalBytes ? std::optional(total) : std::nullopt;
}
bool OriginsComplete(const PlacementPresentationFields& fields) {
    return (fields.category.state != PlacementFieldState::Known || HasOrigin(fields, "bcat"))
        && (fields.modelPath.state != PlacementFieldState::Known || HasOrigin(fields, "bfil"))
        && (fields.pathingTexture.state != PlacementFieldState::Known || HasOrigin(fields, "bptx"))
        && (fields.useClickHelper.state != PlacementFieldState::Known || HasOrigin(fields, "buch"));
}
Result<std::size_t> ValidateInput(const PlacementPresentationContext& context,
    const WorldSelectionVersion& expectedVersion, std::string_view expectedResourceIdentity,
    const PlacementPresentationInput& input, const PlacementPresentationLimits& limits) {
    using SizeResult = Result<std::size_t>;
    const PlacementPresentationLimits hard;
    if (limits.maxFieldBytes > hard.maxFieldBytes || limits.maxOrigins > hard.maxOrigins
        || limits.maxLogicalBytes > hard.maxLogicalBytes || limits.maxTextureBytes > hard.maxTextureBytes
        || limits.maxTexturePixels > hard.maxTexturePixels || limits.maxLogicalBytes < sizeof(PlacementPresentationRow))
        return SizeResult::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_LIMIT_INVALID", "展示查询预算只能收紧且须容纳行头。");
    if (context.version != expectedVersion || input.version != expectedVersion || expectedResourceIdentity.empty()
        || context.resourceIdentity != expectedResourceIdentity)
        return SizeResult::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_SOURCE_STALE", "物编、地图或资源身份已变化，未返回旧展示事实。");
    const auto inputBytes = InputBytes(input, context.resourceIdentity, limits);
    if (!inputBytes) return SizeResult::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_INPUT_BUDGET", "有效字段或来源事实超过单查询预算。");
    if (input.object.rawcode.size() != 4
        || static_cast<unsigned>(input.object.type) > static_cast<unsigned>(ObjectData::ObjectType::Destructable)
        || (input.owner && (input.owner->rawcode.size() != 4
            || static_cast<unsigned>(input.owner->file) > static_cast<unsigned>(MapData::PlacementFile::Doodads)))
        || !OwnerTypeConsistent(input) || !OriginsComplete(input.fields)
        || (input.fields.useClickHelper.state == PlacementFieldState::Known && !input.fields.useClickHelper.value))
        return SizeResult::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_INPUT_INVALID", "有效字段状态、对象或Known字段来源不完整。");
    return SizeResult::Success(*inputBytes);
}
struct InputFieldsFragment {
    PlacementPresentationFields fields;
    std::string canonical;
};
struct InputSerializationCache {
    // 仅保存规范化计算，不保存展示行。上下文/limits由批次固定，原始字段仍逐行验证。
    static constexpr std::size_t budget = 8ull * 1024 * 1024;
    std::optional<std::string> prefix;
    std::map<std::pair<unsigned, std::string>, InputFieldsFragment> fields;
    std::map<std::string, Result<std::string>, std::less<>> paths;
    std::size_t ownedBytes = 0;
    [[nodiscard]] bool Fits(std::size_t charge) const { return charge <= budget - ownedBytes; }
};
void IdentityPart(std::string& canonical, std::string_view value) {
    canonical += std::to_string(value.size()); canonical += ':'; canonical += value;
}
template<class Value> void IdentityNumber(std::string& canonical, Value value) {
    IdentityPart(canonical, std::to_string(value));
}
std::string IdentityPrefix(const PlacementPresentationInput& input, std::string_view resourceIdentity) {
    std::string canonical = "placement-presentation-input-v1;";
    const auto part = [&](std::string_view value) { IdentityPart(canonical, value); };
    const auto number = [&](auto value) { IdentityNumber(canonical, value); };
    part(input.version.document.documentId); number(input.version.document.generation);
    number(input.version.document.revision); part(input.version.document.sourceSha256);
    number(input.version.catalogRevision); number(input.version.editorRevision); part(resourceIdentity);
    return canonical;
}
void AppendIdentityFields(std::string& canonical, const PlacementPresentationFields& fields) {
    const auto part = [&](std::string_view value) { IdentityPart(canonical, value); };
    const auto number = [&](auto value) { IdentityNumber(canonical, value); };
    for (const auto* fact : {&fields.category, &fields.modelPath, &fields.pathingTexture}) {
        number(static_cast<unsigned>(fact->state)); part(fact->value); part(fact->reason);
    }
    number(static_cast<unsigned>(fields.useClickHelper.state)); number(fields.useClickHelper.value.has_value());
    if (fields.useClickHelper.value) number(*fields.useClickHelper.value);
    part(fields.useClickHelper.reason); number(fields.origins.size());
    for (const auto& origin : fields.origins) {
        part(origin.field); part(origin.objectRawcode); part(origin.sourceId); part(origin.sourcePath);
        number(origin.byteOffset.has_value()); if (origin.byteOffset) number(*origin.byteOffset);
    }
}
bool SameIdentityFields(const PlacementPresentationFields& left, const PlacementPresentationFields& right) {
    const auto sameText = [](const PlacementTextFact& a, const PlacementTextFact& b) {
        return a.state == b.state && a.value == b.value && a.reason == b.reason;
    };
    if (!sameText(left.category, right.category) || !sameText(left.modelPath, right.modelPath)
        || !sameText(left.pathingTexture, right.pathingTexture)
        || left.useClickHelper.state != right.useClickHelper.state || left.useClickHelper.value != right.useClickHelper.value
        || left.useClickHelper.reason != right.useClickHelper.reason || left.origins.size() != right.origins.size()) return false;
    for (std::size_t i = 0; i < left.origins.size(); ++i) {
        const auto& a = left.origins[i]; const auto& b = right.origins[i];
        if (a.field != b.field || a.objectRawcode != b.objectRawcode || a.sourceId != b.sourceId
            || a.sourcePath != b.sourcePath || a.byteOffset != b.byteOffset) return false;
    }
    return true;
}
std::size_t FieldsOwnedBytes(const PlacementPresentationFields& fields) {
    std::size_t charge = sizeof(fields) + fields.origins.capacity() * sizeof(PlacementFieldOrigin);
    for (const auto* fact : {&fields.category, &fields.modelPath, &fields.pathingTexture})
        charge += fact->value.capacity() + fact->reason.capacity() + 2;
    charge += fields.useClickHelper.reason.capacity() + 1;
    for (const auto& origin : fields.origins)
        charge += origin.field.capacity() + origin.objectRawcode.capacity() + origin.sourceId.capacity() + origin.sourcePath.capacity() + 4;
    return charge;
}
Result<std::string> InputIdentity(const PlacementPresentationInput& input,
    std::string_view resourceIdentity, const PlacementPresentationLimits& limits,
    InputSerializationCache* cache, PlacementPresentationQueryTimings* timings) {
    QueryPhaseTimer canonicalTimer(timings ? &timings->identityCanonicalNs : nullptr);
    std::string canonical;
    if (cache && cache->prefix) canonical = *cache->prefix;
    else {
        canonical = IdentityPrefix(input, resourceIdentity);
        if (cache) {
            std::string prefix = canonical;
            const auto charge = sizeof(prefix) + prefix.capacity() + 1;
            if (cache->Fits(charge)) { cache->prefix = std::move(prefix); cache->ownedBytes += charge; }
        }
    }
    IdentityNumber(canonical, static_cast<unsigned>(input.object.type)); IdentityPart(canonical, input.object.rawcode);
    IdentityNumber(canonical, input.owner.has_value());
    if (input.owner) {
        IdentityNumber(canonical, static_cast<unsigned>(input.owner->file)); IdentityNumber(canonical, input.owner->index);
        IdentityPart(canonical, input.owner->rawcode); IdentityNumber(canonical, input.owner->creationId);
    }
    const auto fieldsStart = canonical.size();
    const auto key = std::pair{static_cast<unsigned>(input.object.type), input.object.rawcode};
    const InputFieldsFragment* existingFields = nullptr;
    if (cache) {
        const auto found = cache->fields.find(key);
        if (found != cache->fields.end()) existingFields = &found->second;
    }
    if (existingFields && SameIdentityFields(existingFields->fields, input.fields)) canonical += existingFields->canonical;
    else AppendIdentityFields(canonical, input.fields);
    if (canonical.size() > limits.maxLogicalBytes)
        return Result<std::string>::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_IDENTITY_BUDGET", "完整展示输入身份超过预算。");
    if (cache && !existingFields && cache->Fits(sizeof(InputFieldsFragment) + canonical.size() - fieldsStart)) {
        InputFieldsFragment fragment{input.fields, canonical.substr(fieldsStart)};
        const auto charge = FieldsOwnedBytes(fragment.fields) + sizeof(fragment.canonical) + fragment.canonical.capacity() + 1
            + sizeof(key) + key.second.capacity() + 1 + 4 * sizeof(void*);
        if (cache->Fits(charge)) { cache->fields.emplace(key, std::move(fragment)); cache->ownedBytes += charge; }
    }
    canonicalTimer.Finish();
    QueryPhaseTimer hashTimer(timings ? &timings->identityHashNs : nullptr);
    return Sha256Text(canonical);
}
std::optional<std::size_t> DiagnosticBytes(std::span<const Diagnostic> diagnostics,
    const PlacementPresentationLimits& limits) {
    if (diagnostics.size() > 64) return {};
    std::size_t total = 0;
    for (const auto& diagnostic : diagnostics) {
        const auto pathChars = diagnostic.origin ? diagnostic.origin->source.native().size() : 0;
        if (pathChars > limits.maxFieldBytes / sizeof(std::filesystem::path::value_type)) return {};
        const auto pathBytes = pathChars * sizeof(std::filesystem::path::value_type);
        const auto fingerprintBytes = diagnostic.origin ? diagnostic.origin->fingerprint.size() : 0;
        if (diagnostic.subsystem.size() > limits.maxFieldBytes || diagnostic.code.size() > limits.maxFieldBytes
            || diagnostic.message.size() > limits.maxFieldBytes || pathBytes > limits.maxFieldBytes || fingerprintBytes > limits.maxFieldBytes) return {};
        const auto bytes = sizeof(diagnostic) + diagnostic.subsystem.size() + diagnostic.code.size()
            + diagnostic.message.size() + pathBytes + fingerprintBytes;
        if (bytes > limits.maxLogicalBytes - total) return {};
        total += bytes;
    }
    return total;
}
const Result<std::string>& CanonicalInputPath(std::string_view path, InputSerializationCache* cache,
    Result<std::string>& uncached) {
    if (cache) {
        const auto found = cache->paths.find(path);
        if (found != cache->paths.end()) return found->second;
    }
    uncached = Resources::CanonicalImportPath(path);
    if (cache) {
        std::string key(path);
        auto charge = sizeof(key) + key.capacity() + 1 + sizeof(uncached) + 4 * sizeof(void*)
            + uncached.diagnostics.capacity() * sizeof(Diagnostic);
        if (uncached.value) charge += uncached.value->capacity() + 1;
        for (const auto& diagnostic : uncached.diagnostics) {
            charge += diagnostic.subsystem.capacity() + diagnostic.code.capacity() + diagnostic.message.capacity() + 3;
            if (diagnostic.origin) charge += diagnostic.origin->source.native().capacity() * sizeof(std::filesystem::path::value_type)
                + diagnostic.origin->fingerprint.capacity() + sizeof(std::filesystem::path::value_type) + 1;
        }
        // key保留原始字节；大小写、分隔符或无效路径之间不能借用另一输入的结果。
        if (cache->Fits(charge)) {
            const auto stored = cache->paths.emplace(std::move(key), std::move(uncached));
            cache->ownedBytes += charge; return stored.first->second;
        }
    }
    return uncached;
}
PlacementPresentationKind Classify(const ObjectData::ObjectKey& object, const PlacementPresentationFields& fields,
    std::string* reason = nullptr) {
    if (object.type != ObjectData::ObjectType::Destructable) return PlacementPresentationKind::Default;
    if (fields.category.state == PlacementFieldState::Absent
        || (fields.category.state == PlacementFieldState::Known && fields.category.value != "P"))
        return PlacementPresentationKind::Default;
    if (fields.category.state != PlacementFieldState::Known) {
        if (reason) *reason = "有效类别不可用或为opaque，未根据模型路径猜测。";
        return PlacementPresentationKind::Unknown;
    }
    if (fields.useClickHelper.state != PlacementFieldState::Known || !fields.useClickHelper.value) {
        if (reason) *reason = "P类编辑器辅助标记不可用或未明确，保留未决分类。";
        return PlacementPresentationKind::Unknown;
    }
    return *fields.useClickHelper.value ? PlacementPresentationKind::PathingBlockerHelper : PlacementPresentationKind::Default;
}
std::uint16_t U16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])
        | (std::to_integer<unsigned>(bytes[offset + 1]) << 8));
}
std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8) value |= std::to_integer<std::uint32_t>(bytes[offset++]) << shift;
    return value;
}
struct FootprintDescription {
    PlacementFootprintState footprintState = PlacementFootprintState::Unknown;
    std::string footprintReason;
    std::optional<PlacementPathingFootprint> footprint;
    std::vector<Diagnostic> resourceDiagnostics;
};
void DescribeTga(FootprintDescription& row, std::span<const std::byte> bytes,
    const PlacementPresentationLimits& limits) {
    const auto invalid = [&](std::string code, std::string message) {
        row.footprintState = PlacementFootprintState::Unavailable; row.footprintReason = message;
        auto diagnostic = Status::Failure("PlacementPresentation", std::move(code), std::move(message));
        row.resourceDiagnostics.push_back(std::move(diagnostic.diagnostics.front()));
    };
    if (bytes.empty()) { invalid("PLACEMENT_FOOTPRINT_RESOURCE_EMPTY", "占地资源为空，无法取得真实尺寸。"); return; }
    if (bytes.size() < 18) {
        invalid("PLACEMENT_FOOTPRINT_HEADER_INVALID", "TGA头截断，未猜测占地。"); return;
    }
    const auto byte = [&](std::size_t index) { return std::to_integer<unsigned>(bytes[index]); };
    if (byte(1) != 0 || U16(bytes, 3) != 0 || U16(bytes, 5) != 0 || byte(7) != 0
        || byte(2) != 2 || (byte(16) != 24 && byte(16) != 32) || (byte(17) & 0xc0u) != 0) {
        row.footprintState = PlacementFootprintState::Unsupported;
        row.footprintReason = "占地来源不是当前支持的无色表、非压缩24/32-bit TGA。"; return;
    }
    const auto width = U16(bytes, 12), height = U16(bytes, 14);
    const auto pixels = static_cast<std::size_t>(width) * height;
    const auto start = 18u + byte(0), pixelBytes = byte(16) / 8u;
    if (width == 0 || height == 0) { invalid("PLACEMENT_FOOTPRINT_DIMENSION_INVALID", "TGA含零维度，未补默认占地。"); return; }
    if (pixels > limits.maxTexturePixels) { invalid("PLACEMENT_FOOTPRINT_PIXEL_BUDGET", "TGA像素数超出本查询预算。"); return; }
    std::size_t payloadEnd = bytes.size();
    constexpr std::string_view footerSignature = "TRUEVISION-XFILE.";
    if (bytes.size() >= 26 && bytes.back() == std::byte{0}
        && std::equal(footerSignature.begin(), footerSignature.end(), bytes.end() - 18,
            [](char a, std::byte b) { return static_cast<unsigned char>(a) == std::to_integer<unsigned char>(b); })) {
        payloadEnd -= 26;
        const auto footerStart = payloadEnd;
        for (const auto offset : {U32(bytes, bytes.size() - 26), U32(bytes, bytes.size() - 22)}) {
            if (offset == 0) continue;
            if (offset < start || offset > footerStart) {
                invalid("PLACEMENT_FOOTPRINT_METADATA_INVALID", "TGA页尾扩展/开发者位置越界，未将其当成像素。"); return;
            }
            payloadEnd = std::min(payloadEnd, static_cast<std::size_t>(offset));
        }
    }
    if (start > payloadEnd || pixels > (payloadEnd - start) / pixelBytes) {
        invalid("PLACEMENT_FOOTPRINT_PAYLOAD_INVALID", "TGA真实像素载荷截断，未从文件名/选择尺寸补值。"); return;
    }
    PlacementPathingFootprint footprint;
    footprint.pixelWidth = width; footprint.pixelHeight = height;
    footprint.worldWidth = static_cast<float>(width) * footprint.worldUnitsPerPixel;
    footprint.worldHeight = static_cast<float>(height) * footprint.worldUnitsPerPixel;
    row.footprintState = PlacementFootprintState::Available; row.footprint = footprint;
}
struct FootprintResourceFact {
    Result<Resources::ResolvedResource> owned;
    std::optional<std::size_t> diagnosticBytes;
    Status sourceValidation;
    std::size_t locationBytes = 0;
    bool described = false;
    Status descriptionStatus;
    std::optional<std::string> sha256;
    FootprintDescription description;
};
struct FootprintResourceCache {
    // 仅一次冻结批次中的实际读取闭包，不以行号、owner或旧地图版本缓存展示行。
    std::map<std::string, std::shared_ptr<FootprintResourceFact>, std::less<>> facts;
    std::size_t ownedBytes = 0;
};
std::shared_ptr<FootprintResourceFact> CaptureFootprintResource(const PlacementPresentationContext& context,
    std::string_view canonicalPath, const PlacementPresentationLimits& limits, FootprintResourceCache* cache) {
    if (cache) {
        const auto found = cache->facts.find(canonicalPath);
        if (found != cache->facts.end()) return found->second;
    }
    auto fact = std::make_shared<FootprintResourceFact>();
    fact->owned = context.resourceReader(canonicalPath, limits.maxTextureBytes);
    fact->diagnosticBytes = DiagnosticBytes(fact->owned.diagnostics, limits);
    if (fact->diagnosticBytes && fact->owned.Succeeded()) {
        const auto& resource = *fact->owned.value;
        const auto& location = resource.location;
        if (resource.bytes.size() > limits.maxTextureBytes)
            fact->sourceValidation = Status::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_TEXTURE_BUDGET", "资源reader返回了超过捕获预算的字节。");
        else {
            const auto returnedPath = Resources::CanonicalImportPath(location.canonicalPath);
            if (!returnedPath.Succeeded() || *returnedPath.value != canonicalPath || location.sourceId.empty() || location.sourcePath.empty()
                || location.sourceId.size() > limits.maxFieldBytes || location.sourcePath.size() > limits.maxFieldBytes)
                fact->sourceValidation = Status::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_RESOURCE_INVALID", "占地资源与请求路径或实际来源不一致。");
            else fact->locationBytes = location.canonicalPath.size() + location.sourceId.size() + location.sourcePath.size() + sizeof(location) + 512;
        }
    }
    if (cache && fact->diagnosticBytes && fact->sourceValidation.Succeeded()
        && (fact->owned.Succeeded() || !fact->owned.value)) {
        // 同时计入真实载荷、来源/诊断及固定解析元数据。缓存满后仍逐行读取，不限制地图实例数。
        constexpr std::size_t budget = 64ull * 1024 * 1024;
        const auto payloadBytes = fact->owned.value ? fact->owned.value->bytes.size() : 0;
        const auto charge = sizeof(FootprintResourceFact) + canonicalPath.size() + *fact->diagnosticBytes
            + fact->locationBytes + payloadBytes + 1024;
        if (charge <= budget - cache->ownedBytes) {
            cache->facts.emplace(canonicalPath, fact); cache->ownedBytes += charge;
        }
    }
    return fact;
}
Status DescribeFootprintResource(FootprintResourceFact& fact, const PlacementPresentationLimits& limits) {
    if (!fact.described) {
        auto sha = Sha256(fact.owned.value->bytes);
        if (!sha.Succeeded()) fact.descriptionStatus.diagnostics = std::move(sha.diagnostics);
        else {
            FootprintDescription description;
            DescribeTga(description, fact.owned.value->bytes, limits);
            fact.sha256 = std::move(*sha.value);
            fact.description = std::move(description);
        }
        fact.described = true;
    }
    return fact.descriptionStatus;
}
}

Result<PlacementPresentationKind> ClassifyPlacementPresentation(const PlacementPresentationContext& context,
    const WorldSelectionVersion& expectedVersion, std::string_view expectedResourceIdentity,
    const PlacementPresentationInput& input, const PlacementPresentationLimits& limits) try {
    const auto valid = ValidateInput(context, expectedVersion, expectedResourceIdentity, input, limits);
    if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    return Result<PlacementPresentationKind>::Success(Classify(input.object, input.fields));
} catch (const std::exception&) {
    return Result<PlacementPresentationKind>::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_QUERY_FAILED", "展示事实查询失败，未返回部分结果。");
}

namespace {
Output QueryPlacementPresentationImpl(const PlacementPresentationContext& context,
    const WorldSelectionVersion& expectedVersion, std::string_view expectedResourceIdentity,
    const PlacementPresentationInput& input, const PlacementPresentationLimits& limits,
    FootprintResourceCache* cache, InputSerializationCache* inputCache, PlacementPresentationQueryTimings* timings) try {
    if (timings) *timings = {};
    QueryPhaseTimer totalTimer(timings ? &timings->totalNs : nullptr);
    Result<std::size_t> valid;
    {
        QueryPhaseTimer validateTimer(timings ? &timings->validateNs : nullptr);
        valid = ValidateInput(context, expectedVersion, expectedResourceIdentity, input, limits);
    }
    if (!valid.Succeeded()) return {std::nullopt, valid.diagnostics};
    const auto inputBytes = *valid.value;
    PlacementPresentationRow row;
    {
        QueryPhaseTimer rowTimer(timings ? &timings->rowCopyNs : nullptr);
        row.object = input.object; row.owner = input.owner; row.version = expectedVersion;
        row.resourceIdentity = context.resourceIdentity; row.fields = input.fields;
        row.kind = Classify(input.object, input.fields, &row.classificationReason);
    }
    auto identity = InputIdentity(input, context.resourceIdentity, limits, inputCache, timings);
    if (!identity.Succeeded()) return {std::nullopt, std::move(identity.diagnostics)};
    row.inputIdentity = std::move(*identity.value);
    if (row.kind != PlacementPresentationKind::PathingBlockerHelper) {
        if (row.kind == PlacementPresentationKind::Default) row.footprintState = PlacementFootprintState::NotApplicable;
        return Output::Success(std::move(row));
    }
    QueryPhaseTimer resourceTimer(timings ? &timings->resourceNs : nullptr);
    const auto& path = row.fields.pathingTexture;
    if (path.state == PlacementFieldState::Absent
        || (path.state == PlacementFieldState::Known && (path.value.empty() || path.value == "_"))) {
        row.footprintState = PlacementFootprintState::Absent; return Output::Success(std::move(row));
    }
    if (path.state != PlacementFieldState::Known) {
        row.footprintReason = "有效占地字段未知或为opaque，未读取猜测路径。"; return Output::Success(std::move(row));
    }
    Result<std::string> uncachedPath;
    const auto& canonical = CanonicalInputPath(path.value, inputCache, uncachedPath);
    if (!canonical.Succeeded()) {
        row.footprintState = PlacementFootprintState::Unavailable; row.resourceDiagnostics = canonical.diagnostics;
        row.footprintReason = "占地虚拟路径无效。"; return Output::Success(std::move(row));
    }
    if (!context.resourceReader) {
        row.footprintState = PlacementFootprintState::Unavailable;
        row.footprintReason = "当前冻结资源reader不可用。";
        row.resourceDiagnostics = Status::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_READER_UNAVAILABLE", row.footprintReason).diagnostics;
        return Output::Success(std::move(row));
    }
    const auto resource = CaptureFootprintResource(context, *canonical.value, limits, cache);
    const bool resourceSucceeded = resource->owned.Succeeded();
    const auto diagnosticBytes = resource->diagnosticBytes;
    if (!diagnosticBytes || *diagnosticBytes > limits.maxLogicalBytes - inputBytes)
        return Output::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_DIAGNOSTIC_BUDGET", "资源诊断超出owned行预算。");
    row.resourceDiagnostics = resource->owned.diagnostics;
    if (!resourceSucceeded) {
        row.footprintState = PlacementFootprintState::Unavailable;
        row.footprintReason = "占地资源读取失败，保留实际失败且未读取低层来源。";
        return Output::Success(std::move(row));
    }
    if (!resource->sourceValidation.Succeeded()) return {std::nullopt, resource->sourceValidation.diagnostics};
    if (resource->locationBytes > limits.maxLogicalBytes - inputBytes - *diagnosticBytes)
        return Output::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_OUTPUT_BUDGET", "占地来源与行输出超过预算。");
    resourceTimer.Finish();
    QueryPhaseTimer footprintTimer(timings ? &timings->footprintNs : nullptr);
    const auto described = DescribeFootprintResource(*resource, limits);
    if (!described.Succeeded()) return {std::nullopt, described.diagnostics};
    row.textureSource = resource->owned.value->location; row.textureSha256 = resource->sha256;
    row.footprintState = resource->description.footprintState;
    row.footprintReason = resource->description.footprintReason;
    row.footprint = resource->description.footprint;
    row.resourceDiagnostics.insert(row.resourceDiagnostics.end(), resource->description.resourceDiagnostics.begin(),
        resource->description.resourceDiagnostics.end());
    return Output::Success(std::move(row));
} catch (const std::exception&) {
    return Output::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_QUERY_FAILED", "展示事实查询失败，未返回部分结果。");
}
} // namespace

Output QueryPlacementPresentation(const PlacementPresentationContext& context,
    const WorldSelectionVersion& expectedVersion, std::string_view expectedResourceIdentity,
    const PlacementPresentationInput& input, const PlacementPresentationLimits& limits) {
    return QueryPlacementPresentationImpl(context, expectedVersion, expectedResourceIdentity, input, limits, nullptr, nullptr, nullptr);
}

struct PlacementPresentationBatch::Impl {
    PlacementPresentationContext context;
    PlacementPresentationLimits limits;
    FootprintResourceCache resources;
    InputSerializationCache serialization;
};
PlacementPresentationBatch::PlacementPresentationBatch(PlacementPresentationContext context, PlacementPresentationLimits limits)
    : impl_(std::make_unique<Impl>(Impl{std::move(context), limits, {}, {}})) {}
PlacementPresentationBatch::~PlacementPresentationBatch() = default;
PlacementPresentationBatch::PlacementPresentationBatch(PlacementPresentationBatch&&) noexcept = default;
PlacementPresentationBatch& PlacementPresentationBatch::operator=(PlacementPresentationBatch&&) noexcept = default;
Output PlacementPresentationBatch::Query(const WorldSelectionVersion& expectedVersion,
    std::string_view expectedResourceIdentity, const PlacementPresentationInput& input,
    PlacementPresentationQueryTimings* timings) {
    if (timings) *timings = {};
    if (!impl_) return Output::Failure("PlacementPresentation", "PLACEMENT_PRESENTATION_QUERY_FAILED", "展示查询批次已转移，未返回旧结果。");
    return QueryPlacementPresentationImpl(impl_->context, expectedVersion, expectedResourceIdentity, input,
        impl_->limits, &impl_->resources, &impl_->serialization, timings);
}
} // namespace War3Map::Services
