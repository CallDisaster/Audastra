#include "WorldPlacementModels.h"

#include "EditorProject.h"
#include "ObjectNames.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <bit>
#include <cctype>
#include <map>
#include <optional>

namespace War3Map::Modules {
namespace {

using ObjectData::ObjectType;
constexpr int kListedCodes = 12;

std::optional<double> Number(const std::optional<std::string>& text) {
    if (!text) return std::nullopt;
    std::string_view value = *text;
    while (!value.empty() && (value.front() == ' ' || value.front() == '+')) value.remove_prefix(1);
    double result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || !std::isfinite(result)) return std::nullopt;
    return result;
}

bool Blank(const std::optional<std::string>& text) { return !text || text->empty() || *text == "_" || *text == "-"; }

// 物编父链（地图覆盖 → 自定义父对象 → 原版目录）上的字段查询；与物编显示共用同一继承解析。
class ObjectFields {
public:
    explicit ObjectFields(const QtProject::EditorProject& project) : project_(project) {}

    bool Find(ObjectType type, const std::string& rawcode) {
        type_ = type; rawcode_ = rawcode;
        const auto* document = project_.objectDocuments_[static_cast<std::size_t>(type)].get();
        const auto* catalog = project_.standardCatalog_.get();
        const ObjectData::ObjectKey key{type, rawcode};
        if (document && document->FindObject(key)) {
            chain_ = ResolveObjectInheritance(*document, key, catalog);
            return true;
        }
        const auto* native = document ? nullptr : project_.nativeObjectDocuments_[static_cast<std::size_t>(type)].get();
        if (native && native->FindObject(key)) {
            chain_ = ResolveNativeObjectInheritance(*native, key, catalog);
            return true;
        }
        chain_ = {};
        chain_.standardBase = catalog ? catalog->Find(type, rawcode) : nullptr;
        return chain_.standardBase != nullptr;
    }
    std::optional<std::string> Text(std::initializer_list<std::string_view> ids) const { return ObjectFieldValue(chain_, ids); }
    Services::PlacementTextFact FieldFact(std::string_view canonical, std::initializer_list<std::string_view> aliases,
        std::vector<Services::PlacementFieldOrigin>& origins) const {
        Services::PlacementTextFact fact;
        for (const auto* record : chain_.records) for (const auto& modification : record->modifications) {
            auto field = modification.key.fieldId;
            std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std::find(aliases.begin(), aliases.end(), field) == aliases.end()) continue;
            const bool lni = project_.objectDocuments_[static_cast<std::size_t>(type_)] != nullptr;
            Services::PlacementFieldOrigin origin{std::string(canonical), record->key.rawcode,
                lni ? "map-effective-lni" : "map-effective-native",
                std::string(lni ? ObjectData::LniFileName(type_) : ObjectData::NativeFileName(type_)), {}};
            if (modification.origin) {
                if (!modification.origin->source.empty()) {
                    const auto path = modification.origin->source.generic_u8string();
                    origin.sourcePath.assign(reinterpret_cast<const char*>(path.data()), path.size());
                }
                origin.byteOffset = modification.origin->offset;
                if (!modification.origin->fingerprint.empty()) origin.sourceId += ":" + modification.origin->fingerprint;
            }
            if (!modification.sourceKey.empty()) origin.sourceId += ":" + modification.sourceKey;
            origins.push_back(std::move(origin));
            if (modification.valueWasOpaque) {
                fact.state = Services::PlacementFieldState::Opaque; fact.reason = "有效覆盖字段不透明，未猜默认值。";
            } else { fact.state = Services::PlacementFieldState::Known; fact.value = ObjectData::ValueToDisplayString(modification.value); }
            return fact;
        }
        if (!chain_.IsComplete()) { fact.reason = chain_.reason.toStdString(); return fact; }
        if (chain_.standardBase) for (const auto alias : aliases) {
            const auto found = chain_.standardBase->fields.find(std::string(alias));
            if (found == chain_.standardBase->fields.end()) continue;
            if (chain_.standardBase->sourceFiles.empty()) { fact.reason = "原版有效字段缺少实际来源集合。"; return fact; }
            std::string paths;
            for (const auto& source : chain_.standardBase->sourceFiles) paths += std::to_string(source.size()) + ":" + source;
            origins.push_back({std::string(canonical), chain_.standardBase->key.rawcode,
                "standard-effective-source-set", std::move(paths), {}});
            fact.state = Services::PlacementFieldState::Known; fact.value = found->second; return fact;
        }
        fact.state = Services::PlacementFieldState::Absent; fact.reason = "完整物编父链没有此字段。"; return fact;
    }
    Services::PlacementTextFact ModelFact(std::vector<Services::PlacementFieldOrigin>& origins) const {
        switch (type_) {
        case ObjectType::Doodad: return FieldFact("dfil", {"file", "dfil"}, origins);
        case ObjectType::Destructable: return FieldFact("bfil", {"file", "bfil"}, origins);
        case ObjectType::Unit: return FieldFact("umdl", {"file", "umdl"}, origins);
        case ObjectType::Item: return FieldFact("ifil", {"file", "ifil"}, origins);
        default: return {};
        }
    }
    std::shared_ptr<const Services::PlacementPresentationFields> Presentation() const {
        const auto key = std::pair{type_, rawcode_};
        if (const auto found = presentations_.find(key); found != presentations_.end()) return found->second;
        auto result = std::make_shared<Services::PlacementPresentationFields>();
        const auto read = [&](std::string_view canonical, std::initializer_list<std::string_view> aliases) {
            Services::PlacementTextFact fact;
            for (const auto* record : chain_.records) for (const auto& modification : record->modifications) {
                auto field = modification.key.fieldId;
                std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (std::find(aliases.begin(), aliases.end(), field) == aliases.end()) continue;
                const bool lni = project_.objectDocuments_[static_cast<std::size_t>(type_)] != nullptr;
                Services::PlacementFieldOrigin origin{std::string(canonical), record->key.rawcode,
                    lni ? "map-effective-lni" : "map-effective-native",
                    std::string(lni ? ObjectData::LniFileName(type_) : ObjectData::NativeFileName(type_)), {}};
                if (modification.origin) {
                    if (!modification.origin->source.empty()) {
                        const auto source = modification.origin->source.generic_u8string();
                        origin.sourcePath.assign(reinterpret_cast<const char*>(source.data()), source.size());
                    }
                    origin.byteOffset = modification.origin->offset;
                    if (!modification.origin->fingerprint.empty()) origin.sourceId += ":" + modification.origin->fingerprint;
                }
                if (!modification.sourceKey.empty()) origin.sourceId += ":" + modification.sourceKey;
                result->origins.push_back(std::move(origin));
                if (modification.valueWasOpaque) {
                    fact.state = Services::PlacementFieldState::Opaque; fact.reason = "有效物编覆盖值不透明，未转成默认值。";
                } else {
                    fact.state = Services::PlacementFieldState::Known;
                    fact.value = ObjectData::ValueToDisplayString(modification.value);
                }
                return fact;
            }
            if (!chain_.IsComplete()) { fact.reason = chain_.reason.toStdString(); return fact; }
            if (chain_.standardBase) for (const auto alias : aliases) {
                const auto found = chain_.standardBase->fields.find(std::string(alias));
                if (found == chain_.standardBase->fields.end()) continue;
                if (chain_.standardBase->sourceFiles.empty()) { fact.reason = "原版有效字段缺少来源集合。"; return fact; }
                std::string paths;
                for (const auto& source : chain_.standardBase->sourceFiles) paths += std::to_string(source.size()) + ":" + source;
                result->origins.push_back({std::string(canonical), chain_.standardBase->key.rawcode,
                    "standard-effective-source-set", std::move(paths), {}});
                fact.state = Services::PlacementFieldState::Known; fact.value = found->second; return fact;
            }
            fact.state = Services::PlacementFieldState::Absent; return fact;
        };
        result->category = read("bcat", {"bcat", "category"});
        result->modelPath = read("bfil", {"bfil", "file"});
        result->pathingTexture = read("bptx", {"bptx", "pathtex"});
        const auto helper = read("buch", {"buch", "useclickhelper"});
        result->useClickHelper.state = helper.state; result->useClickHelper.reason = helper.reason;
        if (helper.state == Services::PlacementFieldState::Known) {
            if (helper.value == "1") result->useClickHelper.value = true;
            else if (helper.value == "0") result->useClickHelper.value = false;
            else {
                result->useClickHelper.state = Services::PlacementFieldState::Unavailable;
                result->useClickHelper.reason = "有效buch不是明确的0/1整数；未按非零猜辅助分类。";
            }
        }
        presentations_.emplace(key, result); return result;
    }
    bool IsPathingHelper() const {
        const auto key = std::pair{type_, rawcode_};
        if (const auto found = helpers_.find(key); found != helpers_.end()) return found->second;
        // 只复用公共分类。此阶段没有资源reader，绝不将其footprint当作占地证据。
        Services::PlacementPresentationContext context{{}, "world-effective-field-classification-v1", {}};
        Services::PlacementPresentationInput input{{type_, rawcode_}, {}, {}, *Presentation()};
        const auto row = Services::QueryPlacementPresentation(context, {}, context.resourceIdentity, input);
        const bool helper = row.Succeeded() && row.value->kind == Services::PlacementPresentationKind::PathingBlockerHelper;
        helpers_.emplace(key, helper); return helper;
    }
    double Value(std::initializer_list<std::string_view> ids, double fallback) const { return Number(Text(ids)).value_or(fallback); }
    std::string AnimationProperties() const {
        const auto properties = Text({"uani", "animprops"});
        return Blank(properties) ? std::string{} : *properties;
    }
    // 0..255 的颜色分量（缺省 255）。
    std::array<float, 3> Tint(std::initializer_list<std::string_view> red, std::initializer_list<std::string_view> green,
        std::initializer_list<std::string_view> blue) const {
        const auto channel = [&](std::initializer_list<std::string_view> ids) {
            return static_cast<float>(std::clamp(Value(ids, 255), 0.0, 255.0) / 255.0);
        };
        return {channel(red), channel(green), channel(blue)};
    }
    Services::BuildingSplatEffectiveFields BuildingSplat() const {
        Services::BuildingSplatEffectiveFields result;
        if (!chain_.IsComplete()) { result.reason = chain_.reason.toStdString(); return result; }
        const auto read = [&](std::string_view canonical, std::initializer_list<std::string_view> aliases) {
            const auto value = Text(aliases);
            if (!value) return value;
            for (const auto* record : chain_.records) for (const auto& modification : record->modifications) {
                auto field = modification.key.fieldId;
                std::transform(field.begin(), field.end(), field.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (std::find(aliases.begin(), aliases.end(), field) == aliases.end()) continue;
                const auto index = static_cast<std::size_t>(type_);
                const bool lni = project_.objectDocuments_[index] != nullptr;
                Services::BuildingSplatFieldOrigin origin;
                origin.field = canonical; origin.objectRawcode = record->key.rawcode;
                origin.sourceId = lni ? "map-effective-lni" : "map-effective-native";
                origin.sourcePath = std::string(lni ? ObjectData::LniFileName(type_) : ObjectData::NativeFileName(type_));
                if (modification.origin) {
                    if (!modification.origin->source.empty()) origin.sourcePath = modification.origin->source.generic_string();
                    origin.byteOffset = modification.origin->offset;
                    if (!modification.origin->fingerprint.empty()) origin.sourceId += ":" + modification.origin->fingerprint;
                }
                if (!modification.sourceKey.empty()) origin.sourceId += ":" + modification.sourceKey;
                result.origins.push_back(std::move(origin));
                return modification.valueWasOpaque ? std::optional<std::string>{} : value;
            }
            if (chain_.standardBase && !chain_.standardBase->sourceFiles.empty()) {
                // StandardObjectCatalog目前只有整行来源集合；保留集合，不猜哪个文件最后覆盖此字段。
                std::string paths;
                for (const auto& source : chain_.standardBase->sourceFiles) {
                    paths += std::to_string(source.size()) + ":" + source;
                }
                result.origins.push_back({std::string(canonical), chain_.standardBase->key.rawcode,
                    "standard-effective-source-set", std::move(paths), std::nullopt});
            }
            return value;
        };
        const auto uubs = read("uubs", {"uubs", "ubersplat"});
        if (!uubs) {
            std::vector<Services::PlacementFieldOrigin> absentOrigins, buildingOrigins, heroOrigins, creepOrigins;
            const auto fact = FieldFact("uubs", {"uubs", "ubersplat"}, absentOrigins);
            const auto building = FieldFact("ubdg", {"ubdg", "isbldg"}, buildingOrigins);
            const auto hero = FieldFact("hero", {"hero"}, heroOrigins);
            const auto creep = FieldFact("creep", {"creep"}, creepOrigins);
            const auto flag = [](const Services::PlacementTextFact& fact) -> std::optional<bool> {
                if (fact.state != Services::PlacementFieldState::Known) return {};
                if (fact.value == "1") return true;
                if (fact.value == "0") return false;
                return {};
            };
            ObjectData::ObjectFieldContext context;
            context.rawcode = rawcode_; context.hero = flag(hero); context.creep = flag(creep);
            context.building = flag(building); context.item = type_ == ObjectType::Item;
            if (chain_.standardBase) {
                context.baseCode = std::string(chain_.standardBase->Field("code"));
                if (context.baseCode.empty()) context.baseCode = chain_.standardBase->key.rawcode;
            }
            const auto* definition = project_.objectFieldSchema_
                ? project_.objectFieldSchema_->FindNative(ObjectType::Unit, "uubs") : nullptr;
            const auto explicitFlag = [&](std::string_view key, std::string_view value) {
                if (!definition) return false;
                const auto found = definition->metadata.find(std::string(key));
                return found != definition->metadata.end() && found->second == value;
            };
            if (fact.state == Services::PlacementFieldState::Absent && absentOrigins.empty()
                && building.state == Services::PlacementFieldState::Known && building.value == "0" && !buildingOrigins.empty()
                && definition && definition->useBuilding && !definition->useUnit
                && explicitFlag("usebuilding", "1") && explicitFlag("useunit", "0")
                && context.hero.has_value() && context.creep.has_value() && !heroOrigins.empty() && !creepOrigins.empty()
                && !definition->sourceField.empty() && !definition->sourceTable.empty()
                && !project_.objectFieldSchema_->AppliesTo(*definition, context)) {
                result.complete = true;
                result.configuration = Services::BuildingSplatConfiguration::NotApplicableToNonBuilding;
                result.reason = "完整父链的uubs为Absent；有效ubdg=0，实际hero/creep类别已确认，共用字段schema对该对象明确不适用。";
                for (const auto& origin : buildingOrigins) result.origins.push_back({origin.field, origin.objectRawcode,
                    origin.sourceId, origin.sourcePath, origin.byteOffset});
                for (const auto* origins : {&heroOrigins, &creepOrigins})
                    for (const auto& origin : *origins) result.origins.push_back({origin.field, origin.objectRawcode,
                        origin.sourceId, origin.sourcePath, origin.byteOffset});
                // Schema保留declared table和原metadata标记；没有原文件行offset，不伪造定位。
                result.origins.push_back({"uubs-applicability-schema", rawcode_,
                    "object-field-schema:uubs:usebuilding=1:useunit=0:usehero=" + std::to_string(definition->useHero)
                        + ":usecreep=" + (definition->metadata.contains("usecreep") ? std::to_string(definition->useCreep) : "default-true")
                        + ":context.hero=" + std::to_string(*context.hero) + ":context.creep=" + std::to_string(*context.creep)
                        + ":context.item=0",
                    "declared-table:" + definition->sourceTable + ":field:" + definition->sourceField, {}});
                return result;
            }
            if (fact.state == Services::PlacementFieldState::Absent && (!context.hero.has_value() || !context.creep.has_value())) {
                result.reason = "原uubs为Absent，但实际hero/creep类别未确认；未把缺分类当false或推断字段不适用。";
                return result;
            }
            result.reason = "有效uubs缺失或不透明；未猜空贴画。"; return result;
        }
        result.uubs = *uubs;
        if (uubs->empty() || *uubs == "_") { result.complete = true; return result; }
        const std::array<std::optional<std::string>, 3> colors{
            read("uclr", {"uclr", "red"}), read("uclg", {"uclg", "green"}), read("uclb", {"uclb", "blue"})};
        for (std::size_t i = 0; i < colors.size(); ++i) {
            if (!colors[i]) { result.reason = "建筑贴画tint字段缺失或不透明；未填默认255。"; return result; }
            const auto& text = *colors[i]; std::uint32_t channel = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), channel);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || channel > 255) {
                result.reason = "建筑贴画tint不是0..255的整数；未clamp或猜默认值。"; return result;
            }
            result.tint[i] = static_cast<std::uint8_t>(channel);
        }
        result.complete = true; return result;
    }

