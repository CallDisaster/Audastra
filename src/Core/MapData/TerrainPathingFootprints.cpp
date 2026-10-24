#include "War3MapCore/MapData/TerrainPathingResources.h"

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
#include "War3MapCore/MapData/TerrainSpecialDoodadPathing.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <map>
#include <new>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr auto kSubsystem = "Terrain.PathingResources";
struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(kSubsystem, code, std::move(message)).diagnostics};
}
void Check(Status status) {
    if (!status.Succeeded()) throw Failure{std::move(status.diagnostics)};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw Failure{std::move(result.diagnostics)};
    return std::move(*result.value);
}
std::string FootprintPath(std::string_view path, bool allowBlpFallback = false) {
    if (path.empty() || path.size() >= 260)
        Fail("TERRAIN_NATIVE_FOOTPRINT_PATH", "占地贴图路径为空或超出经典路径容量。");
    auto result = Text::LowerAscii(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    // 原版先去掉扩展名，完整查找TGA后才查找BLP。普通占地允许两种已知
    // 声明，内部统一按首选TGA分组；特殊地形仍保留其独立的TGA域。
    if (allowBlpFallback && result.ends_with(".blp")) result.replace(result.size() - 4, 4, ".tga");
    if (!result.ends_with(".tga") || result.front() == '/' || result.find(':') != std::string::npos)
        Fail("TERRAIN_NATIVE_FOOTPRINT_FORMAT", "当前非空占地仅支持原版首选TGA路径：" + result);
    std::size_t start = 0;
    while (start < result.size()) {
        const auto end = result.find('/', start);
        const auto part = std::string_view(result).substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".."
            || std::any_of(part.begin(), part.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            Fail("TERRAIN_NATIVE_FOOTPRINT_PATH", "占地贴图不是明确的归档相对路径：" + result);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
} // namespace

Status TerrainPathingResources::CaptureNativeFootprints(const MemberReader& reader,
    const TerrainHeader& header, std::span<const DoodadPlacement> placements,
    std::optional<std::span<const DoodadPlacement>> afterPlacements,
    std::span<const TerrainNativeDoodadPathingType> types) {
    try {
        if (types.empty()) return Status::Ok();
        if (std::any_of(types.begin(), types.end(), [](const auto& type) {
                return type.gridPolicy == TerrainDoodadGridPolicy::StartupCategoryC;
            })) {
            if (sourceBytes_ > capturedMaximumBytes_ || objectWorkBytes_ > capturedMaximumBytes_ - sourceBytes_)
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "类别目录捕获没有剩余预算。");
            nativeCategoryDirectory_ = Take(Game::NativeDoodadCategoryDirectory::Capture(gameRoot_,
                Game::NativeEditorStartupRules::Classic127aExpansion,
                capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_));
            sourceBytes_ += nativeCategoryDirectory_->SourceBytes();
            objectWorkBytes_ += nativeCategoryDirectory_->WorkBytes();
        }
        const auto reserve = [&](std::size_t count, std::size_t each) {
            if (sourceBytes_ > capturedMaximumBytes_ || objectWorkBytes_ > capturedMaximumBytes_ - sourceBytes_
                || count > (capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_) / each)
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "非空占地工作超过同次捕获剩余预算。");
            objectWorkBytes_ += count * each;
        };
        if (header.width < 2 || header.height < 2 || header.width > 2049 || header.height > 2049
            || types.size() > placements.size() + (afterPlacements ? afterPlacements->size() : 0))
            Fail("TERRAIN_NATIVE_FOOTPRINT_LAYOUT", "占地输入需要完整经典地形尺寸及实际摆放类型集合。");
        struct Type { const TerrainNativeDoodadPathingType* value; std::string path; bool used = false; };
        struct Instance { const DoodadPlacement* placement; const TerrainNativeDoodadPathingType* type; bool after; };
        // 类型解析已预留自身输出；这里另计两棵树的节点、名称列表、精确读取结果、
        // 依赖及其增长容量。TGA与BLP两轮的名称、读取元数据、依赖及字符串
        // 都预收；缺失TGA也保留完整来源身份。分组key只借用归一化path。
        constexpr auto nodeLinks = 4 * sizeof(void*);
        constexpr auto stringStorage = 2 * std::size_t{260};
        constexpr auto perType = sizeof(std::pair<const std::string_view, Type>) + nodeLinks
            + sizeof(std::pair<const std::string_view, std::vector<Instance>>) + nodeLinks
            + 2 * (sizeof(std::string) + sizeof(ArchiveExactMember) + 2 * sizeof(Dependency))
            + sizeof(std::size_t) + 12 * stringStorage;
        reserve(types.size(), perType);
        // vector按至多双倍容量计费；重复实例仍必须完整参与，不能只按rawcode计一次。
        reserve(placements.size() + (afterPlacements ? afterPlacements->size() : 0), 2 * sizeof(Instance));
        const auto width = std::size_t{header.width - 1} * 4;
        const auto height = std::size_t{header.height - 1} * 4;
        reserve(width, height);
        const auto cells = width * height;
        nativeStaticAttributes_.assign(cells, 0);
        reserve(nativeStaticAttributes_.capacity() - cells, 1);
        if (afterPlacements) {
            reserve(width, height);
            nativeAfterStaticAttributes_.emplace(cells, 0);
            reserve(nativeAfterStaticAttributes_->capacity() - cells, 1);
        }

        std::map<std::string_view, Type, std::less<>> byRawcode;
        std::map<std::string_view, std::vector<Instance>, std::less<>> groups;
        for (const auto& type : types) {
            if (type.rawcode.size() != 4
                || (type.gridPolicy != TerrainDoodadGridPolicy::TwoCells
                    && type.gridPolicy != TerrainDoodadGridPolicy::StartupCategoryC)
                || !byRawcode.emplace(type.rawcode, Type{&type, FootprintPath(type.pathTexture, true)}).second)
                Fail("TERRAIN_NATIVE_FOOTPRINT_LAYOUT", "占地类型没有唯一rawcode或属于尚未捕获的吸附类别。");
            if (type.contribution != TerrainNativeFootprintContribution::StaticRgbAttributes
                    && type.contribution != TerrainNativeFootprintContribution::WalkableTerrainSuppression)
                Fail("TERRAIN_NATIVE_FOOTPRINT_LAYOUT", "占地贡献类别未经同次来源解释。");
        }
        const auto append = [&](std::span<const DoodadPlacement> stage, bool after) {
            for (const auto& placement : stage) {
                const auto found = byRawcode.find(placement.rawcode);
                if (found == byRawcode.end()) continue; // 已由同次严格分类器解释的零贡献类型。
                if (found->second.value->contribution == TerrainNativeFootprintContribution::WalkableTerrainSuppression
                    && (placement.flags != 2 || placement.lifePercent != 100))
                    Fail("TERRAIN_WALKABLE_DESTRUCTABLE_STATE", "可行走对象仅支持wire flags=2且lifePercent=100的完整生存实例："
                        + placement.rawcode + " #" + std::to_string(placement.creationId));
                found->second.used = true;
                groups[found->second.path].push_back({&placement, found->second.value, after});
            }
        };
        append(placements, false);
        if (afterPlacements) append(*afterPlacements, true);
        for (const auto& [rawcode, type] : byRawcode)
            if (!type.used) Fail("TERRAIN_NATIVE_FOOTPRINT_LAYOUT", "占地类型没有对应的实际摆放：" + std::string(rawcode));
        std::vector<std::string> names;
        names.reserve(groups.size());
        for (const auto& [path, instances] : groups) { (void)instances; names.emplace_back(path); }
        std::vector<std::string> fallbackNames;
        std::vector<std::size_t> fallbackIndices;
        fallbackNames.reserve(groups.size()); fallbackIndices.reserve(groups.size());
        std::size_t pixelWrites = 0;
        const auto project = [&](std::size_t index, std::string_view actualName,
            std::vector<std::byte>&& bytes, bool blp) {
                // 原始成员已计sourceBytes_且仍存活；只把同次剩余额度交给
                // RLE或BLP的分配前预检。两阶段所有实例共享同一解码结果。
                reserve(0, 1);
                const auto remaining = capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_;
                auto parsed = blp ? DecodeDoodadFootprintBlpBudgeted(bytes, remaining)
                                  : DecodeDoodadFootprintTgaBudgeted(bytes, remaining);
                if (!parsed.Succeeded() || !parsed.value) {
                    for (auto& diagnostic : parsed.diagnostics) diagnostic.message = std::string(actualName) + "：" + diagnostic.message;
                    throw Failure{std::move(parsed.diagnostics)};
                }
                // 按各张图累计预留，不因组末scratch释放而循环复用额度。
                reserve(parsed.value->decodedBytes, 1);
                const auto& footprint = parsed.value->footprint;
                const auto& instances = groups.at(names[index]);
                const auto pixels = std::size_t{footprint.width} * footprint.height;
                // 内存额度不代表运算额度。即使全部实例重叠，也先按完整矩形限制
                // 像素工作，避免小DOO借256²贴图在保存线程产生无界写入。
                if (pixelWrites > capturedMaximumBytes_
                    || instances.size() > (capturedMaximumBytes_ - pixelWrites) / pixels)
                    Fail("TERRAIN_NATIVE_FOOTPRINT_WORK", "占地实例的累计像素写入超过本次保存工作上限。");
                pixelWrites += instances.size() * pixels;
                for (const auto& instance : instances) {
                    const auto snapCells = instance.type->gridPolicy == TerrainDoodadGridPolicy::StartupCategoryC
                        ? nativeCategoryDirectory_->CategoryCSnapCells() : std::uint8_t{2};
                    auto& attributes = instance.after ? *nativeAfterStaticAttributes_ : nativeStaticAttributes_;
                    auto status = AddDoodadFootprint(attributes, header,
                        *instance.placement, footprint, snapCells, instance.type->fixedRotationDegrees,
                        instance.type->contribution);
                    for (auto& diagnostic : status.diagnostics) diagnostic.message = instance.placement->rawcode
                        + " #" + std::to_string(instance.placement->creationId) + " " + std::string(actualName) + "：" + diagnostic.message;
                    Check(std::move(status));
                }
        };
        // 先在地图和游戏中完整查找TGA。只有已证缺失才回退BLP；坏TGA或
        // 暂不支持的TGA保持明确拒绝，不用回退掩盖格式问题。
        Check(CaptureResourceMembers(reader, names, 0,
            [&](std::size_t index, bool present, std::vector<std::byte>&& bytes) {
                if (present) { project(index, names[index], std::move(bytes), false); return; }
                auto fallback = names[index]; fallback.replace(fallback.size() - 4, 4, ".blp");
                fallbackNames.push_back(std::move(fallback)); fallbackIndices.push_back(index);
            }));
        if (!fallbackNames.empty()) Check(CaptureResourceMembers(reader, fallbackNames, fallbackNames.size(),
            [&](std::size_t index, bool present, std::vector<std::byte>&& bytes) {
                if (!present) Fail("TERRAIN_NATIVE_FOOTPRINT_MISSING", "缺少实际占地贴图：" + fallbackNames[index]);
                project(fallbackIndices[index], fallbackNames[index], std::move(bytes), true);
            }));
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Status::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "非空占地捕获分配失败，未建立部分保存资格。");
    } catch (const std::length_error&) {
        return Status::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "非空占地捕获超过容器容量。");
    }
}

