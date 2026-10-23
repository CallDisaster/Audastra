#include "War3MapCore/MapData/TerrainPathingResources.h"

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/TerrainDoodadFootprint.h"
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
std::string FootprintPath(std::string_view path) {
    if (path.empty() || path.size() >= 260)
        Fail("TERRAIN_NATIVE_FOOTPRINT_PATH", "占地贴图路径为空或超出经典路径容量。");
    auto result = Text::LowerAscii(path);
    std::replace(result.begin(), result.end(), '\\', '/');
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
    std::span<const TerrainNativeDoodadPathingType> types) {
    try {
        if (types.empty()) return Status::Ok();
        const auto reserve = [&](std::size_t count, std::size_t each) {
            if (sourceBytes_ > capturedMaximumBytes_ || objectWorkBytes_ > capturedMaximumBytes_ - sourceBytes_
                || count > (capturedMaximumBytes_ - sourceBytes_ - objectWorkBytes_) / each)
                Fail("TERRAIN_NATIVE_OBJECT_BUDGET", "非空占地工作超过同次捕获剩余预算。");
            objectWorkBytes_ += count * each;
        };
        if (header.width < 2 || header.height < 2 || header.width > 2049 || header.height > 2049
            || types.size() > placements.size())
            Fail("TERRAIN_NATIVE_FOOTPRINT_LAYOUT", "占地输入需要完整经典地形尺寸及实际摆放类型集合。");
        struct Type { const TerrainNativeDoodadPathingType* value; std::string path; bool used = false; };
        struct Instance { const DoodadPlacement* placement; const TerrainNativeDoodadPathingType* type; };
        // 类型解析已预留自身输出；这里另计两棵树的节点、名称列表、精确读取结果、
        // 依赖及其增长容量。字符串按最大经典路径的双倍容量预留六份：归一化、
        // 名称、reader名称、canonical临时、依赖名称和归档/SHA；分组key只借用path。
        constexpr auto nodeLinks = 4 * sizeof(void*);
        constexpr auto stringStorage = 2 * std::size_t{260};
        constexpr auto perType = sizeof(std::pair<const std::string_view, Type>) + nodeLinks
            + sizeof(std::pair<const std::string_view, std::vector<Instance>>) + nodeLinks
            + sizeof(std::string) + sizeof(ArchiveExactMember) + 2 * sizeof(Dependency)
            + 6 * stringStorage;
        reserve(types.size(), perType);
        // vector按至多双倍容量计费；重复实例仍必须完整参与，不能只按rawcode计一次。
        reserve(placements.size(), 2 * sizeof(Instance));
        const auto width = std::size_t{header.width - 1} * 4;
        const auto height = std::size_t{header.height - 1} * 4;
        reserve(width, height);
        const auto cells = width * height;
        nativeStaticAttributes_.assign(cells, 0);
        reserve(nativeStaticAttributes_.capacity() - cells, 1);

        std::map<std::string_view, Type, std::less<>> byRawcode;
        std::map<std::string_view, std::vector<Instance>, std::less<>> groups;
        for (const auto& type : types) {
            // 当前typed owner只产出明确非C类别；4格原语须等类别目录来源接通。
            if (type.rawcode.size() != 4 || type.snapCells != 2
                || !byRawcode.emplace(type.rawcode, Type{&type, FootprintPath(type.pathTexture)}).second)
                Fail("TERRAIN_NATIVE_FOOTPRINT_LAYOUT", "占地类型没有唯一rawcode或属于尚未捕获的吸附类别。");
        }
        for (const auto& placement : placements) {
            const auto found = byRawcode.find(placement.rawcode);
            if (found == byRawcode.end()) continue; // 已由同次严格分类器解释的零贡献类型。
            found->second.used = true;
            groups[found->second.path].push_back({&placement, found->second.value});
        }
        for (const auto& [rawcode, type] : byRawcode)
            if (!type.used) Fail("TERRAIN_NATIVE_FOOTPRINT_LAYOUT", "占地类型没有对应的实际摆放：" + std::string(rawcode));
        std::vector<std::string> names;
        names.reserve(groups.size());
        for (const auto& [path, instances] : groups) { (void)instances; names.emplace_back(path); }
        std::size_t pixelWrites = 0;
        // 沿原owner读取map优先的实际TGA；缺失、坏图和来源歧义均拒绝。
        // 每张只读取/扫描一次，多个rawcode和所有重复摆放均共享该次像素解释。
        Check(CaptureResourceMembers(reader, names, names.size(),
            [&](std::size_t index, bool present, std::vector<std::byte>&& bytes) {
                if (!present) Fail("TERRAIN_NATIVE_FOOTPRINT_MISSING", "缺少实际占地贴图：" + names[index]);
                auto parsed = DecodeDoodadFootprintTga(bytes);
                if (!parsed.Succeeded() || !parsed.value) {
                    for (auto& diagnostic : parsed.diagnostics) diagnostic.message = names[index] + "：" + diagnostic.message;
                    throw Failure{std::move(parsed.diagnostics)};
                }
                const auto& instances = groups.at(names[index]);
                const auto pixels = std::size_t{parsed.value->width} * parsed.value->height;
                // 内存额度不代表运算额度。即使全部实例重叠，也先按完整矩形限制
                // 像素工作，避免小DOO借256²贴图在保存线程产生无界写入。
                if (pixelWrites > capturedMaximumBytes_
                    || instances.size() > (capturedMaximumBytes_ - pixelWrites) / pixels)
                    Fail("TERRAIN_NATIVE_FOOTPRINT_WORK", "占地实例的累计像素写入超过本次保存工作上限。");
                pixelWrites += instances.size() * pixels;
                for (const auto& instance : instances) {
                    auto status = AddDoodadFootprint(nativeStaticAttributes_, header,
                        *instance.placement, *parsed.value, instance.type->snapCells, instance.type->fixedRotationDegrees);
                    for (auto& diagnostic : status.diagnostics) diagnostic.message = instance.placement->rawcode
                        + " #" + std::to_string(instance.placement->creationId) + " " + names[index] + "：" + diagnostic.message;
                    Check(std::move(status));
                }
            }));
        return Status::Ok();
    } catch (Failure& failure) { return {std::move(failure.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Status::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "非空占地捕获分配失败，未建立部分保存资格。");
    } catch (const std::length_error&) {
        return Status::Failure(kSubsystem, "TERRAIN_NATIVE_OBJECT_BUDGET", "非空占地捕获超过容器容量。");
    }
}

} // namespace War3Map::MapData