private:
    const QtProject::EditorProject& project_;
    ObjectInheritanceChain chain_;
    ObjectType type_ = ObjectType::Unit;
    std::string rawcode_;
    mutable std::map<std::pair<ObjectType, std::string>, std::shared_ptr<const Services::PlacementPresentationFields>> presentations_;
    mutable std::map<std::pair<ObjectType, std::string>, bool> helpers_; // 当前只读capture一次分类，不逐实例重算SHA。
};

void Unresolved(WorldPlacementCapture& capture, WorldUnresolvedPlacement row) {
    ++capture.unresolved;
    const auto& rawcode = row.owner.rawcode;
    const auto code = QString::fromLatin1(rawcode.data(), static_cast<qsizetype>(rawcode.size()));
    if (capture.unresolvedCodes.size() < kListedCodes && !capture.unresolvedCodes.contains(code)) capture.unresolvedCodes << code;
    capture.unresolvedRows.push_back(std::move(row));
}

Render::ModelInstanceRequest Place(const MapData::PlacementTransform& placement, std::string model) {
    Render::ModelInstanceRequest request;
    request.modelPath = std::move(model);
    request.position = placement.position;
    request.angle = placement.rotation;
    request.scale = placement.scale;
    return request;
}

void UnitPresentation(const ObjectFields& fields, Render::ModelInstanceRequest& request) {
    Render::ModelSelectionDescription display;
    display.rawPlacementZ = request.position[2];
    if (const auto scale = Number(fields.Text({"ussc", "scale"})); scale && *scale >= 0 && *scale <= 1000000) {
        display.circleScale = static_cast<float>(*scale); display.circleScaleKnown = true;
    }
    display.circleHeight = static_cast<float>(fields.Value({"uslz", "selz"}, 0));
    display.circleOnWater = fields.Value({"usew", "selcirconwater"}, 0) != 0;
    display.scalesProjectiles = fields.Value({"uscb", "scalebull"}, 0) != 0;
    if (const auto movement = fields.Text({"umvt", "movetp"}); movement && *movement == "fly") {
        const auto height = fields.Value({"umvh", "moveheight"}, 0);
        if (std::isfinite(static_cast<float>(height)) && std::isfinite(static_cast<float>(request.position[2] + height))) {
            display.flyHeightOffset = static_cast<float>(height);
            display.heightPreviewApproximate = true;
        }
    }
    request.selection = display;
    request.position = WorldPlacementDisplayPosition(request, request.position);
}

