#include "War3MapCore/MapData/TerrainShadowCaster.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3RenderCore/CompiledModel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace War3Map::MapData {
namespace {

using Vec3 = std::array<float, 3>;
using Output = Result<TerrainShadowCasterProjection>;
struct Triangle { std::array<Vec3, 3> vertices; };
struct Sphere { Vec3 center{}; float radius = 0; };
struct ModelTriangles { std::vector<Triangle> triangles; Sphere sphere; };

Output Reject(const char* code, const char* message) {
    return Output::Failure("MapTerrain.Shadow", code, message);
}

// 共享解析模型不保留SEQS extent；仅从已经完整Parse/Compile过的同份
// 原字节回读。重复顶层块的原版覆盖/追加语义不在本子集内，明确拒绝。
std::optional<Sphere> SourceSphere(std::span<const std::byte> bytes) {
    const auto u32 = [&](std::size_t at) { std::uint32_t value = 0; std::memcpy(&value,bytes.data()+at,4); return value; };
    const auto f32 = [&](std::size_t at) { return std::bit_cast<float>(u32(at)); };
    std::array<std::uint32_t,16> seen{};
    std::size_t seenCount = 0, modl = 0, sequence = 0;
    for (std::size_t at = 4; at < bytes.size();) {
        if (bytes.size() - at < 8 || seenCount == seen.size()) return {};
        const auto tag = u32(at), size = u32(at+4);
        if (size > bytes.size() - at - 8 || std::find(seen.begin(),seen.begin()+seenCount,tag) != seen.begin()+seenCount) return {};
        seen[seenCount++] = tag;
        if (tag == 0x4c444f4du) { if (size != 372) return {}; modl = at+8+340; }
        if (tag == 0x53514553u && size != 0) { if (size < 132 || size % 132 != 0) return {}; sequence = at+8+104; }
        at += 8 + size;
    }
    if (!modl) return {};
    auto extent = modl;
    if (std::abs(f32(modl)) < 0x1p-22 && sequence) extent = sequence;
    Sphere sphere;
    sphere.radius = f32(extent);
    if (!std::isfinite(sphere.radius) || sphere.radius < 0 || sphere.radius > 16777216) return {};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const float minimum = f32(extent+4+axis*4), maximum = f32(extent+16+axis*4);
        if (!std::isfinite(minimum) || !std::isfinite(maximum)
            || std::abs(minimum) > 16777216 || std::abs(maximum) > 16777216) return {};
        const float sum = static_cast<float>(static_cast<double>(minimum) + maximum);
        sphere.center[axis] = static_cast<float>(static_cast<double>(sum) * 0.5);
    }
    return sphere;
}

// 695C20的F32栈存储边界；其余乘加使用double。显式double操作避免
// C++先以float相减/相乘，以及任意front-face或t>=0裁剪改写原规则。
std::optional<float> IntersectSigned(const Vec3& origin, const Vec3& direction, const Triangle& triangle) {
    const auto& a = triangle.vertices[0];
    const auto& b = triangle.vertices[1];
    const auto& c = triangle.vertices[2];
    const double e1x = static_cast<double>(b[0]) - a[0];
    const float e1y = static_cast<float>(static_cast<double>(b[1]) - a[1]);
    const float e1z = static_cast<float>(static_cast<double>(b[2]) - a[2]);
    const float e2x = static_cast<float>(static_cast<double>(c[0]) - a[0]);
    const float e2y = static_cast<float>(static_cast<double>(c[1]) - a[1]);
    const float e2z = static_cast<float>(static_cast<double>(c[2]) - a[2]);
    const double px = static_cast<double>(e2z) * direction[1] - static_cast<double>(e2y) * direction[2];
    const double py = static_cast<double>(e2x) * direction[2] - static_cast<double>(e2z) * direction[0];
    const double pz = static_cast<double>(e2y) * direction[0] - static_cast<double>(e2x) * direction[1];
    const float pzStored = static_cast<float>(pz);
    const double determinant = pz * e1z + py * e1y + px * e1x;
    if (std::abs(determinant) < 0x1p-22) return std::nullopt;
    const float inverse = static_cast<float>(1.0 / determinant);
    const double tx = static_cast<double>(origin[0]) - a[0];
    const double ty = static_cast<double>(origin[1]) - a[1];
    const double tz = static_cast<double>(origin[2]) - a[2];
    const float qx = static_cast<float>(ty * e1z - tz * e1y);
    const float qy = static_cast<float>(tz * e1x - tx * e1z);
    const float qz = static_cast<float>(tx * e1y - ty * e1x);
    const float u = static_cast<float>((tz * pzStored + ty * py + tx * px) * inverse);
    const float v = static_cast<float>((static_cast<double>(qx) * direction[0]
        + static_cast<double>(qz) * direction[2] + static_cast<double>(qy) * direction[1]) * inverse);
    if (u < 0 || u > 1 || v < 0 || v > 1 || static_cast<double>(u) + v > 1) return std::nullopt;
    return static_cast<float>((static_cast<double>(qz) * e2z + static_cast<double>(qy) * e2y
        + static_cast<double>(qx) * e2x) * inverse);
}

