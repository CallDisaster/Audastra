#include "War3MapCore/Services/TerrainShadowReceiverResources.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/TerrainNativePathing.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/MapData/TerrainSpecialDoodadPathing.h"
#include "War3MapCore/ObjectData/NativeTerrainDependencies.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Render/MdxTerrainReceiver.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Resources/TerrainTable.h"
#include "War3MapCore/Text/TextUtil.h"
#include "../Render/TerrainShadowNativeRay.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
constexpr const char* kSubsystem = "Terrain.ShadowReceivers";
constexpr std::size_t kDependencyMetadataBytes = 1024 + 64 + 256;
using Vec3 = std::array<float, 3>;
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Check(Status result) { if (!result.Succeeded()) throw Failure{std::move(result.diagnostics)}; }
void Charge(std::size_t& used, std::size_t count, std::size_t maximum, std::size_t multiple = 1) {
    if (!multiple || used > maximum || count > (maximum - used) / multiple)
        Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "悬崖接收面的累计读取、工作或内存超过预算。");
    used += count * multiple;
}
void Cancel(std::stop_token cancellation) {
    if (cancellation.stop_requested())
        Fail("TERRAIN_SHADOW_RECEIVER_CANCELLED", "已取消接收面计算，未返回部分高度场。");
}
std::string Canonical(std::string_view name) { return Take(Resources::CanonicalImportPath(name)); }
std::array<std::size_t, 4> Corners(const MapData::TerrainGridSnapshot& terrain, std::uint32_t x, std::uint32_t y) {
    const auto sw = static_cast<std::size_t>(y) * terrain.header.width + x;
    return {sw, sw + 1, sw + terrain.header.width, sw + terrain.header.width + 1};
}
float RawHeight(const MapData::TerrainVertex& vertex) {
    return static_cast<float>((static_cast<double>(vertex.groundHeightRaw) - 8192.0) / 4.0);
}
float GroundHeight(const MapData::TerrainVertex& vertex) {
    return static_cast<float>(static_cast<double>(RawHeight(vertex)) + 128.0 * (vertex.CliffLevel() - 2));
}

// 5A85D0只用首四字符建立anchor；5A8650还接受alternate，但不另建record。
// 后四数分别是5A8A30的平移格偏移和5A8810的有符号record末端偏移。
struct RampPattern { std::string_view anchor, alternate; int shiftX, shiftY, endX, endY; };
constexpr std::array<RampPattern, 16> kRampPatterns{{
    {"HRAA","RLAA",0,0,1,0}, {"RHAA","LRAA",-1,0,-1,0},
    {"AARH","AALR",0,0,1,0}, {"AAHR","AARL",-1,0,-1,0},
    {"AHRA","ARLA",0,-1,0,-1}, {"ARHA","ALRA",0,0,0,1},
    {"HAAR","RAAL",0,-1,0,-1}, {"RAAH","LAAR",0,0,0,1},
    {"HRAB","RLAA",0,0,1,0}, {"RHBA","LRAA",-1,0,-1,0},
    {"BARH","AALR",0,0,1,0}, {"ABHR","AARL",-1,0,-1,0},
    {"BHRA","ARLA",0,-1,0,-1}, {"ARHB","ALRA",0,0,0,1},
    {"HBAR","RAAL",0,-1,0,-1}, {"RABH","LAAR",0,0,0,1}
}};
const RampPattern* MatchRamp(const std::array<char, 4>& corners, bool alternate) {
    const std::string_view key(corners.data(), corners.size());
    const auto found = std::find_if(kRampPatterns.begin(), kRampPatterns.end(), [&](const auto& pattern) {
        return key == pattern.anchor || (alternate && key == pattern.alternate);
    });
    return found == kRampPatterns.end() ? nullptr : &*found;
}

// 与caster共用MapArchiveSession、GameDataSource及只读来源租约。这里只捕获
// receiver额外需要的CliffTypes/模型；DOO和W3E始终借用已封存的caster owner。
class Inputs {
public:
    Inputs(const MapArchiveSession& source, const std::filesystem::path& gameRoot, std::uint8_t tileset,
        std::span<const MemberReplacement> output, std::span<const std::string> removed,
        const TerrainShadowReceiverLimits& limits, std::stop_token cancellation)
        : source_(source), gameRoot_(gameRoot), tileset_(tileset), limits_(limits), cancellation_(cancellation) {
        if (output.size() > 16384 || removed.size() > 16384)
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "最终资源视图的成员数超过预算。");
        std::set<std::string, std::less<>> seen;
        for (const auto& item : output) {
            if (item.name.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "最终资源成员名超过路径预算。");
            Charge(working, item.name.size() + 128, limits_.maximumWorkingBytes, 4);
            const auto key = Resources::CanonicalImportPath(item.name);
            if (!key.Succeeded() || !key.value) continue;
            if (!seen.insert(*key.value).second)
                Fail("TERRAIN_SHADOW_RECEIVER_FINAL_CONFLICT", "最终资源视图含重复替换成员。");
            replacements_.emplace(*key.value, &item);
        }
        for (const auto& item : removed) {
            if (item.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "最终删除成员名超过路径预算。");
            Charge(working, item.size() + 128, limits_.maximumWorkingBytes, 4);
            const auto key = Resources::CanonicalImportPath(item);
            if (!key.Succeeded() || !key.value) continue;
            if (!seen.insert(*key.value).second)
                Fail("TERRAIN_SHADOW_RECEIVER_FINAL_CONFLICT", "最终资源同时替换/删除或重复删除。");
            removals_.insert(*key.value);
        }
        Charge(working, (source.SourcePath().native().size() + 1) * sizeof(std::filesystem::path::value_type) + 512,
            limits_.maximumWorkingBytes, 4);
        Check(guard_.Watch(source.SourcePath()));
    }
    const ArchiveExactMember& Read(std::string_view name) {
        Cancel(cancellation_);
        if (name.size() > ArchiveExactMemberLimits{}.pathBytes)
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "接收面路径超过预算。");
        auto pathWorking = working;
        Charge(pathWorking, name.size() + 128, limits_.maximumWorkingBytes, 4);
        const auto key = Canonical(name);
        if (const auto found = files_.find(key); found != files_.end()) return found->second;
        if (files_.size() >= 32768)
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "接收面的来源记录数超过硬上限。");
        Charge(working, key.size() + kDependencyMetadataBytes, limits_.maximumWorkingBytes, 4);
        const auto available = std::min({limits_.maximumFileBytes, limits_.maximumSourceBytes - sourceBytes,
            (limits_.maximumWorkingBytes - working) / 2});
        ArchiveExactMember file{std::string(name), false, {}};
        if (const auto replacement = replacements_.find(key); replacement != replacements_.end()) {
            if (replacement->second->data.size() > available)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "最终接收面成员超过读取预算：" + std::string(name));
            file.present = true; file.bytes = replacement->second->data;
        } else if (!removals_.contains(key)) {
            ArchiveExactMemberLimits exact; exact.memberCount = 1; exact.memberBytes = exact.totalBytes = available;
            const std::array<std::string, 1> names{std::string(name)};
            file = std::move(Take(source_.CaptureExactMembers(names, exact)).front());
        }
        TerrainShadowCasterDependency fact;
        fact.name = key; fact.source = file.present ? TerrainShadowCasterSource::Map : TerrainShadowCasterSource::Missing;
        if (!file.present && !gameRoot_.empty()) {
            if (!game_) {
                Check(guard_.WatchGameRoot(gameRoot_));
                game_ = Take(Game::GameDataSource::MountStrict(gameRoot_, tileset_));
            }
            if (game_->HasFile(std::string(name))) {
                auto gameFile = Take(game_->ReadFileBoundedWithSource(std::string(name), available));
                file.present = true; file.bytes = std::move(gameFile.bytes);
                fact.source = TerrainShadowCasterSource::Game; fact.gameContainer = std::move(gameFile.archiveName);
            }
        }
        Charge(sourceBytes, file.bytes.size(), limits_.maximumSourceBytes);
        Charge(working, file.bytes.size(), limits_.maximumWorkingBytes, 2);
        if (fact.gameContainer.size() > 1024)
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "接收面实际容器身份超过有界支持范围。");
        fact.bytes = file.bytes.size();
        if (file.present) fact.sha256 = Take(Sha256(file.bytes));
        facts.emplace(key, std::move(fact));
        return files_.emplace(key, std::move(file)).first->second;
    }
    void Release(std::string_view name) {
        const auto found = files_.find(name);
        if (found == files_.end()) return;
        working -= found->second.bytes.size() * 2 + (found->first.size() + kDependencyMetadataBytes) * 4;
        facts.erase(facts.find(name)); files_.erase(found);
    }
    void Verify() { Check(guard_.Verify()); Check(source_.VerifySourceUnchanged()); }
    void ReserveWorking(std::size_t count, std::size_t multiple = 1) {
        Charge(working, count, limits_.maximumWorkingBytes, multiple);
    }
    std::size_t AvailableWorkingBytes() const { return limits_.maximumWorkingBytes - working; }
    std::size_t sourceBytes = 0, working = 0;
    std::map<std::string, TerrainShadowCasterDependency, std::less<>> facts;