std::uint32_t Variation(std::int32_t variation, double count) {
    const auto total = static_cast<std::uint32_t>(std::clamp(count, 1.0, 1024.0));
    return static_cast<std::uint32_t>(std::clamp(variation, 0, static_cast<std::int32_t>(total) - 1));
}

} // namespace

std::array<float, 3> WorldPlacementDisplayPosition(const Render::ModelInstanceRequest& request,
    const std::array<float, 3>& rawPosition) {
    auto result = rawPosition;
    if (request.selection && request.selection->heightPreviewApproximate)
        result[2] += request.selection->flyHeightOffset;
    return result;
}

namespace {
std::optional<Render::ModelInstanceRequest> RequestFromFields(const ObjectFields& fields, ObjectType type,
    const MapData::PlacementTransform& placement, std::int32_t owner, std::int32_t teamColor, std::int32_t variationIndex) {
    const bool doodad = type == ObjectType::Doodad;
    const bool destructable = type == ObjectType::Destructable;
    const bool unit = type == ObjectType::Unit;
    const bool helper = destructable && fields.IsPathingHelper();
    const auto model = fields.Text(doodad ? std::initializer_list<std::string_view>{"file", "dfil"}
        : destructable ? std::initializer_list<std::string_view>{"file", "bfil"}
        : unit ? std::initializer_list<std::string_view>{"file", "umdl"}
               : std::initializer_list<std::string_view>{"file", "ifil"});
    if (Blank(model) && !helper) return std::nullopt;
    const auto count = doodad ? fields.Value({"numvar", "dvar"}, 1)
        : destructable ? fields.Value({"numvar", "bvar"}, 1) : 1;
    const auto variation = doodad || destructable ? Variation(variationIndex, count) : 0;
    auto request = Place(placement, Blank(model) ? std::string{} : Render::MdxModelFile(*model, variation,
        static_cast<std::uint32_t>(std::max(1.0, count))));
    if (unit) {
        request.animationProperties = fields.AnimationProperties();
        UnitPresentation(fields, request);
    }
    if (destructable) {
        if (auto texture = fields.Text({"texfile", "btxf"}); !Blank(texture)) {
            if (texture->find('.') == std::string::npos) *texture += ".blp";
            request.replaceableTexture = std::move(*texture);
        }
        request.tint = fields.Tint({"colorr", "bvcr"}, {"colorg", "bvcg"}, {"colorb", "bvcb"});
        // 编辑辅助采用真实来源平面尺寸，原版缩放/锚点未取证；不改持久PlacementTransform。
        if (helper) { request.scale = {1, 1, 1}; request.tint = {1, 1, 1}; request.replaceableTexture.clear(); }
    } else if (doodad) request.tint = fields.Tint({"vertr", "dvr1"}, {"vertg", "dvg1"}, {"vertb", "dvb1"});
    else {
        const auto scale = unit ? fields.Value({"modelscale", "usca"}, 1) : fields.Value({"scale", "isca"}, 1);
        for (auto& axis : request.scale) axis = static_cast<float>(axis * std::clamp(scale, 0.01, 100.0));
        request.teamColor = static_cast<std::uint32_t>(std::clamp(teamColor >= 0 ? teamColor : owner, 0, 15));
        request.tint = unit ? fields.Tint({"red", "uclr"}, {"green", "uclg"}, {"blue", "uclb"})
            : fields.Tint({"colorr", "iclr"}, {"colorg", "iclg"}, {"colorb", "iclb"});
    }
    return request;
}
void CaptureRow(const QtProject::EditorProject& project, ObjectFields& fields, const MapDocument& document,
    const MapData::PlacementId& id, WorldPlacementCapture& capture, bool retainUnresolved) {
    (void)project; // fields持有同一project父链查询，不重复构造目录。
    const MapData::PlacementTransform* placement = nullptr;
    std::optional<ObjectType> type;
    std::int32_t owner = 0, teamColor = -1, variation = 0;
    if (id.file == MapData::PlacementFile::Doodads) {
        const auto& item = document.Doodads()->doodads[id.index]; placement = &item; variation = item.variation;
        if (fields.Find(ObjectType::Destructable, id.rawcode)) type = ObjectType::Destructable;
        else if (fields.Find(ObjectType::Doodad, id.rawcode)) type = ObjectType::Doodad;
    } else {
        const auto& item = document.Units()->units[id.index]; placement = &item; owner = item.owner; teamColor = item.teamColor;
        if (item.rawcode == "sloc") return;
        if (fields.Find(ObjectType::Unit, id.rawcode)) type = ObjectType::Unit;
        else if (fields.Find(ObjectType::Item, id.rawcode)) type = ObjectType::Item;
    }
    auto request = type ? RequestFromFields(fields, *type, *placement, owner, teamColor, variation) : std::nullopt;
    if (!request) {
        WorldUnresolvedPlacement row; row.owner = id; row.position = placement->position; row.objectType = type;
        if (!type) { row.reasonCode = "WORLD_OBJECT_DEFINITION_MISSING"; row.reason = "摆放rawcode在当前地图/原版目录中没有可解析物体定义。"; }
        else {
            const auto fact = fields.ModelFact(row.origins); row.modelPath = fact.value;
            if (fact.state == Services::PlacementFieldState::Opaque) { row.reasonCode = "WORLD_MODEL_FIELD_OPAQUE"; row.reason = fact.reason; }
            else if (fact.state == Services::PlacementFieldState::Unavailable) { row.reasonCode = "WORLD_MODEL_SOURCE_UNAVAILABLE"; row.reason = fact.reason; }
            else if (fact.state == Services::PlacementFieldState::Absent) { row.reasonCode = "WORLD_MODEL_FIELD_ABSENT"; row.reason = fact.reason; }
            else { row.reasonCode = "WORLD_MODEL_FIELD_EMPTY"; row.reason = "有效模型字段明确为空或无模型哨兵，未猜替代模型。"; }
        }
        Unresolved(capture, std::move(row));
        if (retainUnresolved) { capture.requests.push_back(Place(*placement, {})); capture.ids.push_back(id); }
        return;
    }
    if (*type == ObjectType::Unit) { ++capture.units; if (request->selection->heightPreviewApproximate) ++capture.flightHeightPreviewUnits; }
    else if (*type == ObjectType::Item) ++capture.items;
    else if (*type == ObjectType::Doodad) ++capture.doodads;
    else ++capture.destructibles;
    if (*type == ObjectType::Destructable) capture.presentations.push_back({static_cast<std::uint32_t>(capture.requests.size()),
        {*type, id.rawcode}, id, fields.Presentation()});
    capture.requests.push_back(std::move(*request)); capture.ids.push_back(id);
}
} // namespace

