#include "War3MapCore/Render/ClassicMinimapObjectAssets.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainEditing.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

namespace War3Map::Render {
namespace {

constexpr const char* kSubsystem = "ClassicMinimapObjectAssets";
constexpr ClassicMinimapObjectAssetLimits kHard{};
constexpr std::size_t kFixedScratch = 2u * 1024u * 1024u;
constexpr std::size_t kMetadataReserve = 64u * 1024u;
constexpr std::array<MapDataKind, 2> kKinds{MapDataKind::Units, MapDataKind::Doodads};
const std::array<std::string, 2> kMembers{"war3mapUnits.doo", "war3map.doo"};

// Snapshot(kind)先复制DOO/W3I再使用现有MapDocument解析器。128N用于其有界wire
// 到各类容器的保守载荷预留；下面锁定现有结构ABI，不把它称为allocator/RSS实测。
static_assert(sizeof(std::string) <= 64 && sizeof(MapData::MapSettings) <= 2048
    && sizeof(MapData::UnitPlacement) <= 1024 && sizeof(MapData::DoodadPlacement) <= 512
    && sizeof(MapData::SpecialDoodadPlacement) <= 128 && sizeof(MapData::PlacementItemSet) <= 64
    && sizeof(MapData::PlacementItemChoice) <= 128 && sizeof(MapData::MapPlayerSettings) <= 160
    && sizeof(MapData::MapRandomUnitRow) <= 64 && sizeof(MapData::MapRandomUnitTable) <= 192
    && sizeof(MapData::MapRandomItemTable) <= 192);

struct Failure { std::vector<Diagnostic> diagnostics; };

[[noreturn]] void Fail(const char* code, const char* message) {
    throw Failure{{{DiagnosticSeverity::Error, kSubsystem, code, message, {}}}};
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)};
    if (!result.value) Fail("CLASSIC_OBJECT_CAPTURE_NO_VALUE", "来源依赖没有返回完整值。");
    return std::move(*result.value);
}

void Check(Status status) { if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)}; }

void Charge(std::size_t& bytes, std::size_t count, std::size_t element = 1) {
    if (element && count > (std::numeric_limits<std::size_t>::max() - bytes) / element)
        Fail("CLASSIC_OBJECT_CAPTURE_BUDGET", "对象捕获载荷乘加溢出。");
    bytes += count * element;
}

void TextCapacity(std::size_t& bytes, const std::string& text) {
    Charge(bytes, text.capacity()); Charge(bytes, 1);
}

void LocationCapacity(std::size_t& bytes, const Resources::ResourceLocation& location) {
    TextCapacity(bytes, location.canonicalPath); TextCapacity(bytes, location.sourceId); TextCapacity(bytes, location.sourcePath);
    if (location.readProvenance) {
        Charge(bytes, sizeof(Resources::ResourceReadProvenance));
        TextCapacity(bytes, location.readProvenance->containerIdentity);
        TextCapacity(bytes, location.readProvenance->memberPath);
        TextCapacity(bytes, location.readProvenance->sourceContext);
    }
}