// 566380先按32位端点构造长线，再归一化。不能直接把caller方向视为
// 已归一化；每个receiver的F32端点舍入也属于本次实际射线。
Vec3 RayEnd(const Vec3& origin, const Vec3& light) {
    Vec3 end{};
    end[0] = static_cast<float>(1280000.0 * light[0] + origin[0]);
    for (std::size_t axis = 1; axis < 3; ++axis) {
        const float delta = static_cast<float>(1280000.0 * light[axis]);
        end[axis] = static_cast<float>(static_cast<double>(delta) + origin[axis]);
    }
    return end;
}

std::optional<Vec3> RayDirection(const Vec3& origin, const Vec3& end) {
    const double dx = static_cast<double>(end[0]) - origin[0];
    const double dy = static_cast<double>(end[1]) - origin[1];
    const double dz = static_cast<double>(end[2]) - origin[2];
    const float dzStored = static_cast<float>(dz);
    const double squared = dz * dzStored + dy * dy + dx * dx;
    if (!std::isfinite(squared) || squared == 0) return std::nullopt;
    const float inverse = static_cast<float>(1.0 / std::sqrt(squared));
    Vec3 result{static_cast<float>(dx * inverse), static_cast<float>(dy * inverse),
        static_cast<float>(static_cast<double>(dzStored) * inverse)};
    for (const auto value : result) if (!std::isfinite(value)) return std::nullopt;
    return result;
}

// 46F7F0保留dot的F32回读，以及中段Y/Z投影的F32暂存；有限segment
// 的筛选在有符号全三角形min之前执行，不能用无限line代替。
bool SegmentMeetsSphere(const Vec3& origin, const Vec3& end, const Sphere& sphere) {
    double ox = static_cast<double>(sphere.center[0]) - origin[0];
    double oy = static_cast<double>(sphere.center[1]) - origin[1];
    double oz = static_cast<double>(sphere.center[2]) - origin[2];
    const float dx = static_cast<float>(static_cast<double>(end[0]) - origin[0]);
    const float dy = static_cast<float>(static_cast<double>(end[1]) - origin[1]);
    const float dz = static_cast<float>(static_cast<double>(end[2]) - origin[2]);
    const double dot = static_cast<double>(dx) * ox + static_cast<double>(dz) * oz + static_cast<double>(dy) * oy;
    const float dotStored = static_cast<float>(dot);
    if (dot > 0) {
        const double length = static_cast<double>(dz) * dz + static_cast<double>(dy) * dy + static_cast<double>(dx) * dx;
        if (dotStored > length) { ox -= dx; oy -= dy; oz -= dz; }
        else {
            const double ratio = dotStored / length;
            ox -= dx * ratio;
            oy -= static_cast<float>(dy * ratio);
            oz -= static_cast<float>(dz * ratio);
        }
    }
    const double distance = ox * ox + oz * oz + oy * oy;
    return distance <= static_cast<double>(sphere.radius) * sphere.radius;
}