private:
    const MapArchiveSession& source_;
    const std::filesystem::path& gameRoot_;
    std::uint8_t tileset_;
    const TerrainShadowReceiverLimits& limits_;
    std::stop_token cancellation_;
    Render::AnimationSourceGuard guard_;
    std::optional<Game::GameDataSource> game_;
    std::map<std::string, const MemberReplacement*, std::less<>> replacements_;
    std::set<std::string, std::less<>> removals_;
    std::map<std::string, ArchiveExactMember, std::less<>> files_;
};

std::vector<Resources::TerrainTableRow> TableRows(Inputs& inputs, std::string_view path, std::string_view id) {
    Resources::ResourceResolver resolver;
    Resources::ResourceSource source;
    source.id = "shadow-receiver-final-view"; source.layer = Resources::ResourceLayer::Other; source.priority = 1;
    source.probe = [&](std::string_view name) {
        const auto& file = inputs.Read(name);
        return Result<Resources::ResourceProbe>::Success({file.present ? Resources::ResourceProbeState::Found
            : Resources::ResourceProbeState::Missing, std::string(name)});
    };
    source.read = [&](std::string_view name) {
        const auto& bytes = inputs.Read(name).bytes;
        if (bytes.size() > Resources::TerrainTableLimits{}.maxTableBytes)
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "悬崖SLK表超过共用表读取器硬上限。");
        // 共用SLK读取器每条物理行至多保留一个cell。只计数行、最长行和
        // 分隔符，不在这里解释SLK；分别覆盖cells树、表头/ID树、最终行及
        // Rows()副本、最长字段字符串和SplitFields的几何增长临时vector。
        // 这是按容器/文本上界的预收，不再用文件字节乘经验倍数估算解析。
        std::size_t lines = 1, lineBytes = 0, longest = 0, fields = 1, maximumFields = 1;
        for (const auto byte : bytes) {
            if (byte == std::byte{'\n'}) {
                ++lines; longest = std::max(longest, lineBytes); maximumFields = std::max(maximumFields, fields);
                lineBytes = 0; fields = 1;
            } else { ++lineBytes; if (byte == std::byte{';'}) ++fields; }
        }
        longest = std::max(longest, lineBytes); maximumFields = std::max(maximumFields, fields);
        constexpr auto nodeBytes = sizeof(std::pair<const int, std::string>) + 4 * sizeof(void*);
        inputs.ReserveWorking(lines, 4 * sizeof(Resources::TerrainTableRow) + 8 * nodeBytes);
        inputs.ReserveWorking(lines, 12 * longest);
        inputs.ReserveWorking(maximumFields, 4 * sizeof(std::string_view));
        inputs.ReserveWorking(bytes.size(), 4);
        return Result<std::vector<std::byte>>::Success(bytes);
    };
    Check(resolver.AddSource(std::move(source)));
    auto table = Take(Resources::CaptureTerrainTable(resolver, path, id));
    return table.Rows();
}

