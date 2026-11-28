#include "PlacementImportBounds.h"
#include "War3MapCore/Render/MdxDeclaredExtents.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace War3Map::Services::Detail {
namespace {
constexpr std::string_view kDomain = "Placement.Import";
using Box = std::array<double, 6>; // 最小 xyz、最大 xyz
using Point = std::array<double, 3>;
using MapData::PlacementManifestKind;
constexpr double kInfinity = std::numeric_limits<double>::infinity();
constexpr Box kEmpty{kInfinity, kInfinity, kInfinity, -kInfinity, -kInfinity, -kInfinity};

Status Fail(const char* code, std::string message) { return Status::Failure(std::string(kDomain), code, std::move(message)); }

std::string Lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

void Include(Box& box, const Point& point) {
    for (unsigned a = 0; a < 3; ++a) { box[a] = std::min(box[a], point[a]); box[a + 3] = std::max(box[a + 3], point[a]); }
}
void Include(Box& box, const Box& other) {
    for (unsigned a = 0; a < 3; ++a) { box[a] = std::min(box[a], other[a]); box[a + 3] = std::max(box[a + 3], other[a + 3]); }
}

// 游戏的摆放变换：世界 = 平移 + 沿世界轴缩放 · 绕 Z 旋转 · 局部（T·S·R），与场景构建同一顺序。
struct Placement {
    Point position;
    double cosine, sine;
    Point scale;
    explicit Placement(const PlacedDoodad& doodad)
        : position(doodad.position), cosine(std::cos(double(doodad.rotation))), sine(std::sin(double(doodad.rotation))),
          scale{doodad.scale[0], doodad.scale[1], doodad.scale[2]} {}
    [[nodiscard]] Point ToWorld(const Point& local) const {
        const double x = cosine * local[0] - sine * local[1], y = sine * local[0] + cosine * local[1];
        return {position[0] + scale[0] * x, position[1] + scale[1] * y, position[2] + scale[2] * local[2]};
    }
    [[nodiscard]] Point ToLocal(const Point& world) const {
        const double x = (world[0] - position[0]) / scale[0], y = (world[1] - position[1]) / scale[1];
        return {cosine * x + sine * y, -sine * x + cosine * y, (world[2] - position[2]) / scale[2]};
    }
};

// 盒的 8 个角经线性变换后的外包盒（线性变换下即为精确外包）。
template<class Map> Box MapCorners(const Box& box, Map&& map) {
    Box out = kEmpty;
    for (unsigned corner = 0; corner < 8; ++corner)
        Include(out, map(Point{box[corner & 1 ? 3 : 0], box[corner & 2 ? 4 : 1], box[corner & 4 ? 5 : 2]}));
    return out;
}

// 范围包住观察点立方体的判定容差（世界单位）：写明范围是 float，旋转后的换算有舍入。
constexpr double kClearanceTolerance = 1e-2;

// P 在一组局部范围里的深度：以 P 为中心、半边长 d 的世界轴向立方体按摆放换到局部坐标后，
// 仍在每个范围里的最大 d。局部第 k 轴上立方体伸展 d·Σ|M_kj|（M 为世界到局部的线性部分）。P 在范围外时为负。
double Clearance(const std::vector<Box>& boxes, const PlacedDoodad& doodad, const Point& point) {
    const auto& scale = doodad.scale;
    if (std::any_of(scale.begin(), scale.end(), [](float value) { return !std::isfinite(value) || value == 0; }))
        return -kInfinity;
    const Placement placement(doodad);
    const auto center = placement.ToLocal(point);
    const double c = std::abs(placement.cosine), s = std::abs(placement.sine);
    const double ix = 1 / std::abs(placement.scale[0]), iy = 1 / std::abs(placement.scale[1]);
    const Point reach{c * ix + s * iy, s * ix + c * iy, 1 / std::abs(placement.scale[2])};
    double out = kInfinity;
    for (const auto& box : boxes)
        for (unsigned a = 0; a < 3; ++a) out = std::min(out, std::min(center[a] - box[a], box[a + 3] - center[a]) / reach[a]);
    return std::isnan(out) ? -kInfinity : out;
}

Box ExtentBox(const Render::MdxDeclaredExtent& extent) {
    return {extent.minimum[0], extent.minimum[1], extent.minimum[2], extent.maximum[0], extent.maximum[1], extent.maximum[2]};
}

struct ModelState {
    std::size_t file = 0;
    PlacementImportBoundsModel report;
    Box local = kEmpty;              // 全部写明范围的并集（模型局部坐标）
    std::vector<Box> visibility;     // MODL 与每条 SEQS 范围：每条都须包住观察点立方体
    std::vector<std::size_t> placed; // placed 下标
    bool flagged = false;
};

Diagnostic Warning(const char* code, std::string message, const std::string& modelPath) {
    return Diagnostic{DiagnosticSeverity::Warning, std::string(kDomain), code, std::move(message),
        OriginSpan{std::filesystem::path(std::u8string(modelPath.begin(), modelPath.end())), 0, 0, {}}};
}

std::string InstanceList(const std::vector<const PlacementImportBoundsFinding*>& findings, bool distances) {
    std::string out;
    for (std::size_t i = 0; i < findings.size() && i < PlacementImportReportedBoundsInstances; ++i) {
        out += (i ? "、#" : "#") + std::to_string(findings[i]->instance);
        if (distances) out += std::format("（{:.0f}）", findings[i]->clearance);
    }
    if (findings.size() > PlacementImportReportedBoundsInstances) out += " 等";
    return out;
}
} // namespace

