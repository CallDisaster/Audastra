#include "War3MapCore/Services/PlacementImport.h"
#include "War3MapCore/Services/PlacementCreation.h"
#include "War3MapCore/Render/ModelGameLimits.h"

#include <cmath>
#include <format>
#include <numbers>

namespace War3Map::Services {
namespace {
constexpr std::string_view kDomain = "Placement.Import";
using R = Result<PlacementImportPlan>;
using ObjectData::ObjectType;
using MapData::PlacementManifestKind;

R Fail(const char* code, std::string message) { return R::Failure(std::string(kDomain), code, std::move(message)); }

R ForType(std::vector<Diagnostic> diagnostics, const std::string& key) {
    for (auto& diagnostic : diagnostics) diagnostic.message = "类型 " + key + "：" + diagnostic.message;
    return {{}, std::move(diagnostics)};
}

// 一类物编（装饰物或单位）：本清单里这一类的全部新类型一次批量新建，rawcode 依次分配。
struct Batch {
    const ObjectCommandContext* source = nullptr;
    ObjectType type = ObjectType::Doodad;
    std::vector<std::size_t> typeIndexes;
    std::vector<ObjectCreationRequest> requests;
};

float Radians(float degrees) {
    double wrapped = std::fmod(double(degrees), 360.0);
    if (wrapped < 0) wrapped += 360.0;
    const auto radians = float(wrapped * std::numbers::pi / 180.0);
    return radians < float(2.0 * std::numbers::pi) ? radians : 0.0f;
}
} // namespace

Result<PlacementImportPlan> PlanPlacementImport(const MapData::PlacementManifest& manifest, const PlacementImportInputs& inputs) {
    const auto& header = inputs.terrain;
    if (header.width < 2 || header.height < 2 || !std::isfinite(header.offset[0]) || !std::isfinite(header.offset[1]))
        return Fail("PLACEMENT_IMPORT_TERRAIN", "地图地形网格不可用，不能确定摆放范围。");
    if (manifest.types.empty() || manifest.instances.empty()
        || manifest.instances.size() > MapData::PlacementManifestMaximumInstances)
        return Fail("PLACEMENT_IMPORT_EMPTY", "摆放清单须有类型和 1.." + std::to_string(MapData::PlacementManifestMaximumInstances) + " 个实例。");
    PlacementImportPlan plan;
    Batch doodads{&inputs.doodads, ObjectType::Doodad, {}, {}}, units{&inputs.units, ObjectType::Unit, {}, {}};
    std::vector<std::string> rawcodes(manifest.types.size()), bases(manifest.types.size());
    for (std::size_t index = 0; index < manifest.types.size(); ++index) {
        const auto& type = manifest.types[index];
        auto& batch = type.kind == PlacementManifestKind::Doodad ? doodads : units;
        const auto& source = *batch.source;
        if (!source.document || !source.standard || !source.schema || source.type != batch.type)
            return Fail("PLACEMENT_IMPORT_OBJECT_SOURCE", std::string("当前") + (batch.type == ObjectType::Doodad ? "装饰物" : "单位")
                + "物编来源不完整，不能新建类型 " + type.key + "。");
        bases[index] = type.base.value_or(std::string(PlacementImportDefaultDoodadBase));
        batch.typeIndexes.push_back(index);
        batch.requests.push_back({bases[index], type.name, {{batch.type == ObjectType::Doodad ? "dfil" : "umdl", {}, {},
            ObjectData::Value{type.model}}}});
    }
    for (auto* batch : {&doodads, &units}) {
        if (batch->requests.empty()) continue;
        std::size_t failed = batch->requests.size();
        auto created = PlanObjectCreationBatch(*batch->source, batch->requests, &failed);
        if (!created.Succeeded() || !created.value) {
            if (failed < batch->typeIndexes.size())
                return ForType(std::move(created.diagnostics), manifest.types[batch->typeIndexes[failed]].key);
            return {{}, std::move(created.diagnostics)};
        }
        for (std::size_t i = 0; i < batch->typeIndexes.size(); ++i) rawcodes[batch->typeIndexes[i]] = created.value->createdIds[i];
        created.value->plan.label = "导入摆放清单";
        (batch->type == ObjectType::Doodad ? plan.doodadObjects : plan.unitObjects) = std::move(created.value->plan);
    }
    for (std::size_t index = 0; index < manifest.types.size(); ++index)
        plan.report.types.push_back({manifest.types[index].key, manifest.types[index].kind, rawcodes[index], bases[index], 0});

    const double west = header.offset[0], south = header.offset[1];
    const double east = west + 128.0 * (header.width - 1), north = south + 128.0 * (header.height - 1);
    std::size_t outside = 0;
    std::string listed;
    for (std::size_t index = 0; index < manifest.instances.size(); ++index) {
        const auto& instance = manifest.instances[index];
        if (instance.type >= manifest.types.size())
            return Fail("PLACEMENT_IMPORT_TYPE_REFERENCE", "实例 " + std::to_string(index) + " 引用了不存在的类型。");
        const auto& type = manifest.types[instance.type];
        const auto [x, y, z] = instance.position;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || x < west || x > east || y < south || y > north) {
            if (outside++ < PlacementImportReportedOutside)
                listed += std::format("{}#{} ({:g}, {:g})", listed.empty() ? "" : "，", index, x, y);
            continue;
        }
        if (type.kind == PlacementManifestKind::Doodad) {
            if (!type.base && instance.variation != 0)
                return Fail("PLACEMENT_IMPORT_VARIATION", "实例 " + std::to_string(index) + "：默认装饰物只有一个变体，variation 只能为 0。");
            auto record = std::get<MapData::DoodadPlacement>(MakeDefaultPlacement(ObjectType::Doodad, rawcodes[instance.type], 0, instance.position));
            record.rotation = Radians(instance.facingDegrees); record.scale = instance.scale; record.variation = instance.variation;
            // 游戏只用 0x04 判断是否沿用存储的 Z；未设置时放回地面。
            if (!type.followGround) { record.flags = std::uint8_t(record.flags | MapData::kDoodadFlagFixedZ); ++plan.report.fixedHeightDoodads; }
            plan.doodads.push_back(std::move(record));
        } else {
            // 中立被动（玩家 16，记录中为 15）。单位的 Z 与三轴缩放游戏不采用，照样保存供编辑器参考。
            auto record = std::get<MapData::UnitPlacement>(MakeDefaultPlacement(ObjectType::Unit, rawcodes[instance.type], 15, instance.position));
            record.rotation = Radians(instance.facingDegrees); record.scale = instance.scale; record.variation = instance.variation;
            plan.units.push_back(std::move(record));
        }
        ++plan.report.types[instance.type].instances;
    }
    if (outside)
        return Fail("PLACEMENT_IMPORT_OUT_OF_BOUNDS", std::format("{} 个实例落在地图范围 x {:g}…{:g}、y {:g}…{:g} 之外：{}{}",
            outside, west, east, south, north, listed, outside > PlacementImportReportedOutside ? " 等" : ""));

    plan.report.doodads = plan.doodads.size();
    plan.report.units = plan.units.size();
    plan.report.files = manifest.files.size();
    if (inputs.memberExists)
        for (const auto& file : manifest.files) if (inputs.memberExists(file.archivePath)) ++plan.report.replacedFiles;
    for (const auto& file : manifest.files)
        if (Render::IsMdxPath(file.archivePath))
            if (auto limit = Render::GeosetGameLimitWarning(file.bytes, file.archivePath)) plan.report.warnings.push_back(std::move(*limit));
    return R::Success(std::move(plan));
}

} // namespace War3Map::Services