struct SpecialGeometry {
    std::uint32_t x = 0, y = 0, width = 0, height = 0; // 实际pathTex像素，不是显示模型包围盒
    std::uint8_t minimum = 0;
    std::string model;
};
std::vector<SpecialGeometry> SpecialGeometries(Inputs& inputs, const MapData::TerrainGridSnapshot& terrain,
    std::span<const MapData::SpecialDoodadPlacement> placements, std::uint8_t maximumLevel,
    std::size_t& work, const TerrainShadowReceiverLimits& limits, std::stop_token cancellation) {
    const auto tick = [&](std::size_t count) { Cancel(cancellation); Charge(work, count, limits.maximumWork); };
    // 共用物编依赖描述读取实际pathTex；不在阴影服务另造SLK继承或元数据规则。
    constexpr std::array<std::string_view, 6> names{"war3map.w3b", "war3map.w3d", "Units/DestructableData.slk",
        "Doodads/Doodads.slk", "Units/DestructableMetaData.slk", "Doodads/DoodadMetaData.slk"};
    std::array<ArchiveExactMember, names.size()> sources;
    inputs.ReserveWorking(sources.size(), sizeof(ArchiveExactMember) + 260);
    constexpr std::array<std::byte, 12> emptyNative{std::byte{2}};
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& source = inputs.Read(names[i]);
        if (i < 2 && source.present && (source.bytes.size() != emptyNative.size()
            || !std::equal(source.bytes.begin(), source.bytes.end(), emptyNative.begin())))
            Fail("TERRAIN_SHADOW_SPECIAL_NATIVE_UNSUPPORTED", "特殊阴影接收面尚不接受非空或未知格式的原生物编覆盖。");
        if ((i == 2 || i == 3) && !source.present)
            Fail("TERRAIN_SHADOW_SPECIAL_GEOMETRY", "特殊阴影接收面缺少实际对象表，未猜测模型或占地。");
        inputs.ReserveWorking(source.bytes.size()); tick(source.bytes.size()); sources[i] = source;
    }
    inputs.ReserveWorking(placements.size(), sizeof(std::string_view) + sizeof(SpecialGeometry) + 260);
    std::vector<std::string_view> rawcodes; rawcodes.reserve(placements.size());
    for (const auto& placement : placements) {
        tick(1);
        if (placement.variation != 0)
            Fail("TERRAIN_SHADOW_SPECIAL_VARIATION", "特殊阴影接收面仅接受已闭合的零变体记录。");
        rawcodes.push_back(placement.rawcode);
    }
    // 排序和索引比较逐次收费，不能让大批重复rawcode或长路径绕过work/取消门。
    auto less = [&](std::string_view a, std::string_view b) {
        tick(std::min(a.size(), b.size()) + 1); return a < b;
    };
    std::sort(rawcodes.begin(), rawcodes.end(), less);
    rawcodes.erase(std::unique(rawcodes.begin(), rawcodes.end(), [&](std::string_view a, std::string_view b) {
        tick(std::min(a.size(), b.size()) + 1); return a == b;
    }), rawcodes.end());
    const auto described = Take(ObjectData::InspectNativeTerrainDependencies({}, rawcodes, sources,
        std::min<std::size_t>(inputs.AvailableWorkingBytes(), 64u * 1024u * 1024u)));
    inputs.ReserveWorking(described.workBytes);
    const auto rows = TableRows(inputs, "Doodads/Doodads.slk", "doodid");
    inputs.ReserveWorking(rows.size(), sizeof(void*) * 8);
    std::map<std::string_view, const Resources::TerrainTableRow*, decltype(less)> byId(less);
    for (const auto& row : rows) { tick(1); byId.emplace(row.id, &row); }
    std::map<std::string, MapData::TerrainDoodadFootprint, decltype(less)> images(less);
    std::vector<SpecialGeometry> result; result.reserve(placements.size());
    for (const auto& placement : placements) {
        tick(described.objects.size());
        const auto type = std::find_if(described.objects.begin(), described.objects.end(), [&](const auto& value) {
            return value.special && value.rawcode == placement.rawcode;
        });
        const auto row = byId.find(placement.rawcode);
        if (type == described.objects.end() || row == byId.end() || !type->parentRawcode.empty()
            || !type->unknownFieldsSha256.empty() || type->pathTexture.empty())
            Fail("TERRAIN_SHADOW_SPECIAL_OBJECT", "特殊阴影接收面需要唯一标准对象及明确启用的实际pathTex。");
        // 本批只接受原首选TGA声明；不将BLP、坏TGA或未知后缀改成一份猜测图像。
        if (type->pathTexture.size() >= 260)
            Fail("TERRAIN_SHADOW_SPECIAL_PATH", "特殊占地路径超出原版有界容量。");
        const auto path = Canonical(type->pathTexture);
        if (!path.ends_with(".TGA"))
            Fail("TERRAIN_SHADOW_SPECIAL_PATH", "特殊阴影接收面仅接受明确的原首选TGA路径。");
        auto image = images.find(path);
        if (image == images.end()) {
            const auto& file = inputs.Read(path);
            if (!file.present) Fail("TERRAIN_SHADOW_SPECIAL_FOOTPRINT_MISSING", "缺少特殊接收面的实际TGA占地。");
            tick(file.bytes.size());
            // 特殊地形沿用type2零拷贝入口，不借普通对象的RLE/BLP扩展域。
            auto decoded = Take(MapData::DecodeDoodadFootprintTga(file.bytes));
            inputs.ReserveWorking(path.size() + sizeof(MapData::TerrainDoodadFootprint) + 8 * sizeof(void*));
            image = images.emplace(path, decoded).first;
        }
        const auto& footprint = image->second;
        tick((std::size_t(footprint.width) / 4 + 1) * (std::size_t(footprint.height) / 4 + 1) * 3);
        const std::array<MapData::TerrainSpecialDoodadPathingInput, 1> shapeInputs{{{&placement, footprint}}};
        const auto shapes = Take(MapData::DescribeTerrainSpecialDoodads(terrain, shapeInputs, maximumLevel,
            inputs.AvailableWorkingBytes()));
        const auto& shape = shapes.front();
        inputs.ReserveWorking(sizeof(MapData::TerrainWaterSpecialDoodadShape) + shape.rawcode.capacity()
            + shape.relativeCliffLevels.capacity() + 32);
        const auto field = [&](std::string_view key) -> std::string_view {
            const auto found = row->second->fields.find(key);
            if (found == row->second->fields.end())
                Fail("TERRAIN_SHADOW_SPECIAL_FIELD", "特殊模型缺少实际file/numVar字段。");
            return found->second;
        };
        const auto file = field("file"), number = field("numvar");
        unsigned variations = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), variations);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || variations < 1 || variations > 10
            || file.empty() || file == "_" || file.size() >= 256 || file.find('.') != std::string_view::npos)
            Fail("TERRAIN_SHADOW_SPECIAL_MODEL_PATH", "特殊模型仅接受实际无扩展名file、numVar1..10的已闭合域。");
        std::string model(file); if (variations > 1) model.push_back('0');
        const auto base = Canonical(model);
        // 5F29B0的末端是最后地格，Describe的x1/y1则是末端顶点。
        std::uint8_t minimum = maximumLevel;
        for (auto y = shape.y0; y <= shape.y1; ++y) for (auto x = shape.x0; x <= shape.x1; ++x)
            minimum = std::min(minimum, terrain.vertices[std::size_t(y) * terrain.header.width + x].CliffLevel());
        result.push_back({shape.x0, shape.y0, footprint.width, footprint.height, minimum, base + ".MDX"});
    }
    return result;
}

void ValidateModel(std::span<const std::byte> bytes, Inputs& inputs) {
    // 完整静态语法与原序几何分别由共享核心验证。语法解析在分配前精确预收
    // 有效存储，不以源文件大小乘经验系数替代预算，也不借Stand结果筛接收面。
    auto checked = Render::ValidateMdxTerrainReceiverStaticBudgeted(bytes,
        std::min<std::size_t>(inputs.AvailableWorkingBytes(), 128u * 1024u * 1024u));
    if (!checked.Succeeded()) {
        if (std::any_of(checked.diagnostics.begin(), checked.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "MDX_STATIC_PARSE_UNQUALIFIED";
        })) Fail("TERRAIN_SHADOW_RECEIVER_CONTROLLER", "悬崖模型超出已解释的静态语法域，原阴影保留。");
        throw Failure{std::move(checked.diagnostics)};
    }
}

std::pair<std::uint8_t, std::uint8_t> CliffSlot(const MapData::TerrainGridSnapshot& terrain,
    std::uint32_t x, std::uint32_t y) {
    const auto selected = [&](std::size_t i) { return std::pair{terrain.vertices[i].CliffTextureIndex(), terrain.vertices[i].CliffVariation()}; };
    for (const auto i : Corners(terrain, x, y)) if (terrain.vertices[i].CliffTextureIndex() != 15) return selected(i);
    const auto minX = static_cast<std::int32_t>(x) - 2, minY = static_cast<std::int32_t>(y) - 2;
    for (auto nx = minX; nx <= static_cast<std::int32_t>(x) + 2; ++nx)
        for (auto ny = minY; ny <= static_cast<std::int32_t>(y) + 2; ++ny) {
            if (nx < 0 || ny < 0 || nx >= static_cast<std::int32_t>(terrain.header.width)
                || ny >= static_cast<std::int32_t>(terrain.header.height)) continue;
            const auto index = static_cast<std::size_t>(ny) * terrain.header.width + nx;
            if (terrain.vertices[index].CliffTextureIndex() != 15) return selected(index);
        }
    return {0, 0};
}
} // namespace

struct TerrainShadowReceiverResources::State {
    struct Model { std::string path; Render::MdxTerrainReceiverSource source; };
    struct Record {
        std::uint32_t x = 0, y = 0;
        std::int32_t endX = 0, endY = 0;
        std::size_t model = 0;
        std::vector<std::vector<Vec3>> positions; // GEOS和VRTX顺序，group索引借用Model
    };
    struct RampVertex { char corner = 'A'; float heightOffset = 0; };
    std::shared_ptr<const TerrainShadowCasterResources> casters;
    std::string sourceSha, lightFingerprint;
    std::filesystem::path gameRoot;
    TerrainShadowReceiverLimits limits;
    std::vector<TerrainShadowCasterDependency> dependencies;
    std::vector<Model> models;
    std::vector<Record> records;
    std::vector<std::vector<std::size_t>> blocks;
    std::vector<RampVertex> rampVertices;
    std::vector<std::uint8_t> modelCells;
    // 零表示普通格；其余是首覆盖special的锚点地格线性下标+1。
    // 57F790先映射锚点block，再按全部原序records选首包含项，不能直取special模型。
    std::vector<std::uint32_t> specialAnchors;
    std::size_t sourceBytes = 0, working = 0, work = 0;
    bool ordinary = true;

