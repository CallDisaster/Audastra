#include "War3MapCore/Services/TerrainShadowReceiverResources.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Render/MdxTerrainReceiver.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Resources/TerrainTable.h"
#include "War3MapCore/Text/TextUtil.h"
#include "../Render/TerrainShadowNativeRay.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::Services {
namespace {
constexpr const char* kSubsystem = "Terrain.ShadowReceivers";
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
            const auto key = Resources::CanonicalImportPath(item.name);
            if (!key.Succeeded() || !key.value) continue;
            Charge(working, key.value->size() + 128, limits_.maximumWorkingBytes, 4);
            if (!seen.insert(*key.value).second)
                Fail("TERRAIN_SHADOW_RECEIVER_FINAL_CONFLICT", "最终资源视图含重复替换成员。");
            replacements_.emplace(*key.value, &item);
        }
        for (const auto& item : removed) {
            if (item.size() > ArchiveExactMemberLimits{}.pathBytes)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "最终删除成员名超过路径预算。");
            const auto key = Resources::CanonicalImportPath(item);
            if (!key.Succeeded() || !key.value) continue;
            Charge(working, key.value->size() + 128, limits_.maximumWorkingBytes, 4);
            if (!seen.insert(*key.value).second)
                Fail("TERRAIN_SHADOW_RECEIVER_FINAL_CONFLICT", "最终资源同时替换/删除或重复删除。");
            removals_.insert(*key.value);
        }
        Check(guard_.Watch(source.SourcePath()));
    }
    const ArchiveExactMember& Read(std::string_view name) {
        Cancel(cancellation_);
        const auto key = Canonical(name);
        if (const auto found = files_.find(key); found != files_.end()) return found->second;
        if (files_.size() >= 32768)
            Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "接收面的来源记录数超过硬上限。");
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
        Charge(working, key.size() + fact.gameContainer.size() + 256, limits_.maximumWorkingBytes, 4);
        fact.bytes = file.bytes.size();
        if (file.present) fact.sha256 = Take(Sha256(file.bytes));
        facts.emplace(key, std::move(fact));
        return files_.emplace(key, std::move(file)).first->second;
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