std::size_t TerrainCapacity(const TerrainMinimapAssetSnapshot& snapshot) {
    const auto& input = snapshot.Input();
    const auto& binding = snapshot.Binding();
    std::size_t bytes = sizeof(snapshot);
    TextCapacity(bytes, binding.archiveSha256); TextCapacity(bytes, binding.originalTerrainSha256);
    TextCapacity(bytes, binding.currentTerrainSha256);
    Charge(bytes, binding.gameRoot.native().capacity(), sizeof(std::filesystem::path::value_type));
    Charge(bytes, 1, sizeof(std::filesystem::path::value_type));
    TextCapacity(bytes, input.binding.documentId); TextCapacity(bytes, input.binding.sourceSha256);
    TextCapacity(bytes, input.binding.terrainSha256); Charge(bytes, input.currentW3e.capacity());
    if (input.sourceW3i) Charge(bytes, input.sourceW3i->capacity());
    Charge(bytes, input.groundAtlases.capacity(), sizeof(std::shared_ptr<const TerrainAtlasSnapshot>));
    if (input.classicColors) {
        Charge(bytes, input.classicColors->cliffColors.capacity(), sizeof(std::array<std::uint8_t, 4>));
    }
    for (std::size_t i = 0; i < input.groundAtlases.size(); ++i) {
        const auto& owner = input.groundAtlases[i];
        if (!owner) Fail("CLASSIC_OBJECT_CAPTURE_TERRAIN", "传入地形快照缺少atlas owner。");
        if (std::find(input.groundAtlases.begin(), input.groundAtlases.begin() + static_cast<std::ptrdiff_t>(i), owner)
            != input.groundAtlases.begin() + static_cast<std::ptrdiff_t>(i)) continue;
        Charge(bytes, sizeof(*owner)); LocationCapacity(bytes, owner->source); TextCapacity(bytes, owner->sourceSha256);
        Charge(bytes, owner->image.rgba.capacity()); Charge(bytes, owner->cells.capacity(), sizeof(TerrainAtlasCell));
        Charge(bytes, owner->additionalMips.capacity(), sizeof(Image::DecodedImage));
        for (const auto& mip : owner->additionalMips) Charge(bytes, mip.rgba.capacity());
    }
    Charge(bytes, snapshot.Dependencies().capacity(), sizeof(TerrainMinimapAssetDependency));
    for (const auto& dependency : snapshot.Dependencies()) {
        LocationCapacity(bytes, dependency.winner); TextCapacity(bytes, dependency.containerIdentity);
        TextCapacity(bytes, dependency.encodedSha256);
        Charge(bytes, dependency.decodedMips.capacity(), sizeof(TerrainMinimapMipFacts));
        for (const auto& mip : dependency.decodedMips) TextCapacity(bytes, mip.decodedRgbaSha256);
    }
    Charge(bytes, snapshot.Tables().capacity(), sizeof(TerrainMinimapTableFacts));
    for (const auto& table : snapshot.Tables()) { TextCapacity(bytes, table.idColumn); TextCapacity(bytes, table.identitySha256); }
    Charge(bytes, snapshot.GroundAssets().capacity(), sizeof(TerrainMinimapGroundAsset));
    for (const auto& ground : snapshot.GroundAssets()) { TextCapacity(bytes, ground.rawcode); TextCapacity(bytes, ground.canonicalTexturePath); }
    TextCapacity(bytes, snapshot.IdentitySha256());
    return bytes;
}

void ValidateLimits(const ClassicMinimapObjectAssetLimits& limits) {
    if (!limits.maxWorkingBytes || limits.maxWorkingBytes > kHard.maxWorkingBytes
        || !limits.maxObjectMemberBytes || limits.maxObjectMemberBytes > kHard.maxObjectMemberBytes
        || !limits.maxCurrentSnapshotBytes || limits.maxCurrentSnapshotBytes > kHard.maxCurrentSnapshotBytes
        || !limits.maxCurrentTerrainBytes || limits.maxCurrentTerrainBytes > kHard.maxCurrentTerrainBytes
        || !limits.maxPlacements || limits.maxPlacements > kHard.maxPlacements
        || !limits.maxNestedRecords || limits.maxNestedRecords > kHard.maxNestedRecords)
        Fail("CLASSIC_OBJECT_CAPTURE_LIMITS", "对象捕获预算只能在非零硬上限内收紧。");
}