    TerrainShadowCasterVerificationLimits VerificationBudget(const TerrainShadowCasterVerificationLimits& requested) const {
        const TerrainShadowCasterVerificationLimits hard;
        if (requested.maximumSourceBytes > hard.maximumSourceBytes || requested.maximumFileBytes > hard.maximumFileBytes
            || requested.maximumWorkingBytes > hard.maximumWorkingBytes)
            Fail("TERRAIN_SHADOW_RECEIVER_LIMITS", "接收面来源复核只能收紧硬上限。");
        if (!requested.maximumSourceBytes || !requested.maximumFileBytes || !requested.maximumWorkingBytes
            || working >= limits.maximumWorkingBytes)
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "驻留接收面未留下来源复核的读取或临时工作预算。");
        return {std::min(requested.maximumSourceBytes, limits.maximumSourceBytes),
            std::min(requested.maximumFileBytes, limits.maximumFileBytes),
            std::min(requested.maximumWorkingBytes, limits.maximumWorkingBytes - working)};
    }
    void VerifyInputs(const MapArchiveSession& source, std::span<const MemberReplacement> replacements,
        std::span<const std::string> removals, const TerrainShadowCasterVerificationLimits& scratch,
        const char* staleCode) const {
        if (ordinary) return;
        {
            const auto light = Take(MapData::TerrainShadowResources::Capture(gameRoot,
                Game::NativeEditorStartupRules::Classic127aExpansion,
                std::min(scratch.maximumSourceBytes, scratch.maximumWorkingBytes)));
            if (light.DependencyFingerprint() != lightFingerprint)
                Fail("TERRAIN_SHADOW_RECEIVER_PROFILE_STALE", "实际MaxHeight/profile来源已变化。");
        }
        auto readLimits = limits;
        readLimits.maximumSourceBytes = scratch.maximumSourceBytes;
        readLimits.maximumFileBytes = scratch.maximumFileBytes;
        readLimits.maximumWorkingBytes = scratch.maximumWorkingBytes;
        Inputs inputs(source, gameRoot, casters->Terrain().header.mainTileset, replacements, removals, readLimits, {});
        for (const auto& dependency : dependencies) {
            (void)inputs.Read(dependency.name);
            if (inputs.facts.at(dependency.name) != dependency)
                Fail(staleCode, "接收面模型、表或实际winner/absence已变化：" + dependency.name);
            inputs.Release(dependency.name);
        }
        inputs.Verify();
    }

    void Tick(std::size_t count, std::stop_token cancellation) {
        Cancel(cancellation); Charge(work, count, limits.maximumWork);
    }
    std::array<char, 4> RampCorners(std::uint32_t x, std::uint32_t y) const {
        if (rampVertices.empty()) return {'A', 'A', 'A', 'A'};
        const auto indices = Corners(casters->Terrain(), x, y);
        return {rampVertices[indices[2]].corner, rampVertices[indices[3]].corner,
            rampVertices[indices[1]].corner, rampVertices[indices[0]].corner};
    }
    float Height(std::size_t index) const {
        return static_cast<float>(static_cast<double>(GroundHeight(casters->Terrain().vertices[index]))
            + (rampVertices.empty() ? 0.0 : rampVertices[index].heightOffset));
    }
    void Transform(Record& record, std::uint8_t minimum, Inputs& inputs, std::stop_token cancellation,
        int shiftX = 0, int shiftY = 0, float preX = 0, float preY = 0) {
        const auto& terrain = casters->Terrain();
        const float degrees = static_cast<float>(static_cast<double>(std::bit_cast<float>(0x40490fdbu))
            * std::bit_cast<float>(0x3bb60b61u));
        const float angle = static_cast<float>(static_cast<double>(degrees) * -90.0);
        const float cosine = static_cast<float>(std::cos(static_cast<double>(angle)));
        const float sine = static_cast<float>(std::sin(static_cast<double>(angle)));
        const double tx = static_cast<double>(terrain.header.offset[0]) + (static_cast<double>(record.x) + shiftX) * 128.0;
        const double ty = static_cast<double>(terrain.header.offset[1]) + (static_cast<double>(record.y) + shiftY) * 128.0;
        const double tz = minimum * 128.0 - 256.0;
        const auto geosets = models[record.model].source.Geosets();
        Charge(inputs.working, geosets.size(), limits.maximumWorkingBytes, sizeof(std::vector<Vec3>));
        record.positions.reserve(geosets.size());
        for (const auto& geoset : geosets) {
            Charge(inputs.working, geoset.positions.size(), limits.maximumWorkingBytes, sizeof(Vec3));
            auto& output = record.positions.emplace_back(); output.reserve(geoset.positions.size());
            for (const auto& point : geoset.positions) {
                Tick(8, cancellation);
                auto local = point;
                if (preX != 0 || preY != 0) {
                    // 5F3050在原局部positions加半占地偏移，各自落F32，之后才57EAD0。
                    local[0] = static_cast<float>(static_cast<double>(point[0]) + preX);
                    local[1] = static_cast<float>(static_cast<double>(point[1]) + preY);
                }
                // 73D830：T*R，平移不再旋转；每坐标在变形前落一次F32。
                Vec3 world{static_cast<float>(static_cast<double>(local[1]) * -sine + static_cast<double>(local[0]) * cosine + tx),
                    static_cast<float>(static_cast<double>(local[0]) * sine + static_cast<double>(local[1]) * cosine + ty),
                    static_cast<float>(static_cast<double>(local[2]) + tz)};
                const double fx = (static_cast<double>(world[0]) - terrain.header.offset[0]) / 128.0;
                const double fy = (static_cast<double>(world[1]) - terrain.header.offset[1]) / 128.0;
                if (!std::isfinite(fx) || !std::isfinite(fy) || std::trunc(fx) < 0 || std::trunc(fy) < 0
                    || std::trunc(fx) >= terrain.header.width || std::trunc(fy) >= terrain.header.height)
                    Fail("TERRAIN_SHADOW_RECEIVER_MODEL_BOUNDS", "悬崖变形顶点不能取得原版地图格，未夹回地图。");
                const auto x = static_cast<std::uint32_t>(std::trunc(fx)), y = static_cast<std::uint32_t>(std::trunc(fy));
                const auto maxX = terrain.header.width - 1, maxY = terrain.header.height - 1;
                const auto base = static_cast<std::size_t>(y) * terrain.header.width + x;
                const auto x1 = std::min(x + 1, maxX), y1 = std::min(y + 1, maxY);
                const std::array<std::size_t, 4> corners{base, static_cast<std::size_t>(y) * terrain.header.width + x1,
                    static_cast<std::size_t>(y1) * terrain.header.width + x, static_cast<std::size_t>(y1) * terrain.header.width + x1};
                std::array<float, 4> relief{};
                for (std::size_t i = 0; i < 4; ++i) relief[i] = RawHeight(terrain.vertices[corners[i]]);
                const float localX = static_cast<float>(static_cast<double>(world[0]) - (terrain.header.offset[0] + x * 128.0));
                const float localY = static_cast<float>(static_cast<double>(world[1]) - (terrain.header.offset[1] + y * 128.0));
                const float u = static_cast<float>(static_cast<double>(localX) / 128.0), v = static_cast<float>(static_cast<double>(localY) / 128.0);
                if (u >= -0.001 && u <= 1.001 && v >= -0.001 && v <= 1.001) {
                    const auto baseHeight = *std::min_element(relief.begin(), relief.end());
                    const float s = static_cast<float>(1.0 - v), t = static_cast<float>(1.0 - u);
                    const float lifted = static_cast<float>((static_cast<double>(relief[0]) - baseHeight) * t * s + baseHeight
                        + (static_cast<double>(relief[1]) - baseHeight) * s * u
                        + (static_cast<double>(relief[2]) - baseHeight) * t * v
                        + (static_cast<double>(relief[3]) - baseHeight) * v * u + world[2]);
                    if (std::abs(static_cast<double>(lifted) - world[2]) >= 0x1p-22) world[2] = lifted;
                }
                if (!std::all_of(world.begin(), world.end(), [](float value) { return std::isfinite(value); }))
                    Fail("TERRAIN_SHADOW_RECEIVER_MODEL_HEIGHT", "悬崖变形产生非有限坐标。");
                output.push_back(world);
            }
        }
    }
};