std::vector<Resources::TerrainTableRow> CliffRows(Inputs& inputs) {
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
    auto table = Take(Resources::CaptureTerrainTable(resolver, "TerrainArt/CliffTypes.slk", "cliffid"));
    return table.Rows();
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
        std::size_t model = 0;
        std::vector<std::vector<Vec3>> positions; // GEOS和VRTX顺序，group索引借用Model
    };
    std::shared_ptr<const TerrainShadowCasterResources> casters;
    std::string sourceSha, lightFingerprint;
    std::filesystem::path gameRoot;
    TerrainShadowReceiverLimits limits;
    std::vector<TerrainShadowCasterDependency> dependencies;
    std::vector<Model> models;
    std::vector<Record> records;
    std::vector<std::vector<std::size_t>> blocks;
    std::size_t sourceBytes = 0, working = 0, work = 0;
    bool ordinary = true;

    void Tick(std::size_t count, std::stop_token cancellation) {
        Cancel(cancellation); Charge(work, count, limits.maximumWork);
    }
    void Transform(Record& record, std::uint8_t minimum, Inputs& inputs, std::stop_token cancellation) {
        const auto& terrain = casters->Terrain();
        const float degrees = static_cast<float>(static_cast<double>(std::bit_cast<float>(0x40490fdbu))
            * std::bit_cast<float>(0x3bb60b61u));
        const float angle = static_cast<float>(static_cast<double>(degrees) * -90.0);
        const float cosine = static_cast<float>(std::cos(static_cast<double>(angle)));
        const float sine = static_cast<float>(std::sin(static_cast<double>(angle)));
        const double tx = static_cast<double>(terrain.header.offset[0]) + record.x * 128.0;
        const double ty = static_cast<double>(terrain.header.offset[1]) + record.y * 128.0;
        const double tz = minimum * 128.0 - 256.0;
        const auto geosets = models[record.model].source.Geosets();
        Charge(inputs.working, geosets.size(), limits.maximumWorkingBytes, sizeof(std::vector<Vec3>));
        record.positions.reserve(geosets.size());
        for (const auto& geoset : geosets) {
            Charge(inputs.working, geoset.positions.size(), limits.maximumWorkingBytes, sizeof(Vec3));
            auto& output = record.positions.emplace_back(); output.reserve(geoset.positions.size());
            for (const auto& point : geoset.positions) {
                Tick(8, cancellation);
                // 73D830：T*R，平移不再旋转；每坐标在变形前落一次F32。
                Vec3 world{static_cast<float>(static_cast<double>(point[1]) * -sine + static_cast<double>(point[0]) * cosine + tx),
                    static_cast<float>(static_cast<double>(point[0]) * sine + static_cast<double>(point[1]) * cosine + ty),
                    static_cast<float>(static_cast<double>(point[2]) + tz)};
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
        Check(state->casters->VerifySources(source, replacements, removals, cancellation));
        if (terrain.header.version != 11 || terrain.header.width < 2 || terrain.header.height < 2
            || terrain.header.width > 2049 || terrain.header.height > 2049
            || terrain.vertices.size() != static_cast<std::size_t>(terrain.header.width) * terrain.header.height)
            Fail("TERRAIN_SHADOW_TERRAIN", "接收面需要完整经典W3E原始顶点。");
        if (!state->casters->SpecialDoodads().empty())
            Fail("TERRAIN_SHADOW_SPECIAL_GEOMETRY", "特殊装饰物接收面尚未接通，原阴影保留。");
        const auto firstLevel = terrain.vertices.front().CliffLevel();
        for (const auto& vertex : terrain.vertices) {
            state->Tick(1, cancellation);
            if (vertex.IsRamp()) Fail("TERRAIN_SHADOW_RAMP_GEOMETRY", "坡道接收面的anchor与模型尚未接通，原阴影保留。");
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
        const auto rows = CliffRows(inputs);
        const auto width = terrain.header.width - 1, height = terrain.header.height - 1, blockWidth = (width + 3) / 4;
        Charge(inputs.working, static_cast<std::size_t>(blockWidth) * ((height + 3) / 4), limits.maximumWorkingBytes,
            sizeof(std::vector<std::size_t>));
        state->blocks.resize(static_cast<std::size_t>(blockWidth) * ((height + 3) / 4));
        std::map<std::string, std::vector<std::string>, std::less<>> variants;
        std::map<std::string, std::size_t, std::less<>> modelIndices;
        // 每block的原记录顺序为x外/y内；不合批，不按材质重排。
        for (std::uint32_t x = 0; x < width; ++x) for (std::uint32_t y = 0; y < height; ++y) {
            state->Tick(4, cancellation);
            const auto corners = Corners(terrain, x, y);
            std::array<std::uint8_t, 4> levels{};
            for (std::size_t i = 0; i < 4; ++i) levels[i] = terrain.vertices[corners[i]].CliffLevel();
            const auto minimum = *std::min_element(levels.begin(), levels.end());
            auto maximum = *std::max_element(levels.begin(), levels.end());
            if (maximum == minimum) continue;
            maximum = std::min<unsigned>(maximum, minimum + 2);
            if (maximum > maximumLevel) continue; // 原版不构造该record，模型路径高度为0
            for (auto& level : levels) level = static_cast<std::uint8_t>(std::min<unsigned>(level, maximum) - minimum);
            const auto [slot, variation] = CliffSlot(terrain, x, y);
            if (slot >= terrain.header.cliffTilesets.size())
                Fail("TERRAIN_SHADOW_RECEIVER_CLIFF_SLOT", "实际悬崖槽超出W3E调色板。");
            const auto& id = terrain.header.cliffTilesets[slot];
            const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& item) { return item.id == id; });
            if (row == rows.end() || !row->fields.contains("cliffmodeldir"))
                Fail("TERRAIN_SHADOW_RECEIVER_CLIFF_ROW", "实际CliffTypes缺少唯一模型目录：" + id);
            std::string shape;
            for (const auto corner : {2u, 3u, 1u, 0u}) shape.push_back(static_cast<char>('A' + levels[corner]));
            const auto prefix = Canonical("Doodads/Terrain/" + row->fields.at("cliffmodeldir") + shape);
            auto found = variants.find(prefix);
            if (found == variants.end()) {
                std::vector<std::string> paths;
                for (unsigned variationIndex = 0; variationIndex < 4; ++variationIndex) {
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
            auto model = modelIndices.find(path);
            if (model == modelIndices.end()) {
                if (state->models.size() >= limits.maximumModels)
                    Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "悬崖接收面唯一模型数超过预算。");
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
                model = modelIndices.emplace(path, index).first;
            }
            if (state->records.size() >= limits.maximumRecords)
                Fail("TERRAIN_SHADOW_RECEIVER_BUDGET", "悬崖接收面记录数超过预算。");
            Charge(inputs.working, 1, limits.maximumWorkingBytes, 2 * sizeof(State::Record) + 2 * sizeof(std::size_t));
            State::Record record{x, y, model->second, {}}; state->Transform(record, minimum, inputs, cancellation);
            state->blocks[static_cast<std::size_t>(y / 4) * blockWidth + x / 4].push_back(state->records.size());
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
                static_cast<float>(terrain.header.offset[1] + (index / terrain.header.width) * 128.0), GroundHeight(terrain.vertices[index])};
        };
        for (std::uint32_t py = 0; py < height; ++py) for (std::uint32_t px = 0; px < width; ++px) {
            tick();
            const auto x = px / 4, y = py / 4;
            const auto block = static_cast<std::size_t>(y / 4) * blockWidth + x / 4;
            const auto& records = state_->blocks[block];
            const Vec3 location{static_cast<float>(terrain.header.offset[0] + px * 32.0 + 16.0),
                static_cast<float>(terrain.header.offset[1] + py * 32.0 + 16.0), 0};
            const auto corners = Corners(terrain, x, y);
            const auto level = terrain.vertices[corners[0]].CliffLevel();
            const bool isModel = std::any_of(corners.begin() + 1, corners.end(),
                [&](auto index) { return terrain.vertices[index].CliffLevel() != level; });
            double sampled = 0;
            if (isModel) {
                const State::Record* selected = nullptr;
                // 原版57F790取首个包含当前格的record；首批普通cliff均为单格。
                for (const auto index : records) {
                    tick();
                    const auto& record = state_->records[index];
                    if (record.x == x && record.y == y) { selected = &record; break; }
                }
                if (selected) {
                    const auto result = modelRay(*selected, {location[0], location[1], 2560.0f}, {0, 0, -1});
                    if (result.first) sampled = 2560.0 - result.second;
                    else {
                        // 原像素中心分数均为奇数/8，最近顶点不存在正好半格的舍入歧义。
                        const auto nx = (px + 2) / 4, ny = (py + 2) / 4;
                        sampled = GroundHeight(terrain.vertices[static_cast<std::size_t>(ny) * terrain.header.width + nx]);
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
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals) const {
    try {
        if (source.SourceSha256() != state_->sourceSha)
            Fail("TERRAIN_SHADOW_RECEIVER_SOURCE_STALE", "接收面来自另一份地图归档。");
        Check(state_->casters->VerifySources(source, replacements, removals));
        if (!state_->ordinary) {
            const auto light = Take(MapData::TerrainShadowResources::Capture(state_->gameRoot,
                Game::NativeEditorStartupRules::Classic127aExpansion, state_->limits.maximumSourceBytes));
            if (light.DependencyFingerprint() != state_->lightFingerprint)
                Fail("TERRAIN_SHADOW_RECEIVER_PROFILE_STALE", "实际MaxHeight/profile来源已变化。");
            Inputs inputs(source, state_->gameRoot, state_->casters->Terrain().header.mainTileset,
                replacements, removals, state_->limits, {});
            for (const auto& dependency : state_->dependencies) {
                (void)inputs.Read(dependency.name);
                if (inputs.facts.at(dependency.name) != dependency)
                    Fail("TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE", "接收面模型、表或实际winner/absence已变化：" + dependency.name);
            }
            inputs.Verify();
        }
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面来源复核分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面来源复核容量异常。"); }
}
Status TerrainShadowReceiverResources::VerifyReadback(const MapArchiveSession& saved) const {
    try {
        Check(state_->casters->VerifyReadback(saved));
        if (!state_->ordinary) {
            const auto light = Take(MapData::TerrainShadowResources::Capture(state_->gameRoot,
                Game::NativeEditorStartupRules::Classic127aExpansion, state_->limits.maximumSourceBytes));
            if (light.DependencyFingerprint() != state_->lightFingerprint)
                Fail("TERRAIN_SHADOW_RECEIVER_PROFILE_STALE", "保存回读时实际MaxHeight/profile来源已变化。");
            Inputs inputs(saved, state_->gameRoot, state_->casters->Terrain().header.mainTileset, {}, {}, state_->limits, {});
            for (const auto& dependency : state_->dependencies) {
                (void)inputs.Read(dependency.name);
                if (inputs.facts.at(dependency.name) != dependency)
                    Fail("TERRAIN_SHADOW_RECEIVER_READBACK_STALE", "保存结果的接收面资源与冻结集合不同：" + dependency.name);
            }
            inputs.Verify();
        }
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面回读分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_SHADOW_RECEIVER_ALLOCATION", "接收面回读容量异常。"); }
}

} // namespace War3Map::Services