WorldPlacementCapture CaptureWorldPlacements(const QtProject::EditorProject& project, const MapDocument& document,
    std::optional<MapData::PlacementFile> file) {
    WorldPlacementCapture capture; ObjectFields fields(project);
    if (!file || *file == MapData::PlacementFile::Doodads) {
        if (const auto& doodads = document.Doodads())
            for (std::uint32_t i = 0; i < doodads->doodads.size(); ++i)
                CaptureRow(project, fields, document, MapData::MakePlacementId(MapData::PlacementFile::Doodads, i, doodads->doodads[i]), capture, false);
    }
    if (!file || *file == MapData::PlacementFile::Units) {
        if (const auto& units = document.Units())
            for (std::uint32_t i = 0; i < units->units.size(); ++i)
                CaptureRow(project, fields, document, MapData::MakePlacementId(MapData::PlacementFile::Units, i, units->units[i]), capture, false);
    }
    return capture;
}
Result<WorldBuildingSplatCapture> CaptureWorldBuildingSplats(const QtProject::EditorProject& project,
    const MapDocument& document, const Services::WorldSelectionVersion& version) {
    WorldBuildingSplatCapture capture; capture.version = version;
    std::map<std::string, Services::BuildingSplatEffectiveFields> fieldsByRawcode;
    std::map<std::string, std::size_t> diagnosticByRawcode;
    ObjectFields fields(project);
    std::string identity;
    const auto part = [&](std::string_view value) { identity += std::to_string(value.size()) + ":" + std::string(value); };
    part(version.document.documentId); part(version.document.sourceSha256);
    part(std::to_string(version.document.generation)); part(std::to_string(version.document.revision));
    part(std::to_string(version.catalogRevision));
    if (const auto& units = document.Units()) for (std::uint32_t i = 0; i < units->units.size(); ++i) {
        const auto& unit = units->units[i];
        if (unit.rawcode == "sloc") continue;
        auto found = fieldsByRawcode.find(unit.rawcode);
        if (found == fieldsByRawcode.end()) {
            const auto type = PlacementType(project, unit.rawcode, ObjectType::Unit, ObjectType::Item);
            if (type == ObjectType::Item) continue;
            Services::BuildingSplatEffectiveFields effective;
            if (type == ObjectType::Unit && fields.Find(ObjectType::Unit, unit.rawcode)) effective = fields.BuildingSplat();
            else effective.reason = "单位/物品物编身份未知或歧义；未猜uubs。";
            WorldBuildingSplatCapture::FieldDiagnostic diagnostic; diagnostic.rawcode = unit.rawcode;
            if (type == ObjectType::Unit && fields.Find(ObjectType::Unit, unit.rawcode))
                diagnostic.uubs = fields.FieldFact("uubs", {"uubs", "ubersplat"}, diagnostic.origins);
            else diagnostic.uubs.reason = effective.reason;
            diagnostic.configuration = effective.configuration;
            if (effective.configuration == Services::BuildingSplatConfiguration::NotApplicableToNonBuilding) {
                diagnostic.applicabilityReason = effective.reason;
                for (const auto& origin : effective.origins) diagnostic.applicabilityOrigins.push_back({origin.field,
                    origin.objectRawcode, origin.sourceId, origin.sourcePath, origin.byteOffset});
            }
            diagnosticByRawcode.emplace(unit.rawcode, capture.fieldDiagnostics.size());
            capture.fieldDiagnostics.push_back(std::move(diagnostic));
            found = fieldsByRawcode.emplace(unit.rawcode, std::move(effective)).first;
        }
        ++capture.fieldDiagnostics[diagnosticByRawcode.at(unit.rawcode)].placements;
        Services::BuildingSplatUnitInput input;
        input.owner = MapData::MakePlacementId(MapData::PlacementFile::Units, i, unit);
        input.version = version; input.position = unit.position; input.fields = found->second;
        part(std::to_string(input.owner.index)); part(input.owner.rawcode); part(std::to_string(input.owner.creationId));
        for (const auto value : input.position) part(std::to_string(std::bit_cast<std::uint32_t>(value)));
        part(input.fields.complete ? "1" : "0"); part(input.fields.uubs); part(input.fields.reason);
        part(std::to_string(static_cast<unsigned>(input.fields.configuration)));
        for (const auto value : input.fields.tint) part(std::to_string(value));
        for (const auto& origin : input.fields.origins) {
            part(origin.field); part(origin.objectRawcode); part(origin.sourceId); part(origin.sourcePath);
            part(origin.byteOffset ? std::to_string(*origin.byteOffset) : "unknown-offset");
        }
        capture.units.push_back(std::move(input));
    }
    auto hashed = Sha256Text(identity);
    if (!hashed.Succeeded()) return {std::nullopt, std::move(hashed.diagnostics)};
    capture.inputIdentity = std::move(*hashed.value);
    return Result<WorldBuildingSplatCapture>::Success(std::move(capture));
}
Result<WorldPlacementCapture> CapturePlacementRows(const QtProject::EditorProject& project, const MapDocument& document,
    std::span<const MapData::PlacementId> rows, bool retainUnresolved) {
    WorldPlacementCapture capture; ObjectFields fields(project);
    constexpr std::size_t budget = 64ull * 1024 * 1024;
    if (rows.size() > budget / (sizeof(Render::ModelInstanceRequest) + sizeof(MapData::PlacementId)))
        return Result<WorldPlacementCapture>::Failure("World.Placement", "WORLD_PLACEMENT_QUERY_BUDGET", "指定摆放行查询超过实际载荷预算。");
    for (const auto& id : rows) {
        auto source = MapData::FindPlacement(document, id);
        if (!source.Succeeded()) return {std::nullopt, std::move(source.diagnostics)};
        CaptureRow(project, fields, document, id, capture, retainUnresolved);
    }
    return Result<WorldPlacementCapture>::Success(std::move(capture));
}
std::optional<Render::ModelInstanceRequest> CapturePlacementPreview(const QtProject::EditorProject& project, ObjectType type,
    const MapData::PlacementTransform& placement, std::int32_t owner, WorldPlacementPresentationCandidate* presentation) {
    ObjectFields fields(project);
    if (!fields.Find(type, placement.rawcode)) return std::nullopt;
    auto request = RequestFromFields(fields, type, placement, type == ObjectType::Item ? 15 : owner, -1, 0);
    if (request) {
        request->previewGhost = true;
        if (presentation && type == ObjectType::Destructable)
            *presentation = {0, {type, placement.rawcode}, {}, fields.Presentation()};
    }
    return request;
}