bool MatchesTerrainCaptureBinding(const TerrainMinimapAssetSnapshot& terrain, const MapArchiveSession& source,
    const MapData::MapDataEditSession& edits) {
    const auto& binding = terrain.Binding();
    const auto validSha = [](std::string_view value) {
        return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    return validSha(terrain.IdentitySha256()) && source.SourceSha256() == binding.archiveSha256 && edits.SessionIdentity() == binding.sessionIdentity
        && edits.Revision() == binding.revision && edits.TerrainSourceRevision() == binding.terrainSourceRevision
        && terrain.Input().binding.sourceSha256 == binding.archiveSha256
        && terrain.Input().binding.terrainSha256 == binding.currentTerrainSha256
        && terrain.Input().binding.documentGeneration == binding.sessionIdentity
        && terrain.Input().binding.revision == binding.revision;
}

void ParseObjectMember(std::size_t index, const ArchiveExactMember& source,
    ClassicMinimapObjectMemberFacts& facts, const ClassicMinimapObjectAssetLimits& limits,
    MapData::UnitPlacements& units, MapData::DoodadPlacements& doodads) {
    facts.memberName = kMembers[index];
    if (!source.present) return;
    if (source.bytes.empty())
        Fail("CLASSIC_OBJECT_CAPTURE_ZERO_BYTES", "present零字节DOO不是合法空对象层。");
    MapData::PlacementReadLimits parser;
    parser.maxBytes = limits.maxObjectMemberBytes;
    parser.maxPlacements = limits.maxPlacements; parser.maxNestedRecords = limits.maxNestedRecords;
    if (!index) {
        auto parsed = Take(MapData::ParseUnitPlacements(source.bytes, parser));
        if (parsed.recordLayout != MapData::PlacementRecordLayout::Classic)
            Fail("CLASSIC_OBJECT_CAPTURE_LAYOUT", "单位层未由唯一classic parser证明为classic布局。");
        facts.version = parsed.version; facts.subversion = parsed.subversion;
        facts.recordLayout = parsed.recordLayout; facts.parserFullyConsumed = true;
        facts.ordinaryPlacements = parsed.units.size();
        facts.state = parsed.units.empty() ? ClassicMinimapCapturedObjectState::ParsedEmpty
                                           : ClassicMinimapCapturedObjectState::Parsed;
        units = std::move(parsed);
    } else {
        auto parsed = Take(MapData::ParseDoodadPlacements(source.bytes, parser));
        if (parsed.recordLayout != MapData::PlacementRecordLayout::Classic)
            Fail("CLASSIC_OBJECT_CAPTURE_LAYOUT", "装饰物层未由唯一classic parser证明为classic布局。");
        facts.version = parsed.version; facts.subversion = parsed.subversion;
        facts.recordLayout = parsed.recordLayout; facts.specialVersion = parsed.specialVersion; facts.parserFullyConsumed = true;
        facts.ordinaryPlacements = parsed.doodads.size(); facts.specialPlacements = parsed.specialDoodads.size();
        facts.state = (parsed.doodads.empty() && parsed.specialDoodads.empty())
            ? ClassicMinimapCapturedObjectState::ParsedEmpty : ClassicMinimapCapturedObjectState::Parsed;
        doodads = std::move(parsed);
    }
    facts.sourceBytes = source.bytes.size();
    facts.sourceSha256 = Take(Sha256(source.bytes));
}

void CheckCurrentMember(std::size_t index, const ArchiveExactMember& source,
    const MapData::MapDataEditSession& edits, ClassicMinimapObjectMemberFacts& facts,
    const ClassicMinimapObjectAssetLimits& limits) {
    // 两次Snapshot串行，MapDocument在本函数返回时释放，避免并存两个W3I解析。
    const auto current = Take(edits.Snapshot(kKinds[index], limits.maxCurrentSnapshotBytes));
    const auto& member = current.Member(kKinds[index]);
    if (!source.present) {
        if (member.state != MapDataState::Missing || !member.sources.empty())
            Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_MISMATCH", "源Missing与当前对象成员存在性不一致。");
        facts.sourceAndCurrentBytesEqual = true;
        return;
    }
    if (member.state != MapDataState::Ready || member.memberName != kMembers[index] || member.sources.size() != 1
        || member.sources.front().name != kMembers[index] || member.sources.front().data != source.bytes)
        Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_MISMATCH", "当前对象成员必须唯一Ready且与准确源字节相同。");
    if (!index) {
        if (!current.Units() || current.Units()->recordLayout != MapData::PlacementRecordLayout::Classic)
            Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_LAYOUT", "当前单位层不是已接通的classic布局。");
    } else if (!current.Doodads() || current.Doodads()->recordLayout != MapData::PlacementRecordLayout::Classic) {
        Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_LAYOUT", "当前装饰物层不是已接通的classic零记录布局。");
    }
    facts.currentBytes = member.sources.front().data.size();
    facts.currentSha256 = Take(Sha256(member.sources.front().data));
    if (facts.currentBytes != facts.sourceBytes || facts.currentSha256 != facts.sourceSha256)
        Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_MISMATCH", "对象当前原字与源摘要不一致。");
    facts.sourceAndCurrentBytesEqual = true;
}

class Identity {
public:
    Identity() : digest_(1024u * 1024u) {}
    void U64(std::uint64_t value) {
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<std::byte>((value >> (i * 8)) & 255);
        Check(digest_.Update(bytes));
    }
    void Text(std::string_view text) { U64(text.size()); Check(digest_.Update(std::as_bytes(std::span(text.data(), text.size())))); }
    std::string Finish() { return Take(digest_.Finish()); }
private:
    Sha256Stream digest_;
};

std::size_t PlacementCapacity(std::size_t bytes, const MapData::UnitPlacements& units,
    const MapData::DoodadPlacements& doodads);

std::size_t AuxiliaryCapacity(const ClassicMinimapObjectDomain& domain, const ClassicMinimapObjectAssetFacts& facts,
    const std::array<std::vector<std::byte>, 2>& sourceMembers, const MapData::UnitPlacements& units,
    const MapData::DoodadPlacements& doodads, const ClassicMinimapObjectLayer& objectLayer, const std::string& identity) {
    // 组合对象的其余固定字段和padding也计入，Terrain owner本体已在TerrainCapacity收取。
    std::size_t bytes = sizeof(ClassicMinimapObjectAssets) - sizeof(TerrainMinimapAssetSnapshot);
    TextCapacity(bytes, domain.sourceSha256); TextCapacity(bytes, domain.documentId);
    TextCapacity(bytes, domain.unitsMemberSha256); TextCapacity(bytes, domain.doodadsMemberSha256);
    for (const auto& member : facts.members) {
        TextCapacity(bytes, member.memberName); TextCapacity(bytes, member.sourceSha256); TextCapacity(bytes, member.currentSha256);
    }
    for (const auto& member : sourceMembers) Charge(bytes, member.capacity());
    bytes = PlacementCapacity(bytes, units, doodads);
    Charge(bytes, objectLayer.pixels.capacity(), sizeof(ClassicMinimapObjectPixel));
    Charge(bytes, objectLayer.styles.capacity(), sizeof(ClassicMinimapObjectStyle));
    for (const auto& style : objectLayer.styles) {
        TextCapacity(bytes, style.object.rawcode); TextCapacity(bytes, style.modelPath);
        TextCapacity(bytes, style.pathingTexture); TextCapacity(bytes, style.replaceableTexture);
    }
    Charge(bytes, objectLayer.dependencies.capacity(), sizeof(ClassicMinimapObjectLayer::Dependency));
    for (const auto& dependency : objectLayer.dependencies) {
        TextCapacity(bytes, dependency.winner.canonicalPath); TextCapacity(bytes, dependency.winner.sourceId);
        TextCapacity(bytes, dependency.winner.sourcePath); TextCapacity(bytes, dependency.encodedSha256);
    }
    for (const auto& diagnostic : objectLayer.diagnostics) TextCapacity(bytes, diagnostic.message);
    TextCapacity(bytes, identity);
    return bytes;
}

std::size_t PlacementCapacity(std::size_t bytes, const MapData::UnitPlacements& units,
    const MapData::DoodadPlacements& doodads) {
    Charge(bytes, units.units.capacity(), sizeof(MapData::UnitPlacement));
    for (const auto& unit : units.units) {
        TextCapacity(bytes, unit.rawcode); if (unit.skinId) TextCapacity(bytes, *unit.skinId);
        for (const auto& set : unit.itemSets) { Charge(bytes, set.items.capacity(), sizeof(MapData::PlacementItemChoice));
            for (const auto& item : set.items) TextCapacity(bytes, item.rawcode); }
        Charge(bytes, unit.inventory.capacity(), sizeof(MapData::PlacementInventoryItem));
        for (const auto& item : unit.inventory) TextCapacity(bytes, item.rawcode);
        Charge(bytes, unit.abilities.capacity(), sizeof(MapData::PlacementAbility));
        for (const auto& ability : unit.abilities) TextCapacity(bytes, ability.rawcode);
    }
    Charge(bytes, doodads.doodads.capacity(), sizeof(MapData::DoodadPlacement));
    for (const auto& doodad : doodads.doodads) {
        TextCapacity(bytes, doodad.rawcode); if (doodad.skinId) TextCapacity(bytes, *doodad.skinId);
        for (const auto& set : doodad.itemSets) { Charge(bytes, set.items.capacity(), sizeof(MapData::PlacementItemChoice));
            for (const auto& item : set.items) TextCapacity(bytes, item.rawcode); }
    }
    Charge(bytes, doodads.specialDoodads.capacity(), sizeof(MapData::SpecialDoodadPlacement));
    for (const auto& special : doodads.specialDoodads) TextCapacity(bytes, special.rawcode);
    Charge(bytes, units.OriginalBytes().capacity()); Charge(bytes, doodads.OriginalBytes().capacity());
    return bytes;
}

} // namespace