std::optional<std::array<std::int32_t, 2>> BlockDirection(const TerrainShadowCasterFilterInputs& filters,
    const Vec3& light) {
    if (!std::isfinite(filters.maximumTerrainHeight) || !std::isfinite(filters.minimumTerrainHeight)
        || std::abs(filters.maximumTerrainHeight) > 65536 || std::abs(filters.minimumTerrainHeight) > 65536
        || filters.maximumTerrainHeight < filters.minimumTerrainHeight || light[2] == 0) return {};
    const double span = static_cast<double>(filters.maximumTerrainHeight) - filters.minimumTerrainHeight + 384.0;
    const double inverseZ = 1.0 / light[2];
    const float x = static_cast<float>(span * light[0] * inverseZ);
    const double y = inverseZ * (span * light[1]);
    const auto index = [](double value) -> std::optional<std::int32_t> {
        const double divided = value / 512.0;
        // 排除原版int转换溢出域，之后blockX+offset也不会溢出。
        if (!std::isfinite(divided) || std::abs(divided) > 1048576) return {};
        return static_cast<std::int32_t>(divided) + (value > 0 ? 1 : 0);
    };
    const auto bx = index(x), by = index(y);
    if (!bx || !by) return {};
    return std::array<std::int32_t,2>{*bx,*by};
}

bool BlockContainsCaster(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
    const std::array<float,2>& worldOrigin, const std::array<std::int32_t,2>& direction, const Vec3& anchor) {
    const auto nx = static_cast<std::int32_t>((width + 15) / 16);
    const auto ny = static_cast<std::int32_t>((height + 15) / 16);
    const auto bx = static_cast<std::int32_t>(x / 16), by = static_cast<std::int32_t>(y / 16);
    const auto endX = bx + direction[0], endY = bx + direction[1]; // 原版560E98确实加blockX。
    const auto minX = std::max(0, std::min(bx,endX)), maxX = std::min(nx - 1, std::max(bx,endX));
    const auto minY = std::max(0, std::min(by,endY)), maxY = std::min(ny - 1, std::max(by,endY));
    // 相邻block的128 halo重叠，整个矩形的anchor候选域连续；无需遍历
    // 重复block。5E51E0先F32(origin+512)，再F32(+128)。
    const auto lowX = static_cast<float>(static_cast<double>(worldOrigin[0]) + minX * 512.0);
    const auto lowY = static_cast<float>(static_cast<double>(worldOrigin[1]) + minY * 512.0);
    const auto highX = static_cast<float>(static_cast<double>(worldOrigin[0]) + maxX * 512.0);
    const auto highY = static_cast<float>(static_cast<double>(worldOrigin[1]) + maxY * 512.0);
    return anchor[0] >= static_cast<float>(static_cast<double>(lowX) - 128.0)
        && anchor[1] >= static_cast<float>(static_cast<double>(lowY) - 128.0)
        && anchor[0] <= static_cast<float>(static_cast<double>(static_cast<float>(static_cast<double>(highX) + 512.0)) + 128.0)
        && anchor[1] <= static_cast<float>(static_cast<double>(static_cast<float>(static_cast<double>(highY) + 512.0)) + 128.0);
}

} // namespace