std::variant<MapData::UnitPlacement, MapData::DoodadPlacement> MakeNewPlacement(
    ObjectData::ObjectType type, const std::string& rawcode, std::int32_t owner, const std::array<float, 3>& position) {
    constexpr float kFacing = 4.71238898f; // 270°
    if (type == ObjectType::Doodad || type == ObjectType::Destructable) {
        MapData::DoodadPlacement doodad;
        doodad.rawcode = rawcode; doodad.position = position; doodad.rotation = kFacing; doodad.scale = {1, 1, 1};
        doodad.flags = 2; doodad.lifePercent = 100; doodad.droppedItemTable = -1;
        return doodad;
    }
    MapData::UnitPlacement unit;
    unit.rawcode = rawcode; unit.position = position; unit.rotation = kFacing; unit.scale = {1, 1, 1};
    unit.flags = 2; unit.owner = type == ObjectType::Item ? 15 : owner;
    unit.hitPoints = -1; unit.mana = -1; unit.droppedItemTable = -1; unit.gold = 12500;
    unit.targetAcquisition = -1; unit.heroLevel = 1;
    unit.random = MapData::RandomAnyUnit{1, 0};
    unit.teamColor = -1; unit.waygateRegionId = -1;
    return unit;
}

} // namespace War3Map::Modules