Result<std::shared_ptr<const TerrainShadowReceiverResources>> TerrainShadowReceiverResources::Capture(
    const MapArchiveSession& source, const std::filesystem::path& gameRoot,
    std::shared_ptr<const TerrainShadowCasterResources> casters, const MapData::TerrainShadowResources& light,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    const TerrainShadowReceiverLimits& limits, std::stop_token cancellation) {
    using Output = Result<std::shared_ptr<const TerrainShadowReceiverResources>>;
    try {
        const TerrainShadowReceiverLimits hard;
        if (!limits.maximumSourceBytes || limits.maximumSourceBytes > hard.maximumSourceBytes
            || !limits.maximumFileBytes || limits.maximumFileBytes > hard.maximumFileBytes
            || !limits.maximumWorkingBytes || limits.maximumWorkingBytes > hard.maximumWorkingBytes
            || !limits.maximumWork || limits.maximumWork > hard.maximumWork
            || !limits.maximumRecords || limits.maximumRecords > hard.maximumRecords
            || !limits.maximumModels || limits.maximumModels > hard.maximumModels)
            Fail("TERRAIN_SHADOW_RECEIVER_LIMITS", "接收面预算只能收紧非零硬上限。");
        Cancel(cancellation);
        if (!casters) Fail("TERRAIN_SHADOW_RECEIVER_SOURCE", "接收面缺少实际地形与DOO来源。");
        // 公共Capture也必须闭合同一游戏目录，不能借A目录筛出的caster集合
        // 配B目录的cliff/profile。规范路径及实际目录等价性均由文件系统给出。
        if (gameRoot.empty() != casters->GameRoot().empty())
            Fail("TERRAIN_SHADOW_RECEIVER_GAME_ROOT", "接收面与借用阴影模型未使用同一实际游戏目录。");
        if (!gameRoot.empty()) {
            std::error_code pathError;
            if (gameRoot.native().size() > 32767)
                Fail("TERRAIN_SHADOW_RECEIVER_GAME_ROOT", "接收面游戏来源路径超过有界支持范围。");
            const auto actualRoot = std::filesystem::canonical(gameRoot, pathError);
            if (pathError || !std::filesystem::equivalent(actualRoot, casters->GameRoot(), pathError) || pathError)
                Fail("TERRAIN_SHADOW_RECEIVER_GAME_ROOT", "接收面与借用阴影模型的实际游戏目录不同或无法核对。");
        }
        const auto& terrain = casters->Terrain();
        auto state = std::make_shared<State>(); state->casters = std::move(casters);
        state->sourceSha = source.SourceSha256(); state->gameRoot = state->casters->GameRoot(); state->limits = limits;
        // 核对借用owner确属同一最终地图视图，不接受别的图或旧替换列表。
        TerrainShadowCasterVerificationLimits verification;
        verification.maximumSourceBytes = std::min(verification.maximumSourceBytes, limits.maximumSourceBytes);
        verification.maximumFileBytes = std::min(verification.maximumFileBytes, limits.maximumFileBytes);
        verification.maximumWorkingBytes = limits.maximumWorkingBytes;
        Check(state->casters->VerifySources(source, replacements, removals, cancellation, verification));
        if (terrain.header.version != 11 || terrain.header.width < 2 || terrain.header.height < 2
            || terrain.header.width > 2049 || terrain.header.height > 2049
            || terrain.vertices.size() != static_cast<std::size_t>(terrain.header.width) * terrain.header.height)
            Fail("TERRAIN_SHADOW_TERRAIN", "接收面需要完整经典W3E原始顶点。");
        const bool hasSpecials = !state->casters->SpecialDoodads().empty();
        if (hasSpecials) state->ordinary = false;
        const auto firstLevel = terrain.vertices.front().CliffLevel();
        for (const auto& vertex : terrain.vertices) {
            state->Tick(1, cancellation);
            if (vertex.IsRamp()) state->ordinary = false;
            if (vertex.CliffLevel() != firstLevel) state->ordinary = false;
        }
        if (state->ordinary)
            return Output::Success(std::shared_ptr<const TerrainShadowReceiverResources>(new TerrainShadowReceiverResources(std::move(state))));
        // 复杂receiver还没共用于DOO变换Z；不得借wire Z绕过该门。
        if (!state->casters->Casters().empty())
            Fail("TERRAIN_SHADOW_RECEIVER_CASTER_UNSUPPORTED", "复杂地形上的投影装饰物Z尚未接通。");
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const auto offset = static_cast<double>(terrain.header.offset[axis]);
            const auto last = offset + ((axis ? terrain.header.height : terrain.header.width) - 1) * 128.0;
            if (!std::isfinite(offset) || std::fmod(offset, 128.0) != 0 || offset < -32768 || last > 32640)
                Fail("TERRAIN_SHADOW_COORDINATE", "接收面须128对齐并位于原版九位顶点坐标域。");
        }
        for (const auto& vertex : terrain.vertices) if (vertex.extendedTextureLayout || vertex.textureFlagsExtensionRaw
            || (vertex.groundHeightRaw & 0xc000) || vertex.UnknownWaterFlags() || vertex.CliffLevel() == 15)
            Fail("TERRAIN_SHADOW_TERRAIN_FLAGS", "复杂接收面遇到未闭合的地形原始字段。");
        if (!light.TerrainHeightBounds())
            Fail("TERRAIN_SHADOW_HEIGHT_BOUNDS_UNKNOWN", "悬崖接收面缺少同次profile的实际MaxHeight。");
        // 借用的light也必须来自本次实际gameRoot；检查真实profile，不接受
        // 调用者从别的游戏目录递来的数值或来源对象。普通面不消费此输入。
        {
            const auto actualLight = Take(MapData::TerrainShadowResources::Capture(state->gameRoot,
                Game::NativeEditorStartupRules::Classic127aExpansion,
                std::min(limits.maximumSourceBytes, limits.maximumWorkingBytes)));
            if (actualLight.DependencyFingerprint() != light.DependencyFingerprint())
                Fail("TERRAIN_SHADOW_RECEIVER_PROFILE_STALE", "接收面MaxHeight与本次实际游戏profile来源不一致。");
        }
        const auto maximumLevel = std::trunc((static_cast<double>((*light.TerrainHeightBounds())[0]) + 256.0) / 128.0);
        if (maximumLevel < 0 || maximumLevel > 14)
            Fail("TERRAIN_SHADOW_RECEIVER_PROFILE", "实际MaxHeight派生的悬崖上限不在0..14支持域。");
        state->lightFingerprint = light.DependencyFingerprint();
        Inputs inputs(source, state->gameRoot, terrain.header.mainTileset, replacements, removals, limits, cancellation);
        const auto specials = hasSpecials ? SpecialGeometries(inputs, terrain, state->casters->SpecialDoodads(),
            std::uint8_t(maximumLevel), state->work, limits, cancellation) : std::vector<SpecialGeometry>{};
        const bool hasRamps = std::any_of(terrain.vertices.begin(), terrain.vertices.end(),
            [](const auto& vertex) { return vertex.IsRamp(); });
        if (hasRamps) {
            Charge(inputs.working, terrain.vertices.size(), limits.maximumWorkingBytes, sizeof(State::RampVertex));
            state->rampVertices.resize(terrain.vertices.size());
            for (std::uint32_t y = 0; y < terrain.header.height; ++y) for (std::uint32_t x = 0; x < terrain.header.width; ++x) {
                // 远离原R位的角不会参与任何R/S格或模型名，不为它重复查询第二圈。
                state->Tick(9, cancellation); bool adjacent = false;
                for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) {
                    const int nx = int(x) + dx, ny = int(y) + dy;
                    if (nx >= 0 && ny >= 0 && nx < int(terrain.header.width) && ny < int(terrain.header.height)
                        && terrain.vertices[std::size_t(ny) * terrain.header.width + nx].IsRamp()) adjacent = true;
                }
                if (!adjacent) continue;
                // 共用查询包含固定两圈验证、九点完整分类及S高度，先收其有限工作上界。
                state->Tick(1024, cancellation);
                const auto value = Take(MapData::QueryNativeTerrainRamp(terrain, std::uint8_t(maximumLevel), x, y));
                const auto index = std::size_t(y) * terrain.header.width + x;
                if (terrain.vertices[index].IsRamp() && (value.corner == '!' || !value.belongsToRampCell))
                    Fail("TERRAIN_SHADOW_RAMP_GEOMETRY", "孤立或未闭合的原始坡道位不能构造普通接收面，原阴影保留。");
                state->rampVertices[index] = {value.corner, value.heightOffset};
            }
        }
        const auto rows = TableRows(inputs, "TerrainArt/CliffTypes.slk", "cliffid");
        const auto width = terrain.header.width - 1, height = terrain.header.height - 1, blockWidth = (width + 3) / 4;
        Charge(inputs.working, static_cast<std::size_t>(width) * height, limits.maximumWorkingBytes);
        state->modelCells.resize(static_cast<std::size_t>(width) * height);
        if (hasSpecials) {
            Charge(inputs.working, static_cast<std::size_t>(width) * height, limits.maximumWorkingBytes, sizeof(std::uint32_t));
            state->specialAnchors.resize(static_cast<std::size_t>(width) * height);
            for (const auto& special : specials)
                for (auto x = special.x; x < special.x + special.width / 4; ++x)
                    for (auto y = special.y; y < special.y + special.height / 4; ++y) {
                        state->Tick(1, cancellation);
                        const auto index = static_cast<std::size_t>(y) * width + x;
                        if (!state->specialAnchors[index]) {
                            state->specialAnchors[index] = special.y * width + special.x + 1;
                            state->modelCells[index] = 1;
                        }
                    }
        }
        Charge(inputs.working, static_cast<std::size_t>(blockWidth) * ((height + 3) / 4), limits.maximumWorkingBytes,
            sizeof(std::vector<std::size_t>));
        state->blocks.resize(static_cast<std::size_t>(blockWidth) * ((height + 3) / 4));
        std::map<std::string, std::vector<std::string>, std::less<>> variants;
        std::map<std::string, std::size_t, std::less<>> modelIndices;
        const auto modelIndex = [&](const std::string& path) {
            auto model = modelIndices.find(path);
            if (model != modelIndices.end()) return model->second;
            if (state->models.size() >= limits.maximumModels)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "接收面唯一模型数超过预算。");
            const auto& bytes = inputs.Read(path).bytes;
            ValidateModel(bytes, inputs);
            auto raw = Take(Render::ReadMdxTerrainReceiverSource(bytes,
                std::min<std::size_t>(128u * 1024u * 1024u, limits.maximumWorkingBytes - inputs.working)));
            for (const auto& geoset : raw.Geosets()) {
                if (geoset.positions.empty() && !geoset.groups.empty())
                    Fail("TERRAIN_SHADOW_RECEIVER_GROUP", "零顶点GEOS的原版group指针资格尚未闭合。");
                for (const auto& group : geoset.groups)
                    if (group.NativeTopology() == 3 && group.indexCount % 3)
                        Fail("TERRAIN_SHADOW_RECEIVER_GROUP", "原版list组计数不是完整三角形，拒绝越界求交。");
            }
            Charge(inputs.working, raw.ChargedResidentBytes(), limits.maximumWorkingBytes);
            Charge(inputs.working, path.size() + sizeof(State::Model), limits.maximumWorkingBytes, 2);
            const auto index = state->models.size(); state->models.push_back({path, std::move(raw)});
            modelIndices.emplace(path, index); return index;
        };
        // 每block的原记录顺序为x外/y内；不合批，不按材质重排。
        for (std::uint32_t x = 0; x < width; ++x) for (std::uint32_t y = 0; y < height; ++y) {
            state->Tick(4, cancellation);
            if (hasSpecials && state->specialAnchors[static_cast<std::size_t>(y) * width + x]) continue;
            const auto corners = Corners(terrain, x, y);
            std::array<std::uint8_t, 4> levels{};
            for (std::size_t i = 0; i < 4; ++i) levels[i] = terrain.vertices[corners[i]].CliffLevel();
            const auto minimum = *std::min_element(levels.begin(), levels.end());
            auto maximum = *std::max_element(levels.begin(), levels.end());
            const auto labels = state->RampCorners(x, y);
            unsigned weight = 0;
            for (const auto label : labels) weight += label == 'R' ? 1u : label == 'S' ? 2u : 0u;
            const auto* ramp = weight && weight <= 1 ? MatchRamp(labels, false) : nullptr;
            // 57E210：有R/S时仅模式匹配才走模型，不回退异层判断。
            state->modelCells[static_cast<std::size_t>(y) * width + x] = weight
                ? MatchRamp(labels, true) != nullptr : maximum != minimum;
            if (weight ? !ramp : maximum == minimum) continue;
            auto [slot, variation] = CliffSlot(terrain, x, y);
            if (ramp) {
                // 5A86D0：本格/东/西/北/南，只扫描含R/S的格；严格最高层首顶点胜出。
                int highest = -1; slot = 15; variation = 0;
                for (const auto delta : {std::array{0,0}, std::array{1,0}, std::array{-1,0}, std::array{0,1}, std::array{0,-1}}) {
                    state->Tick(8, cancellation);
                    const int nx = int(x) + delta[0], ny = int(y) + delta[1];
                    if (nx < 0 || ny < 0 || nx >= int(width) || ny >= int(height)) continue;
                    const auto around = state->RampCorners(std::uint32_t(nx), std::uint32_t(ny));
                    if (std::none_of(around.begin(), around.end(), [](char c) { return c == 'R' || c == 'S'; })) continue;
                    for (const auto i : Corners(terrain, std::uint32_t(nx), std::uint32_t(ny))) {
                        const auto& vertex = terrain.vertices[i];
                        if (vertex.CliffLevel() > highest) { highest = vertex.CliffLevel(); slot = vertex.CliffTextureIndex(); }
                    }
                }
                if (slot == 15) slot = 0;
            } else {
                maximum = std::min<unsigned>(maximum, minimum + 2);
                if (maximum > maximumLevel) continue; // 原版不构造该record，模型路径高度为0
                for (auto& level : levels) level = static_cast<std::uint8_t>(std::min<unsigned>(level, maximum) - minimum);
            }
            if (slot >= terrain.header.cliffTilesets.size())
                Fail("TERRAIN_SHADOW_RECEIVER_CLIFF_SLOT", "实际悬崖槽超出W3E调色板。");
            const auto& id = terrain.header.cliffTilesets[slot];
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.id == id; });
            const auto field = ramp ? "rampmodeldir" : "cliffmodeldir";
            if (row == rows.end() || !row->fields.contains(field) || row->fields.at(field).empty())
                Fail("TERRAIN_SHADOW_RECEIVER_CLIFF_ROW", "实际CliffTypes缺少唯一模型目录：" + id);
            std::string shape;
            if (ramp) for (const auto label : labels) shape.push_back(label == 'R' ? 'L' : label);
            else for (const auto corner : {2u, 3u, 1u, 0u}) shape.push_back(static_cast<char>('A' + levels[corner]));
            // 50A480/50A4D0先把真实目录写成%s\%s，再由cliff/ramp追加形状。
            const auto& directory = row->fields.at(field);
            const auto prefix = Canonical("Doodads/Terrain/" + directory + "/" + directory + shape);
            auto found = variants.find(prefix);
            if (found == variants.end()) {
                std::vector<std::string> paths;
                for (unsigned variationIndex = 0; variationIndex < (ramp ? 1u : 4u); ++variationIndex) {
                    state->Tick(1, cancellation);
                    const auto base = prefix + std::to_string(variationIndex);
                    const auto& mdl = inputs.Read(base + ".mdl");
                    if (mdl.present)
                        Fail("TERRAIN_SHADOW_RECEIVER_MDL_UNSUPPORTED", "实际最高层悬崖MDL尚未解析，未改读低层MDX：" + base);
                    const auto path = base + ".mdx";
                    if (!inputs.Read(path).present) break;
                    paths.push_back(path);
                }
                if (paths.empty()) Fail("TERRAIN_SHADOW_RECEIVER_MODEL_MISSING", "悬崖形状没有原版首变体模型：" + prefix);
                Charge(inputs.working, prefix.size() + 512, limits.maximumWorkingBytes, 4);
                found = variants.emplace(prefix, std::move(paths)).first;
            }
            const auto& path = found->second[variation % found->second.size()];
            if (state->records.size() >= limits.maximumRecords)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "悬崖接收面记录数超过预算。");
            Charge(inputs.working, 1, limits.maximumWorkingBytes, 2 * sizeof(State::Record) + 2 * sizeof(std::size_t));
            State::Record record{x, y, int(x) + (ramp ? ramp->endX : 0), int(y) + (ramp ? ramp->endY : 0), modelIndex(path), {}};
            state->Transform(record, minimum, inputs, cancellation, ramp ? ramp->shiftX : 0, ramp ? ramp->shiftY : 0);
            state->blocks[static_cast<std::size_t>(y / 4) * blockWidth + x / 4].push_back(state->records.size());
            state->records.push_back(std::move(record));
        }
        // 5F2FD0：按原special登记序追加到其锚点block；不向占地覆盖的其它block复制。
        for (const auto& special : specials) {
            state->Tick(1, cancellation);
            auto mdl = special.model; mdl.replace(mdl.size() - 4, 4, ".MDL");
            if (inputs.Read(mdl).present)
                Fail("TERRAIN_SHADOW_RECEIVER_MDL_UNSUPPORTED", "实际最高层特殊MDL尚未解析，未改读低层MDX。");
            if (!inputs.Read(special.model).present)
                Fail("TERRAIN_SHADOW_SPECIAL_MODEL_MISSING", "缺少特殊接收面的实际模型，未使用预览替代品。");
            if (state->records.size() >= limits.maximumRecords)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "特殊接收面记录数超过预算。");
            Charge(inputs.working, 1, limits.maximumWorkingBytes, 2 * sizeof(State::Record) + 2 * sizeof(std::size_t));
            State::Record record{special.x, special.y, int(special.x + special.width / 4 - 1),
                int(special.y + special.height / 4 - 1), modelIndex(special.model), {}};
            state->Transform(record, special.minimum, inputs, cancellation, 0, 0,
                -static_cast<float>(special.height) * 16.0f, static_cast<float>(special.width) * 16.0f);
            state->blocks[static_cast<std::size_t>(special.y / 4) * blockWidth + special.x / 4].push_back(state->records.size());
            state->records.push_back(std::move(record));
        }
        inputs.Verify();
        state->sourceBytes = inputs.sourceBytes; state->working = inputs.working;
        state->dependencies.reserve(inputs.facts.size());
        for (auto& [name, fact] : inputs.facts) { (void)name; state->dependencies.push_back(std::move(fact)); }
        return Output::Success(std::shared_ptr<const TerrainShadowReceiverResources>(new TerrainShadowReceiverResources(std::move(state))));
    } catch (Failure& failure) { Output result; result.diagnostics = std::move(failure.diagnostics); return result; }
    catch (const std::bad_alloc&) { return Output::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面分配失败，原阴影保留。"); }
    catch (const std::length_error&) { return Output::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面容量异常，原阴影保留。"); }
}

std::span<const TerrainShadowCasterDependency> TerrainShadowReceiverResources::Dependencies() const noexcept { return state_->dependencies; }
std::size_t TerrainShadowReceiverResources::RecordCount() const noexcept { return state_->records.size(); }
std::size_t TerrainShadowReceiverResources::SourceBytes() const noexcept { return state_->sourceBytes; }
std::size_t TerrainShadowReceiverResources::WorkingBytes() const noexcept { return state_->working; }
std::size_t TerrainShadowReceiverResources::Work() const noexcept { return state_->work; }

Result<MapData::TerrainShadowReceiverGrid> TerrainShadowReceiverResources::Sample(
    const MapData::TerrainShadowRebuildLimits& limits, std::stop_token cancellation) const {
    using Output = Result<MapData::TerrainShadowReceiverGrid>;
    const auto& terrain = state_->casters->Terrain();
    if (state_->ordinary)
        return MapData::SampleOrdinaryTerrainShadowHeights(terrain, state_->casters->SpecialDoodads(), limits, cancellation);
    try {
        const MapData::TerrainShadowRebuildLimits hard;
        if (limits.maximumDimension > hard.maximumDimension || limits.maximumPixels > hard.maximumPixels
            || limits.maximumSampleBytes > hard.maximumSampleBytes || limits.maximumWork > hard.maximumWork)
            Fail("TERRAIN_SHADOW_LIMITS", "接收面采样只能收紧硬预算。");
        const auto width = (terrain.header.width - 1) * 4, height = (terrain.header.height - 1) * 4;
        if (width > limits.maximumDimension || height > limits.maximumDimension || height > limits.maximumPixels / width
            || static_cast<std::size_t>(width) * height > limits.maximumSampleBytes / sizeof(float))
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "接收面高度场超过像素或有效存储预算。");
        MapData::TerrainShadowReceiverGrid grid;
        grid.width = width; grid.height = height;
        grid.receiverHeights.reserve(static_cast<std::size_t>(width) * height);
        const auto tick = [&](std::size_t count = 1) { Cancel(cancellation); Charge(grid.work, count, limits.maximumWork); };
        const auto triangle = [&](const Vec3& origin, const Vec3& direction, const std::array<Vec3, 3>& points) {
            tick();
            const auto distance = MapData::ShadowNative::IntersectSigned(origin, direction, points);
            if (distance && !std::isfinite(*distance))
                Fail("TERRAIN_SHADOW_RECEIVER_INTERSECTION", "接收面交距不能表示为有限F32。");
            return distance;
        };
        // 580B40：保留全部组的最小signed交距，却只返回最后一次group的bool。
        // 不把any-hit、未知拓扑或空组折叠为“前面命中过就成功”。
        const auto modelRay = [&](const State::Record& record, const Vec3& origin, const Vec3& direction) {
            float minimum = std::numeric_limits<float>::infinity();
            bool lastGroupHit = false;
            const auto geosets = state_->models[record.model].source.Geosets();
            for (std::size_t g = 0; g < geosets.size(); ++g) {
                const auto& positions = record.positions[g];
                for (const auto& group : geosets[g].groups) {
                    tick(); lastGroupHit = false;
                    float groupMinimum = std::numeric_limits<float>::infinity();
                    const auto indices = std::span(geosets[g].indices).subspan(group.firstIndex, group.indexCount);
                    const auto test = [&](std::size_t a, std::size_t b, std::size_t c) {
                        const auto distance = triangle(origin, direction, {positions[indices[a]], positions[indices[b]], positions[indices[c]]});
                        if (distance && *distance < groupMinimum) { groupMinimum = *distance; lastGroupHit = true; }
                    };
                    if (group.NativeTopology() == 3)
                        for (std::size_t i = 0; i < indices.size(); i += 3) test(i, i + 1, i + 2);
                    else if (group.NativeTopology() == 4)
                        for (std::size_t i = 2; i < indices.size(); ++i) test(i - 2, i - 1, i);
                    if (lastGroupHit && groupMinimum <= minimum) minimum = groupMinimum;
                }
            }
            return std::pair{lastGroupHit, minimum};
        };
        const auto tileWidth = terrain.header.width - 1, tileHeight = terrain.header.height - 1, blockWidth = (tileWidth + 3) / 4;
        const auto point = [&](std::size_t index) {
            return Vec3{static_cast<float>(terrain.header.offset[0] + (index % terrain.header.width) * 128.0),
                static_cast<float>(terrain.header.offset[1] + (index / terrain.header.width) * 128.0), state_->Height(index)};
        };
        for (std::uint32_t py = 0; py < height; ++py) for (std::uint32_t px = 0; px < width; ++px) {
            tick();
            const auto x = px / 4, y = py / 4;
            auto queryX = x, queryY = y;
            if (!state_->specialAnchors.empty()) {
                const auto anchor = state_->specialAnchors[static_cast<std::size_t>(y) * tileWidth + x];
                if (anchor) { queryX = (anchor - 1) % tileWidth; queryY = (anchor - 1) / tileWidth; }
            }
            const auto block = static_cast<std::size_t>(queryY / 4) * blockWidth + queryX / 4;
            const auto& records = state_->blocks[block];
            const Vec3 location{static_cast<float>(terrain.header.offset[0] + px * 32.0 + 16.0),
                static_cast<float>(terrain.header.offset[1] + py * 32.0 + 16.0), 0};
            const bool isModel = state_->modelCells[static_cast<std::size_t>(y) * tileWidth + x] != 0;
            double sampled = 0;
            if (isModel) {
                const State::Record* selected = nullptr;
                // 57F790先做special锚点映射，再查该block首包含record；世界射线XY不变。
                for (const auto index : records) {
                    tick();
                    const auto& record = state_->records[index];
                    if (int(queryX) >= std::min(int(record.x), record.endX) && int(queryX) <= std::max(int(record.x), record.endX)
                        && int(queryY) >= std::min(int(record.y), record.endY) && int(queryY) <= std::max(int(record.y), record.endY)) {
                        selected = &record; break;
                    }
                }
                if (selected) {
                    const auto result = modelRay(*selected, {location[0], location[1], 2560.0f}, {0, 0, -1});
                    if (result.first) sampled = 2560.0 - result.second;
                    else {
                        // 原像素中心分数均为奇数/8，最近顶点不存在正好半格的舍入歧义。
                        const auto nx = (px + 2) / 4, ny = (py + 2) / 4;
                        sampled = state_->Height(static_cast<std::size_t>(ny) * terrain.header.width + nx);
                    }
                }
            } else {
                const Vec3 origin{location[0], location[1], -1280.0f}, direction{0, 0, 1};
                float minimum = std::numeric_limits<float>::infinity();
                // 5A5E90按整个4x4 block先扫描基础ground，再扫描该block全部模型。
                // 每个有效像素有完整ground三角形；其竖线必与包围ground的预筛球相交。
                for (auto gy = (y / 4) * 4; gy < std::min((y / 4 + 1) * 4, tileHeight); ++gy)
                    for (auto gx = (x / 4) * 4; gx < std::min((x / 4 + 1) * 4, tileWidth); ++gx) {
                        const auto indices = Corners(terrain, gx, gy);
                        const std::array<Vec3, 4> positions{point(indices[0]), point(indices[1]), point(indices[2]), point(indices[3])};
                        for (const auto indices3 : {std::array{0u, 1u, 2u}, std::array{2u, 1u, 3u}}) {
                            const auto distance = triangle(origin, direction,
                                {positions[indices3[0]], positions[indices3[1]], positions[indices3[2]]});
                            if (distance && *distance < minimum) minimum = *distance;
                        }
                    }
                for (const auto index : records) {
                    const auto result = modelRay(state_->records[index], origin, direction);
                    if (result.first && result.second < minimum) minimum = result.second;
                }
                if (std::isfinite(minimum)) sampled = -1280.0 + minimum;
            }
            if (!std::isfinite(sampled)) Fail("TERRAIN_SHADOW_RECEIVER_HEIGHT", "接收高度不是有限值。");
            grid.receiverHeights.push_back(static_cast<float>(sampled + 0.1));
        }
        return Output::Success(std::move(grid));
    } catch (Failure& failure) { Output result; result.diagnostics = std::move(failure.diagnostics); return result; }
    catch (const std::bad_alloc&) { return Output::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面采样分配失败。"); }
    catch (const std::length_error&) { return Output::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面采样容量异常。"); }
}

Status TerrainShadowReceiverResources::VerifySources(const MapArchiveSession& source,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    const TerrainShadowCasterVerificationLimits& limits) const {
    try {
        if (source.SourceSha256() != state_->sourceSha)
            Fail("TERRAIN_SHADOW_RECEIVER_SOURCE_STALE", "接收面来自另一份地图归档。");
        const auto scratch = state_->VerificationBudget(limits);
        Check(state_->casters->VerifySources(source, replacements, removals, {}, scratch));
        state_->VerifyInputs(source, replacements, removals, scratch, "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面来源复核分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面来源复核容量异常。"); }
}
Status TerrainShadowReceiverResources::VerifyReadback(const MapArchiveSession& saved,
    const TerrainShadowCasterVerificationLimits& limits) const {
    try {
        const auto scratch = state_->VerificationBudget(limits);
        Check(state_->casters->VerifyReadback(saved, {}, scratch));
        state_->VerifyInputs(saved, {}, {}, scratch, "TERRAIN_SHADOW_RECEIVER_READBACK_STALE");
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面回读分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面回读容量异常。"); }
}

} // namespace War3Map::Services