namespace {
void VerifyObjectMemberBytes(const ClassicMinimapObjectAssets& snapshot, const MapArchiveSession& source,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    bool readback, const ClassicMinimapObjectAssetLimits& limits) {
    ArchiveExactMemberLimits exact;
    exact.memberCount = 2; exact.memberBytes = limits.maxObjectMemberBytes; exact.totalBytes = limits.maxObjectMemberBytes * 2;
    const auto members = Take(source.CaptureExactMembers(kMembers, exact));
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& expected = snapshot.CapturedMembers()[i];
        const auto normalize = [](std::string_view name) { std::string result(name);
            for (auto& c : result) { if (c == '/') c = '\\'; else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return result; };
        const auto key = normalize(kMembers[i]);
        const MemberReplacement* replacement = nullptr; bool removed = false;
        for (const auto& entry : replacements) if (normalize(entry.name) == key) {
            if (replacement) Fail("CLASSIC_OBJECT_CAPTURE_FINAL_DUPLICATE", "最终对象成员有重复替换。");
            replacement = &entry;
        }
        for (const auto& entry : removals) if (normalize(entry) == key) {
            if (removed || replacement) Fail("CLASSIC_OBJECT_CAPTURE_FINAL_CONFLICT", "最终对象成员重复删除或同时被替换和删除。");
            removed = true;
        }
        const bool present = replacement || (!removed && members[i].present);
        const auto& bytes = replacement ? replacement->data : members[i].bytes;
        if (present != expected.currentPresent || (present && (bytes.size() != expected.currentBytes
            || Take(Sha256(bytes)) != expected.currentSha256)))
            Fail("CLASSIC_OBJECT_CAPTURE_FINAL_BYTES", "最终对象成员与小地图捕获的当前摆放不同。");
        if (!readback && (members[i].present != expected.sourcePresent || (members[i].present
            && (members[i].bytes.size() != expected.sourceBytes || Take(Sha256(members[i].bytes)) != expected.sourceSha256))))
            Fail("CLASSIC_OBJECT_CAPTURE_SOURCE_STALE", "原始对象成员来源已改变。");
    }
}
}