Status TerrainPathingResources::CaptureSpecialFootprints(const MemberReader& reader,
    const TerrainGridSnapshot& terrain,
    std::span<const SpecialDoodadPlacement> placements, std::span<const TerrainNativeDoodadPathingType> types,
    std::uint8_t maximumCliffLevel, std::size_t stage) {
    try {
        const auto reserve = [&](std::size_t count, std::size_t each) {
            if (sourceBytes_ > capturedMaximumBytes_ || objectWorkBytes_ > capturedMaximumBytes_ - sourceBytes_
                || count > (capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_) / each)
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "特殊地形捕获超过累计剩余预算。");
            objectWorkBytes_ += count * each;
        };
        struct Type { std::string path; };
        struct Image { std::vector<std::byte> bytes; TerrainDoodadFootprint decoded; };
        if (types.empty() || stage > 1 || terrain.header.width < 2 || terrain.header.height < 2
            || terrain.header.width > 2049 || terrain.header.height > 2049)
            Fail("TERRAIN_SPECIAL_LAYOUT", "特殊地形类型必须覆盖同次DOO的全部记录。");
        constexpr auto links = 4 * sizeof(void*);
        constexpr auto perType = sizeof(std::pair<const std::string_view, Type>) + links
            + sizeof(std::pair<const std::string, Image>) + links + sizeof(std::string)
            + sizeof(ArchiveExactMember) + 2 * sizeof(Dependency) + 6 * 2 * std::size_t{260};
        reserve(types.size(), perType);
        reserve(placements.size(), sizeof(TerrainSpecialDoodadPathingInput));
        const auto width = std::size_t{terrain.header.width - 1} * 4;
        const auto height = std::size_t{terrain.header.height - 1} * 4;
        if (!waterBrushRules_) reserve(width, height);
        std::map<std::string_view, Type, std::less<>> byRawcode;
        std::map<std::string, Image, std::less<>> images;
        for (const auto& type : types) {
            if (type.rawcode.size() != 4 || type.gridPolicy != TerrainDoodadGridPolicy::SpecialTerrain || type.fixedRotationDegrees
                || type.contribution != TerrainNativeFootprintContribution::StaticRgbAttributes)
                Fail("TERRAIN_SPECIAL_LAYOUT", "特殊地形类型不能带普通装饰物旋转或吸附规则。");
            const auto path = type.pathTexture.empty() ? std::string{} : FootprintPath(type.pathTexture);
            if (!byRawcode.emplace(type.rawcode, Type{path}).second)
                Fail("TERRAIN_SPECIAL_LAYOUT", "特殊地形类型rawcode重复。");
            if (!path.empty()) images.try_emplace(path);
        }
        std::vector<std::string> names; names.reserve(images.size());
        for (const auto& [path, image] : images) { (void)image; names.push_back(path); }
        Check(CaptureResourceMembers(reader, names, names.size(),
            [&](std::size_t index, bool present, std::vector<std::byte>&& bytes) {
                if (!present) Fail("TERRAIN_NATIVE_FOOTPRINT_MISSING", "缺少特殊地形实际占地贴图：" + names[index]);
                auto& image = images.at(names[index]);
                image.decoded = Take(DecodeDoodadFootprintTga(bytes));
                // 源字节额度已由CaptureResourceMembers预留，移入稳定map节点保活借用像素。
                image.bytes = std::move(bytes);
            }));
        std::vector<TerrainSpecialDoodadPathingInput> inputs; inputs.reserve(placements.size());
        for (const auto& placement : placements) {
            const auto found = byRawcode.find(placement.rawcode);
            if (found == byRawcode.end()) Fail("TERRAIN_SPECIAL_LAYOUT", "特殊DOO有未解释的rawcode：" + placement.rawcode);
            inputs.push_back({&placement, found->second.path.empty() ? std::nullopt
                : std::optional{images.at(found->second.path).decoded}});
        }
        if (waterBrushRules_) {
            const auto remaining = capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_;
            waterSpecialShapes_ = Take(DescribeTerrainSpecialDoodads(terrain, inputs, maximumCliffLevel, remaining));
            reserve(waterSpecialShapes_.capacity(), sizeof(TerrainWaterSpecialDoodadShape));
            for (const auto& shape : waterSpecialShapes_) reserve(shape.relativeCliffLevels.capacity() + shape.rawcode.capacity() + 1, 1);
        } else {
            nativeSpecialAttributes_[stage] = Take(BuildTerrainSpecialDoodadPathing(terrain, terrain, inputs, maximumCliffLevel, capturedMaximumBytes_));
            reserve(nativeSpecialAttributes_[stage].capacity() - width * height, 1);
            nativeSpecialCount_[stage] = placements.size();
        }
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) { return Status::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "特殊地形捕获分配失败。"); }
    catch (const std::length_error&) { return Status::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "特殊地形捕获超过容量。"); }
}

} // namespace War3Map::MapData