static Output ProjectCasters(const TerrainShadowHeightSamples& samples,
    const std::array<float, 2>& worldOrigin, std::span<const TerrainShadowCasterModelInput> models,
    std::span<const TerrainShadowCasterInstanceInput> instances, const TerrainShadowCasterLimits& limits,
    std::stop_token cancellation, const TerrainShadowCasterFilterInputs* filters) {
    const TerrainShadowCasterLimits hard;
    if (limits.maximumDimension > hard.maximumDimension || limits.maximumPixels > hard.maximumPixels
        || limits.maximumSampleBytes > hard.maximumSampleBytes || limits.maximumWork > hard.maximumWork
        || limits.maximumModels > hard.maximumModels || limits.maximumInstances > hard.maximumInstances
        || limits.maximumModelBytes > hard.maximumModelBytes || limits.maximumTriangles > hard.maximumTriangles
        || limits.maximumGeometryBytes > hard.maximumGeometryBytes || limits.maximumCompiledBytes > hard.maximumCompiledBytes)
        return Reject("TERRAIN_SHADOW_CASTER_LIMITS", "静态阴影caster只能收紧硬预算。");
    if (!samples.width || !samples.height || samples.width % 4 || samples.height % 4)
        return Reject("TERRAIN_SHADOW_CASTER_DIMENSIONS", "caster需要每地格四像素的完整receiver网格。");
    if (samples.width > limits.maximumDimension || samples.height > limits.maximumDimension
        || samples.height > limits.maximumPixels / samples.width)
        return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster网格超过尺寸或像素预算。");
    const auto count = static_cast<std::size_t>(samples.width) * samples.height;
    const auto blockDirection = filters ? BlockDirection(*filters, samples.lightDirection)
        : std::optional<std::array<std::int32_t,2>>{};
    if (filters && !blockDirection)
        return Reject("TERRAIN_SHADOW_CASTER_FILTER", "实际地形高度profile或光线不能形成有界原版区块方向。");
    if (count > limits.maximumSampleBytes / sizeof(float))
        return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster借用高度场超过样本预算。");
    if (samples.receiverHeights.size() != count)
        return Reject("TERRAIN_SHADOW_CASTER_HEIGHT_COUNT", "caster必须借用完整高度场，不能补零或截断。");
    for (const auto value : samples.lightDirection) if (!std::isfinite(value))
        return Reject("TERRAIN_SHADOW_CASTER_LIGHT", "caster实际光照方向含非有限数。");
    if (samples.lightDirection == Vec3{})
        return Reject("TERRAIN_SHADOW_CASTER_LIGHT", "caster实际光照方向为零。");
    for (std::size_t axis = 0; axis < 2; ++axis) {
        const double last = static_cast<double>(worldOrigin[axis]) + (axis == 0 ? samples.width : samples.height) * 32.0;
        if (!std::isfinite(worldOrigin[axis]) || std::fmod(worldOrigin[axis], 128.0) != 0
            || worldOrigin[axis] < -32768 || last > 32640)
            return Reject("TERRAIN_SHADOW_CASTER_COORDINATE", "caster世界范围须与经典W3E九位顶点坐标相符。");
    }
    if (models.size() > limits.maximumModels || instances.size() > limits.maximumInstances)
        return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster模型或实例数量超过预算。");
    if (cancellation.stop_requested())
        return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster已取消，未返回部分候选。");

    try {
        TerrainShadowCasterProjection candidate;
        candidate.width = samples.width; candidate.height = samples.height;
        const auto consume = [&](std::size_t amount = 1) {
            if (amount > limits.maximumWork - candidate.work) return false;
            candidate.work += amount;
            return true;
        };
        for (const auto height : samples.receiverHeights) {
            if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster高度检查已取消。");
            if (!consume()) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster高度检查超过工作预算。");
            if (!std::isfinite(height) || std::abs(height) > 65536)
                return Reject("TERRAIN_SHADOW_CASTER_HEIGHT", "caster高度场含非有限数或超出经典高度域。");
        }
        const auto modelHeaderBytes = models.size() * sizeof(ModelTriangles);
        if (modelHeaderBytes > limits.maximumGeometryBytes)
            return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster模型目录超过几何内存预算。");
        const auto triangleCapacity = (limits.maximumGeometryBytes - modelHeaderBytes) / sizeof(Triangle);
        std::vector<ModelTriangles> geometry;
        geometry.reserve(models.size());
        std::size_t inputBytes = 0, maximumModelTriangles = 0;
        for (std::size_t index = 0; index < models.size(); ++index) {
            if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster模型准备已取消。");
            const auto& input = models[index];
            if (input.path.empty() || input.path.size() > 1024 || input.bytes.empty())
                return Reject("TERRAIN_SHADOW_CASTER_MODEL", "caster需要明确模型路径和实际原字节。");
            if (input.bytes.size() > limits.maximumModelBytes - inputBytes || !consume(input.bytes.size()))
                return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster模型字节或解析工作超过预算。");
            inputBytes += input.bytes.size();
            auto parsed = Render::ParseMdx(input.bytes);
            if (!parsed.Succeeded()) return {std::nullopt, std::move(parsed.diagnostics)};
            auto digest = Sha256(input.bytes);
            if (!digest.Succeeded()) return {std::nullopt, std::move(digest.diagnostics)};
            // 独立原字节重新编译，不接受caller的已静态/缓存资格。这里的source
            // 仅为进程内身份；资源winner、SHA及最终修订须由Services捕获证明。
            auto compiled = War3RenderCore::CompileMdxModel(*parsed.value,
                {static_cast<std::uint64_t>(index + 1), 1}, {std::string(input.path), "terrain-shadow-cpu", std::move(*digest.value)},
                limits.maximumCompiledBytes);
            if (!compiled.Succeeded()) return {std::nullopt, std::move(compiled.diagnostics)};
            if (!compiled.value->UsesNoControllerStatic())
                return Reject("TERRAIN_SHADOW_CASTER_DYNAMIC", "模型未通过整模型无控制器静态资格，不能以显示姿态烘焙。");
            std::size_t visibleTriangles = 0;
            for (const auto& geoset : parsed.value->geosets) {
                if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster geoset检查已取消。");
                if (!consume()) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster geoset检查超过工作预算。");
                if (geoset.selectionFlags != 0)
                    return Reject("TERRAIN_SHADOW_CASTER_VISIBILITY", "GEOS原标志与原版内部可见位尚未闭合。");
                if (geoset.animation && geoset.animation->staticAlpha != 0 && geoset.animation->staticAlpha != 1)
                    return Reject("TERRAIN_SHADOW_CASTER_VISIBILITY", "caster只接受已证明静态全显或全隐的GEOA。");
                if (geoset.animation && geoset.animation->staticAlpha == 0) continue;
                if (geoset.indices.size() / 3 > limits.maximumTriangles - visibleTriangles)
                    return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster模型三角形超过预算。");
                visibleTriangles += geoset.indices.size() / 3;
            }
            if (visibleTriangles > limits.maximumTriangles - candidate.triangles)
                return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster全部模型三角形超过预算。");
            candidate.triangles += visibleTriangles;
            maximumModelTriangles = std::max(maximumModelTriangles, visibleTriangles);
            if (candidate.triangles > triangleCapacity || maximumModelTriangles > triangleCapacity - candidate.triangles)
                return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster局部和单实例几何超过内存预算。");
            ModelTriangles model;
            if (filters) {
                if (!consume(input.bytes.size())) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "原始extent回读超过工作预算。");
                const auto sphere = SourceSphere(input.bytes);
                if (!sphere) return Reject("TERRAIN_SHADOW_CASTER_SPHERE", "模型缺少有限MODL/首SEQS sphere或含重复顶层块。");
                model.sphere = *sphere;
            }
            model.triangles.reserve(visibleTriangles);
            for (const auto& geoset : parsed.value->geosets) {
                if (geoset.animation && geoset.animation->staticAlpha == 0) continue;
                for (std::size_t at = 0; at < geoset.indices.size(); at += 3) {
                    if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster几何提取已取消。");
                    if (!consume()) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster几何提取超过工作预算。");
                    model.triangles.push_back({{geoset.positions[geoset.indices[at]],
                        geoset.positions[geoset.indices[at + 1]], geoset.positions[geoset.indices[at + 2]]}});
                }
            }
            geometry.push_back(std::move(model));
        }
        candidate.casterShadowBits.assign(count, 0);
        std::vector<Triangle> world;
        world.reserve(maximumModelTriangles);
        for (const auto& instance : instances) {
            if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster实例准备已取消。");
            if (!consume()) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster实例检查超过工作预算。");
            if (instance.modelIndex >= geometry.size())
                return Reject("TERRAIN_SHADOW_CASTER_INSTANCE", "caster实例指向不存在的模型。");
            for (const auto value : instance.nativeMatrix) if (!std::isfinite(value))
                return Reject("TERRAIN_SHADOW_CASTER_MATRIX", "caster实际世界矩阵含非有限数。");
            Sphere worldSphere;
            const Vec3 anchor{instance.nativeMatrix[3],instance.nativeMatrix[7],instance.nativeMatrix[11]};
            if (filters) {
                for (const auto scale : instance.actualScale) if (!std::isfinite(scale) || scale <= 0 || scale > 65536)
                    return Reject("TERRAIN_SHADOW_CASTER_SCALE", "caster原版sphere需要实际有限正轴scale。");
                const auto& local = geometry[instance.modelIndex].sphere;
                worldSphere.radius = static_cast<float>(static_cast<double>(local.radius)
                    * std::max({instance.actualScale[0],instance.actualScale[1],instance.actualScale[2]}));
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto base = axis * 4;
                    worldSphere.center[axis] = static_cast<float>(static_cast<double>(instance.nativeMatrix[base + 2]) * local.center[2]
                        + static_cast<double>(instance.nativeMatrix[base + 1]) * local.center[1]
                        + static_cast<double>(instance.nativeMatrix[base]) * local.center[0] + instance.nativeMatrix[base + 3]);
                    if (!std::isfinite(worldSphere.center[axis]) || std::abs(worldSphere.center[axis]) > 16777216
                        || std::abs(anchor[axis]) > 16777216 || !std::isfinite(worldSphere.radius))
                        return Reject("TERRAIN_SHADOW_CASTER_SPHERE", "caster变换sphere超出有限有界域。");
                }
            }
            world.clear();
            for (const auto& triangle : geometry[instance.modelIndex].triangles) {
                if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster世界顶点转换已取消。");
                if (!consume(3)) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster世界顶点转换超过工作预算。");
                Triangle transformed{};
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    const auto& p = triangle.vertices[corner];
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const auto base = axis * 4;
                        const float value = static_cast<float>(static_cast<double>(instance.nativeMatrix[base + 2]) * p[2]
                            + static_cast<double>(instance.nativeMatrix[base + 1]) * p[1]
                            + static_cast<double>(instance.nativeMatrix[base]) * p[0] + instance.nativeMatrix[base + 3]);
                        if (!std::isfinite(value) || std::abs(value) > 16777216)
                            return Reject("TERRAIN_SHADOW_CASTER_MATRIX", "caster世界顶点非有限或超出有界F32几何域。");
                        transformed.vertices[corner][axis] = value;
                    }
                }
                world.push_back(transformed);
            }
            if (world.empty()) continue;
            for (std::uint32_t y = 0; y < samples.height; ++y) {
                for (std::uint32_t x = 0; x < samples.width; ++x) {
                    if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster射线已取消。");
                    if (!consume()) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster像素检查超过工作预算。");
                    const auto pixel = static_cast<std::size_t>(y) * samples.width + x;
                    if (candidate.casterShadowBits[pixel]) continue;
                    if (filters && !BlockContainsCaster(x,y,samples.width,samples.height,worldOrigin,*blockDirection,anchor)) continue;
                    const Vec3 origin{static_cast<float>(static_cast<double>(worldOrigin[0]) + x * 32.0 + 16.0),
                        static_cast<float>(static_cast<double>(worldOrigin[1]) + y * 32.0 + 16.0), samples.receiverHeights[pixel]};
                    const auto end = RayEnd(origin, samples.lightDirection);
                    if (filters && !SegmentMeetsSphere(origin,end,worldSphere)) continue;
                    const auto direction = RayDirection(origin, end);
                    if (!direction) return Reject("TERRAIN_SHADOW_CASTER_LIGHT", "caster长线端点不能形成有限非零射线。");
                    float minimum = std::numeric_limits<float>::max();
                    bool hit = false;
                    for (const auto& triangle : world) {
                        if (!consume()) return Reject("TERRAIN_SHADOW_CASTER_BUDGET", "caster三角形射线超过工作预算。");
                        const auto distance = IntersectSigned(origin, *direction, triangle);
                        if (!distance) continue;
                        if (!std::isfinite(*distance))
                            return Reject("TERRAIN_SHADOW_CASTER_INTERSECTION", "caster交距不能表示为有限F32。");
                        hit = true;
                        minimum = std::min(minimum, *distance);
                    }
                    if (hit && minimum > 0) candidate.casterShadowBits[pixel] = 1;
                }
            }
        }
        if (cancellation.stop_requested()) return Reject("TERRAIN_SHADOW_CASTER_CANCELLED", "caster完成前取消，未返回候选。");
        return Output::Success(std::move(candidate));
    } catch (const std::bad_alloc&) {
        return Reject("TERRAIN_SHADOW_CASTER_ALLOCATION", "caster分配失败，未返回部分候选。");
    } catch (const std::length_error&) {
        return Reject("TERRAIN_SHADOW_CASTER_ALLOCATION", "caster容量不足，未返回部分候选。");
    }
}

Output ProjectStaticTerrainShadowCasters(const TerrainShadowHeightSamples& samples,
    const std::array<float,2>& worldOrigin, std::span<const TerrainShadowCasterModelInput> models,
    std::span<const TerrainShadowCasterInstanceInput> instances, const TerrainShadowCasterLimits& limits,
    std::stop_token cancellation) {
    return ProjectCasters(samples,worldOrigin,models,instances,limits,cancellation,nullptr);
}

Output ProjectFilteredStaticTerrainShadowCasters(const TerrainShadowHeightSamples& samples,
    const std::array<float,2>& worldOrigin, std::span<const TerrainShadowCasterModelInput> models,
    std::span<const TerrainShadowCasterInstanceInput> instances, const TerrainShadowCasterFilterInputs& filters,
    const TerrainShadowCasterLimits& limits, std::stop_token cancellation) {
    return ProjectCasters(samples,worldOrigin,models,instances,limits,cancellation,&filters);
}

} // namespace War3Map::MapData