Status CheckPlacementImportBounds(const MapData::PlacementManifest& manifest, const PlacementImportInputs& inputs,
    const std::vector<PlacedDoodad>& placed, PlacementImportPlan& plan) {
    const auto& header = inputs.terrain;
    const double west = header.offset[0], south = header.offset[1];
    const double east = west + 128.0 * (header.width - 1), north = south + 128.0 * (header.height - 1);
    const auto& options = inputs.bounds;
    auto& bounds = plan.report.bounds;
    bounds.standpoint = options.standpoint;
    bounds.margin = options.margin;
    bounds.widenRequested = options.widen;
    if (!std::isfinite(options.margin) || options.margin <= 0 || options.margin > PlacementImportMaximumStandpointMargin)
        return Fail("PLACEMENT_IMPORT_BOUNDS_OPTIONS",
            std::format("观察点余量须大于 0 且不超过 {:g}（收到 {:g}）。", PlacementImportMaximumStandpointMargin, options.margin));
    if (const auto& p = options.standpoint) {
        const auto& point = *p;
        if (!std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])
            || std::abs(point[2]) > PlacementImportMaximumStandpointMargin
            || point[0] < west || point[0] > east || point[1] < south || point[1] > north)
            return Fail("PLACEMENT_IMPORT_BOUNDS_OPTIONS", std::format("观察点 ({:g}, {:g}, {:g}) 须在地图范围 x {:g}…{:g}、"
                "y {:g}…{:g} 内，z 的绝对值不超过 {:g}。", point[0], point[1], point[2], west, east, south, north,
                PlacementImportMaximumStandpointMargin));
    }
    if (options.widen && !options.standpoint)
        return Fail("PLACEMENT_IMPORT_BOUNDS_OPTIONS", "加宽模型范围需要给出观察点。");

    // 装饰物类型用到的清单内模型文件，按 files 顺序建立；多个类型可共用一个文件。游戏按 .mdx 读取写成 .mdl 的路径。
    std::unordered_map<std::string, std::size_t> fileByPath;
    for (std::size_t file = 0; file < manifest.files.size(); ++file) fileByPath.emplace(Lower(manifest.files[file].archivePath), file);
    std::vector<std::optional<std::size_t>> typeFile(manifest.types.size());
    std::vector<char> fileUsed(manifest.files.size(), 0);
    for (std::size_t type = 0; type < manifest.types.size(); ++type) {
        if (manifest.types[type].kind != PlacementManifestKind::Doodad) continue;
        auto key = Lower(manifest.types[type].model);
        auto found = fileByPath.find(key);
        if (found == fileByPath.end() && key.ends_with(".mdl")) { key.back() = 'x'; found = fileByPath.find(key); }
        if (found == fileByPath.end()) continue;
        typeFile[type] = found->second;
        fileUsed[found->second] = 1;
    }
    std::vector<ModelState> models;
    std::vector<std::size_t> modelOfFile(manifest.files.size(), SIZE_MAX);
    for (std::size_t file = 0; file < manifest.files.size(); ++file) {
        if (!fileUsed[file]) continue;
        modelOfFile[file] = models.size();
        ModelState model;
        model.file = file;
        model.report.archivePath = manifest.files[file].archivePath;
        auto read = Render::ReadMdxDeclaredExtents(manifest.files[file].bytes, Render::MdxExtentScope::All);
        if (!read.Succeeded() || !read.value) {
            model.report.uncheckedReason = read.diagnostics.empty() ? std::string("写明范围读取失败。") : read.diagnostics.front().message;
        } else {
            bool usable = true;
            const auto include = [&](const Render::MdxDeclaredExtent& extent) {
                usable = usable && Render::MdxExtentUsable(extent);
                if (usable) Include(model.local, ExtentBox(extent));
            };
            include(read.value->model);
            for (const auto* list : {&read.value->sequences, &read.value->geosets, &read.value->geosetSequences})
                for (const auto& extent : *list) include(extent);
            if (usable) {
                model.report.checked = true;
                model.visibility.push_back(ExtentBox(read.value->model));
                for (const auto& extent : read.value->sequences) model.visibility.push_back(ExtentBox(extent));
            } else {
                model.local = kEmpty; model.report.uncheckedReason = "写明范围含无效数值（非有限、半径为负或最小大于最大）。";
            }
        }
        models.push_back(std::move(model));
    }
    for (std::size_t type = 0; type < manifest.types.size(); ++type)
        if (typeFile[type]) plan.report.types[type].boundsChecked = models[modelOfFile[*typeFile[type]]].report.checked;

    for (std::size_t index = 0; index < placed.size(); ++index) {
        const auto& doodad = placed[index];
        if (!typeFile[doodad.type] || !models[modelOfFile[*typeFile[doodad.type]]].report.checked) {
            ++bounds.uncheckedInstances;
            continue;
        }
        auto& model = models[modelOfFile[*typeFile[doodad.type]]];
        auto& type = plan.report.types[doodad.type];
        ++bounds.checkedInstances;
        ++model.report.placements;
        model.placed.push_back(index);
        const Placement placement(doodad);
        const auto box = MapCorners(model.local, [&](const Point& local) { return placement.ToWorld(local); });
        const bool offMap = box[3] < west || box[0] > east || box[4] < south || box[1] > north;
        const double clearance = options.standpoint ? Clearance(model.visibility, doodad, *options.standpoint) : 0.0;
        const bool distant = options.standpoint && clearance < options.margin - kClearanceTolerance;
        if (!offMap && !distant) continue;
        model.flagged = true;
        if (offMap) ++model.report.offMap;
        if (distant) ++model.report.farFromStandpoint;
        const auto problem = offMap && distant ? PlacementBoundsProblem::Both
            : offMap ? PlacementBoundsProblem::OffMap : PlacementBoundsProblem::FarFromStandpoint;
        auto& counter = offMap && distant ? bounds.both : offMap ? bounds.offMap : bounds.farFromStandpoint;
        auto& typeCounter = offMap && distant ? type.both : offMap ? type.offMap : type.farFromStandpoint;
        ++counter;
        ++typeCounter;
        bounds.findings.push_back({doodad.instance, doodad.type, problem, box, clearance});
    }
    bounds.uncheckedInstances += plan.units.size();

    // 加宽：每个有问题的模型取它全部摆放处的 P±margin（换到各自的局部坐标）的并集，只改导入副本的范围字段。
    if (options.widen) {
        const auto& p = *options.standpoint;
        const double m = options.margin;
        const Box cube{p[0] - m, p[1] - m, p[2] - m, p[0] + m, p[1] + m, p[2] + m};
        for (auto& model : models) {
            if (!model.flagged) continue;
            Box required = kEmpty;
            for (const auto index : model.placed) {
                const Placement placement(placed[index]);
                Include(required, MapCorners(cube, [&](const Point& world) { return placement.ToLocal(world); }));
            }
            auto widened = Render::WidenMdxDeclaredExtents(manifest.files[model.file].bytes,
                {required[0], required[1], required[2]}, {required[3], required[4], required[5]});
            if (!widened.Succeeded() || !widened.value)
                return Fail("PLACEMENT_IMPORT_BOUNDS_WIDEN", "模型 " + model.report.archivePath + " 不能加宽写明范围："
                    + (widened.diagnostics.empty() ? std::string("未知原因") : widened.diagnostics.front().message) + "整份未导入。");
            if (!widened.value->changed) continue;
            model.report.widenedExtents = widened.value->changed;
            ++bounds.widenedModels;
            plan.fileRewrites.push_back({model.file, std::move(widened.value->bytes)});
        }
    }

    // 提醒：每个类型分“地图外”和“未包住观察点”各一条；每个加宽的模型一条；清单内读不了范围的模型一条。
    std::vector<std::vector<const PlacementImportBoundsFinding*>> offMapByType(manifest.types.size()), farByType(manifest.types.size());
    for (const auto& finding : bounds.findings) {
        if (finding.problem != PlacementBoundsProblem::FarFromStandpoint) offMapByType[finding.type].push_back(&finding);
        if (finding.problem != PlacementBoundsProblem::OffMap) farByType[finding.type].push_back(&finding);
    }
    for (std::size_t type = 0; type < manifest.types.size(); ++type) {
        if (offMapByType[type].empty() && farByType[type].empty()) continue;
        const auto& model = models[modelOfFile[*typeFile[type]]];
        const auto prefix = "类型 " + manifest.types[type].key + "（模型 " + model.report.archivePath + "）：";
        const std::string widened = model.report.widenedExtents ? "导入的副本已加宽。" : "";
        if (!offMapByType[type].empty())
            plan.report.warnings.push_back(Warning("PLACEMENT_IMPORT_BOUNDS_OFF_MAP", prefix + std::format(
                "{} 个摆放的模型写明范围整个在地图 x {:g}…{:g}、y {:g}…{:g} 之外，游戏里不会绘制：{}。{}",
                offMapByType[type].size(), west, east, south, north, InstanceList(offMapByType[type], false), widened),
                model.report.archivePath));
        if (!farByType[type].empty()) {
            const auto& point = *options.standpoint;
            plan.report.warnings.push_back(Warning("PLACEMENT_IMPORT_BOUNDS_FAR", prefix + std::format(
                "{} 个摆放的模型写明范围没有包住观察点 ({:g}, {:g}, {:g}) ±{:g}，游戏里有被剔掉的风险"
                "（括号内为观察点在范围里的深度，负值是在范围外的距离）：{}。{}",
                farByType[type].size(), point[0], point[1], point[2], options.margin, InstanceList(farByType[type], true), widened),
                model.report.archivePath));
        }
    }
    for (const auto& model : models) {
        if (model.report.widenedExtents)
            plan.report.warnings.push_back(Warning("PLACEMENT_IMPORT_BOUNDS_WIDENED", std::format(
                "模型 {}：已在导入的副本里加宽 {} 条写明范围（MODL/SEQS/GEOS），{} 个摆放处都包含观察点 ±{:g}；源文件未改。",
                model.report.archivePath, model.report.widenedExtents, model.report.placements, options.margin), model.report.archivePath));
        else if (!model.report.checked)
            plan.report.warnings.push_back(Warning("PLACEMENT_IMPORT_BOUNDS_UNCHECKED", "模型 " + model.report.archivePath
                + "：" + model.report.uncheckedReason + "未检查写明范围。", model.report.archivePath));
        bounds.models.push_back(model.report);
    }
    return Status::Ok();
}

} // namespace War3Map::Services::Detail