Status VerifyClassicMinimapObjectAssetsCurrent(const ClassicMinimapObjectAssets& snapshot,
    const MapArchiveSession& source, const MapData::MapDataEditSession& edits, const std::filesystem::path& gameRoot,
    const TerrainMinimapAssetCaptureLimits& terrainLimits, const ClassicMinimapObjectAssetLimits& objectLimits,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) {
    try {
        ValidateLimits(objectLimits);
        Check(VerifyTerrainMinimapAssetsCurrent(snapshot.TerrainAssets(), source, edits, gameRoot, terrainLimits));
        for (std::size_t i = 0; i < 2; ++i) {
            const auto current = Take(edits.Snapshot(kKinds[i], objectLimits.maxCurrentSnapshotBytes));
            const auto& member = current.Member(kKinds[i]); const auto& expected = snapshot.CapturedMembers()[i];
            if ((member.state != MapDataState::Missing) != expected.currentPresent)
                Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_MISMATCH", "当前对象层存在性与捕获不同。");
            if (expected.currentPresent && (member.state != MapDataState::Ready || member.sources.size() != 1
                || member.sources.front().data.size() != expected.currentBytes
                || Take(Sha256(member.sources.front().data)) != expected.currentSha256))
                Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_MISMATCH", "当前对象层正文与捕获不同。");
        }
        Check(VerifyClassicMinimapObjectLayerDependencies(snapshot.ObjectLayer(), source, gameRoot,
            snapshot.TerrainAssets().Binding().mainTileset, replacements, removals));
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
}

Status VerifyClassicMinimapObjectAssetsFrozenOutput(const ClassicMinimapObjectAssets& snapshot,
    const MapArchiveSession& source, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals, const TerrainMinimapAssetCaptureLimits& terrainLimits,
    const ClassicMinimapObjectAssetLimits& objectLimits) {
    try {
        ValidateLimits(objectLimits);
        Check(VerifyTerrainMinimapAssetsFrozenOutput(snapshot.TerrainAssets(), source, replacements, removals, terrainLimits));
        VerifyObjectMemberBytes(snapshot, source, replacements, removals, false, objectLimits);
        Check(VerifyClassicMinimapObjectLayerDependencies(snapshot.ObjectLayer(), source,
            snapshot.TerrainAssets().Binding().gameRoot, snapshot.TerrainAssets().Binding().mainTileset, replacements, removals));
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
}

Status VerifyClassicMinimapObjectAssetsReadback(const ClassicMinimapObjectAssets& snapshot,
    const MapArchiveSession& saved, const TerrainMinimapAssetCaptureLimits& terrainLimits,
    const ClassicMinimapObjectAssetLimits& objectLimits) {
    try {
        ValidateLimits(objectLimits);
        Check(VerifyTerrainMinimapAssetsReadback(snapshot.TerrainAssets(), saved, terrainLimits));
        VerifyObjectMemberBytes(snapshot, saved, {}, {}, true, objectLimits);
        Check(VerifyClassicMinimapObjectLayerDependencies(snapshot.ObjectLayer(), saved,
            snapshot.TerrainAssets().Binding().gameRoot, snapshot.TerrainAssets().Binding().mainTileset, {}, {}, true));
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
}

Result<ClassicMinimapObjectAssets> CaptureClassicMinimapObjectAssets(TerrainMinimapAssetSnapshot&& terrain,
    const MapArchiveSession& source, const MapData::MapDataEditSession& edits,
    const ClassicMinimapObjectAssetLimits& limits, std::span<const MemberReplacement> replacements,
    std::span<const std::string> removals) {
    try {
        ValidateLimits(limits);
        // FullW3e只供离线候选；正式保存由root的ClassicPlayable策略核W3I/camera来源。
        // 对象层本身在完整地图坐标下绘制，候选组合时再裁进实际输出rect。
        // 先预收，再进行任何新的IO/hash/parse。已有输入内部provider/MPQ缓冲不在载荷口径。
        ClassicMinimapObjectAssetFacts facts;
        facts.retainedTerrainCapacityBytes = TerrainCapacity(terrain);
        auto extra = kFixedScratch;
        Charge(extra, kMetadataReserve);
        Charge(extra, limits.maxCurrentTerrainBytes, 2); // Serialize copy + SHA正文chunk
        Charge(extra, limits.maxObjectMemberBytes, 6); // exact双成员、parser原字/散列及结果保活
        Charge(extra, limits.maxCurrentSnapshotBytes, ClassicMinimapObjectAssetFacts::snapshotScratchBytesPerWireByte + 4);
        facts.estimatedExtraPeakBytes = extra; facts.estimatedPeakBytes = facts.retainedTerrainCapacityBytes;
        Charge(facts.estimatedPeakBytes, extra);
        if (facts.estimatedPeakBytes > limits.maxWorkingBytes)
            Fail("CLASSIC_OBJECT_CAPTURE_BUDGET", "既有Terrain与对象捕获/当前快照解析峰值超过累计预算。");
        if (!MatchesTerrainCaptureBinding(terrain, source, edits))
            Fail("CLASSIC_OBJECT_CAPTURE_STALE", "Terrain快照不属于当前archive或编辑代际。");
        const auto currentTerrain = edits.Terrain();
        if (!currentTerrain || terrain.Input().currentW3e.size() > limits.maxCurrentTerrainBytes)
            Fail("CLASSIC_OBJECT_CAPTURE_TERRAIN", "当前W3E缺失或超过对象捕获序列化预算。");
        std::size_t serializedBytes = 37;
        Charge(serializedBytes, currentTerrain->header.groundTilesets.size(), 4);
        Charge(serializedBytes, currentTerrain->header.cliffTilesets.size(), 4);
        Charge(serializedBytes, currentTerrain->vertices.size(), 7);
        if (currentTerrain->header.version != 11 || serializedBytes > limits.maxCurrentTerrainBytes)
            Fail("CLASSIC_OBJECT_CAPTURE_TERRAIN", "当前Terrain不是有界classic v11序列化域。");
        {
            const auto currentBytes = Take(MapData::SerializeMapTerrain(*currentTerrain));
            if (currentBytes != terrain.Input().currentW3e
                || Take(Sha256(currentBytes)) != terrain.Binding().currentTerrainSha256)
                Fail("CLASSIC_OBJECT_CAPTURE_STALE", "真实当前W3E与传入Terrain快照不同。");
        }
        ArchiveExactMemberLimits exact;
        exact.memberCount = 2; exact.memberBytes = limits.maxObjectMemberBytes;
        exact.totalBytes = limits.maxObjectMemberBytes * 2;
        auto members = Take(source.CaptureExactMembers(kMembers, exact));
        if (members.size() != 2 || members[0].name != kMembers[0] || members[1].name != kMembers[1])
            Fail("CLASSIC_OBJECT_CAPTURE_SOURCE", "准确捕获没有返回要求的两个对象成员身份。");
        MapData::UnitPlacements units;
        MapData::DoodadPlacements doodads;
        std::size_t measuredSnapshotBytes = 0;
        for (const auto kind : kKinds) Charge(measuredSnapshotBytes, Take(edits.SnapshotByteSize(kind)));
        for (std::size_t i = 0; i < 2; ++i) {
            const auto current = Take(edits.Snapshot(kKinds[i], limits.maxCurrentSnapshotBytes));
            const auto& member = current.Member(kKinds[i]);
            ArchiveExactMember currentMember{kMembers[i], member.state != MapDataState::Missing, {}};
            if (currentMember.present) {
                if (member.state != MapDataState::Ready || member.sources.size() != 1 || member.sources.front().name != kMembers[i])
                    Fail("CLASSIC_OBJECT_CAPTURE_CURRENT_LAYOUT", "当前对象层必须是唯一且可完整解析的Ready成员。");
                currentMember.bytes = member.sources.front().data;
            }
            MapData::UnitPlacements sourceUnits; MapData::DoodadPlacements sourceDoodads;
            ClassicMinimapObjectMemberFacts sourceFacts, currentFacts;
            ParseObjectMember(i, members[i], sourceFacts, limits, sourceUnits, sourceDoodads);
            ParseObjectMember(i, currentMember, currentFacts, limits, units, doodads);
            auto& fact = facts.members[i] = std::move(currentFacts);
            fact.memberName = kMembers[i]; fact.sourcePresent = members[i].present; fact.currentPresent = currentMember.present;
            fact.sourceBytes = members[i].bytes.size(); fact.currentBytes = currentMember.bytes.size();
            fact.sourceSha256 = fact.sourcePresent ? Take(Sha256(members[i].bytes)) : std::string{};
            fact.currentSha256 = fact.currentPresent ? Take(Sha256(currentMember.bytes)) : std::string{};
            fact.sourceAndCurrentBytesEqual = fact.sourcePresent == fact.currentPresent && members[i].bytes == currentMember.bytes;
            // 源与当前可以合法不同：编辑会话可能已新增、删除或修改对象。两份正文
            // 分别保留并由 Current/FrozenOutput 的 revision + replacement 门核对；
            // 这里只拒绝任一侧无法由唯一classic parser完整读取的记录。
        }
        if (!doodads.specialDoodads.empty())
            Fail("CLASSIC_OBJECT_CAPTURE_SPECIAL_UNRESOLVED", "当前war3map.doo包含特殊装饰物记录；其是否进入原版0x08小地图层尚未由IDA确认，拒绝生成以免静默漏画。");
        auto objectWorking = facts.estimatedPeakBytes;
        Charge(objectWorking, measuredSnapshotBytes);
        Charge(objectWorking, members[0].bytes.capacity()); Charge(objectWorking, members[1].bytes.capacity());
        Charge(objectWorking, currentTerrain->vertices.capacity(), sizeof(MapData::TerrainVertex));
        Charge(objectWorking, units.units.capacity(), sizeof(MapData::UnitPlacement));
        Charge(objectWorking, doodads.doodads.capacity(), sizeof(MapData::DoodadPlacement));
        Charge(objectWorking, doodads.specialDoodads.capacity(), sizeof(MapData::SpecialDoodadPlacement));
        if (objectWorking > limits.maxWorkingBytes)
            Fail("CLASSIC_OBJECT_CAPTURE_BUDGET", "当前对象摆放解析峰值超过累计捕获预算。");
        auto objectLayer = Take(CaptureClassicMinimapObjectLayer(doodads.doodads, source, terrain.Binding().gameRoot,
            currentTerrain->header.mainTileset, currentTerrain->header.offset,
            {0, 0, currentTerrain->header.width - 1, currentTerrain->header.height - 1}, replacements, removals));
        Check(source.VerifySourceUnchanged());
        if (!MatchesTerrainCaptureBinding(terrain, source, edits) || edits.Terrain() != currentTerrain)
            Fail("CLASSIC_OBJECT_CAPTURE_STALE", "对象捕获期间来源、会话或当前Terrain已变化。");
        ClassicMinimapObjectDomain domain;
        domain.sourceSha256 = terrain.Binding().archiveSha256;
        domain.documentId = terrain.Input().binding.documentId;
        domain.documentGeneration = terrain.Binding().sessionIdentity;
        domain.revision = terrain.Binding().revision; domain.terrainSourceRevision = terrain.Binding().terrainSourceRevision;
        const auto toDomainState = [](ClassicMinimapCapturedObjectState state) {
            switch (state) {
            case ClassicMinimapCapturedObjectState::Missing: return ClassicMinimapMemberState::Missing;
            case ClassicMinimapCapturedObjectState::ParsedEmpty: return ClassicMinimapMemberState::ParsedEmpty;
            case ClassicMinimapCapturedObjectState::Parsed: return ClassicMinimapMemberState::Parsed;
            }
            return ClassicMinimapMemberState::Unknown;
        };
        domain.unitsState = toDomainState(facts.members[0].state);
        domain.doodadsState = toDomainState(facts.members[1].state);
        domain.unitsMemberBytes = facts.members[0].currentBytes; domain.doodadsMemberBytes = facts.members[1].currentBytes;
        domain.unitsMemberSha256 = facts.members[0].currentSha256; domain.doodadsMemberSha256 = facts.members[1].currentSha256;
        Identity identity;
        identity.Text(ClassicMinimapObjectAssetFacts::capturePolicy); identity.Text(terrain.IdentitySha256());
        identity.Text(domain.sourceSha256); identity.Text(domain.documentId); identity.U64(domain.documentGeneration);
        identity.U64(domain.revision); identity.U64(domain.terrainSourceRevision);
        for (const auto& member : facts.members) {
            identity.Text(member.memberName); identity.U64(static_cast<std::uint64_t>(member.state));
            identity.U64(member.sourceBytes); identity.Text(member.sourceSha256);
            identity.U64(member.currentBytes); identity.Text(member.currentSha256);
            identity.U64(member.sourcePresent); identity.U64(member.currentPresent);
            identity.U64(static_cast<std::uint32_t>(member.version)); identity.U64(static_cast<std::uint32_t>(member.subversion));
            identity.U64(static_cast<std::uint32_t>(member.recordLayout)); identity.U64(static_cast<std::uint32_t>(member.specialVersion));
            identity.U64(member.ordinaryPlacements); identity.U64(member.specialPlacements);
            identity.U64(member.parserFullyConsumed ? 1u : 0u);
        }
        for (const auto& dependency : objectLayer.dependencies) {
            identity.Text(dependency.winner.canonicalPath); identity.Text(dependency.encodedSha256);
            identity.U64(dependency.present); identity.U64(dependency.mapMemberPresent);
        }
        auto identitySha = identity.Finish();
        std::array<std::vector<std::byte>, 2> owned{std::move(members[0].bytes), std::move(members[1].bytes)};
        facts.retainedAuxiliaryCapacityBytes = AuxiliaryCapacity(domain, facts, owned, units, doodads, objectLayer, identitySha);
        if (facts.retainedAuxiliaryCapacityBytes > kMetadataReserve + 6 * limits.maxObjectMemberBytes
            + limits.maxPlacements * (sizeof(MapData::UnitPlacement) + sizeof(MapData::DoodadPlacement)))
            Fail("CLASSIC_OBJECT_CAPTURE_BUDGET", "对象见证实际capacity超过预收元数据与正文。");
        // 至此所有失败门已通过才move既有Terrain owner；失败不损耗调用者的原快照。
        ClassicMinimapObjectAssets output(std::move(terrain));
        output.domain_ = std::move(domain); output.facts_ = std::move(facts);
        output.sourceMembers_ = std::move(owned); output.identitySha256_ = std::move(identitySha);
        output.units_ = std::move(units); output.doodads_ = std::move(doodads);
        output.objectLayer_ = std::move(objectLayer);
        return Result<ClassicMinimapObjectAssets>::Success(std::move(output));
    } catch (Failure& failure) {
        return {std::nullopt, std::move(failure.diagnostics)};
    } catch (const std::bad_alloc&) {
        return Result<ClassicMinimapObjectAssets>::Failure(kSubsystem, "CLASSIC_OBJECT_CAPTURE_ALLOCATION", "对象捕获分配失败。");
    } catch (const std::length_error&) {
        return Result<ClassicMinimapObjectAssets>::Failure(kSubsystem, "CLASSIC_OBJECT_CAPTURE_ALLOCATION", "对象捕获容器容量异常。");
    }
}

} // namespace War3Map::Render
