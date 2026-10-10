#include "War3MapCore/Services/ProjectReferenceIndex.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <functional>

namespace War3Map::Services {
namespace {

using ObjectData::IndexedReferenceOrigin;

UsagePlace PlaceOf(std::int32_t ecaType) {
    switch (ecaType) {
    case 0: return UsagePlace::Event;
    case 1: return UsagePlace::Condition;
    default: return UsagePlace::Action;
    }
}

const char* PlacementDetail(IndexedReferenceOrigin origin) {
    switch (origin) {
    case IndexedReferenceOrigin::PlacedUnitOrItem: return "放置单位 / 物品";
    case IndexedReferenceOrigin::PlacedDoodadOrDestructable: return "放置装饰物 / 可破坏物";
    case IndexedReferenceOrigin::SpecialDoodadOrDestructable: return "特殊装饰物";
    case IndexedReferenceOrigin::PlacedAbility: return "放置单位的技能";
    case IndexedReferenceOrigin::PlacedInventoryItem: return "放置单位的物品栏";
    case IndexedReferenceOrigin::PlacedDropItem: return "掉落物品";
    case IndexedReferenceOrigin::RandomUnitChoice: return "随机单位候选";
    default: return "放置实例";
    }
}

// 触发器语句遍历：参数（含嵌套调用与数组下标）中的记号与被调用函数归到所在语句。
class TriggerWalker {
public:
    TriggerWalker(const Trigger::TriggerDocument& document,
        std::function<std::size_t(Usage)> add, std::function<void(std::size_t, const std::string&)> token,
        std::function<void(std::size_t, const std::string&)> call)
        : document_(document), add_(std::move(add)), token_(std::move(token)), call_(std::move(call)) {}

    void Walk() {
        for (const auto& trigger : document_.triggers) {
            ownerId_ = TriggerOwnerId(trigger.id);
            owner_ = trigger.name.text;
            for (const std::uint32_t root : trigger.ecas) Statement(root, trigger.enabled == 0, 0);
        }
    }

private:
    void Statement(std::uint32_t index, bool disabled, std::size_t depth) {
        if (index >= document_.ecaNodes.size() || depth > 256) return;
        const auto& eca = document_.ecaNodes[index];
        const bool off = disabled || eca.enabled == 0;
        Usage usage;
        usage.module = UsageModule::Triggers;
        usage.ownerId = ownerId_;
        usage.owner = owner_;
        usage.place = PlaceOf(eca.type);
        usage.ecaIndex = index;
        usage.disabled = off;
        const std::size_t id = add_(std::move(usage));
        call_(id, eca.name.text);
        for (const std::uint32_t argument : eca.arguments) Argument(argument, id, depth + 1);
        for (const std::uint32_t child : eca.children) Statement(child, off, depth + 1);
    }

    void Argument(std::uint32_t index, std::size_t usage, std::size_t depth) {
        if (index >= document_.argumentNodes.size() || depth > 256) return;
        const auto& argument = document_.argumentNodes[index];
        if (argument.callEca && *argument.callEca < document_.ecaNodes.size()) {
            const auto& call = document_.ecaNodes[*argument.callEca];
            call_(usage, call.name.text);
            for (const std::uint32_t nested : call.arguments) Argument(nested, usage, depth + 1);
        } else {
            const std::string_view value = War3Map::Text::TrimHorizontal(argument.value.text);
            if (!value.empty() && value.size() <= 256) {
                token_(usage, std::string(value));
                // 变量参数可能带 udg_ 前缀（自定义文本转写等），同时登记裸名。
                if (argument.type == 1 && value.starts_with("udg_")) token_(usage, std::string(value.substr(4)));
            }
        }
        if (argument.indexArgument) Argument(*argument.indexArgument, usage, depth + 1);
    }

    const Trigger::TriggerDocument& document_;
    std::function<std::size_t(Usage)> add_;
    std::function<void(std::size_t, const std::string&)> token_;
    std::function<void(std::size_t, const std::string&)> call_;
    std::string ownerId_;
    std::string owner_;
};

} // namespace

std::string ObjectOwnerId(const ObjectData::ObjectKey& key) {
    return "object:" + std::string(ObjectData::ToString(key.type)) + ':' + key.rawcode;
}

std::string TriggerOwnerId(std::uint64_t triggerId) {
    return "trigger:" + std::to_string(triggerId);
}

std::size_t ProjectReferenceIndex::Add(Usage usage) {
    usages_.push_back(std::move(usage));
    return usages_.size() - 1;
}

void ProjectReferenceIndex::Link(std::size_t usage, const std::string& token) {
    auto& list = tokens_[token];
    if (list.empty() || list.back() != usage) list.push_back(usage);
    outgoing_[usages_[usage].ownerId].insert(token);
}

ProjectReferenceIndex ProjectReferenceIndex::Build(const ProjectReferenceInputs& inputs) {
    ProjectReferenceIndex index;
    if (inputs.triggers != nullptr) {
        TriggerWalker walker(*inputs.triggers,
            [&index](Usage usage) { return index.Add(std::move(usage)); },
            [&index](std::size_t usage, const std::string& token) { index.Link(usage, token); },
            [&index](std::size_t usage, const std::string& function) {
                if (function.empty()) return;
                auto& list = index.functions_[function];
                if (list.empty() || list.back() != usage) list.push_back(usage);
            });
        walker.Walk();
        for (const auto& variable : inputs.triggers->variables) {
            const std::string_view value = War3Map::Text::TrimHorizontal(variable.value.text);
            if (variable.hasDefault == 0 || value.empty()) continue;
            Usage usage;
            usage.module = UsageModule::Variables;
            usage.ownerId = "variable:" + variable.name.text;
            usage.owner = variable.name.text;
            usage.place = UsagePlace::VariableInitial;
            usage.detail = std::string(value);
            index.Link(index.Add(std::move(usage)), std::string(value));
        }
    }

    const MapDocument empty = MapDocument::FromMembers({});
    const auto objects = ObjectData::ObjectReferenceIndex::Build(
        inputs.objects, inputs.map != nullptr ? *inputs.map : empty, inputs.schema);
    index.coverage_.assign(objects.Coverage().begin(), objects.Coverage().end());
    for (const auto& reference : objects.References()) {
        Usage usage;
        usage.possible = !reference.exactType;
        if (reference.ownerObject) {
            usage.module = UsageModule::Objects;
            usage.ownerId = ObjectOwnerId(*reference.ownerObject);
            usage.owner = reference.ownerObject->rawcode;
            usage.ownerObject = reference.ownerObject;
            usage.place = reference.origin == IndexedReferenceOrigin::ObjectParent
                ? UsagePlace::ObjectParent : UsagePlace::ObjectField;
            usage.detail = reference.fieldId;
        } else {
            usage.module = UsageModule::World;
            usage.ownerId = "world";
            usage.owner = "地图放置";
            usage.place = UsagePlace::Placement;
            usage.detail = PlacementDetail(reference.origin);
            usage.placementIndex = reference.placementIndex;
        }
        index.Link(index.Add(std::move(usage)), reference.rawcode);
    }
    return index;
}

std::vector<const Usage*> ProjectReferenceIndex::UsagesOf(
    std::span<const std::string> tokens, std::string_view excludeOwnerId) const {
    std::vector<std::size_t> ids;
    for (const std::string& token : tokens) {
        const auto found = tokens_.find(token);
        if (found != tokens_.end()) ids.insert(ids.end(), found->second.begin(), found->second.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::vector<const Usage*> result;
    for (const std::size_t id : ids) {
        if (!excludeOwnerId.empty() && usages_[id].ownerId == excludeOwnerId) continue;
        result.push_back(&usages_[id]);
    }
    return result;
}

std::vector<const Usage*> ProjectReferenceIndex::CallsOf(std::string_view function) const {
    std::vector<const Usage*> result;
    const auto found = functions_.find(std::string(function));
    if (found == functions_.end()) return result;
    for (const std::size_t id : found->second) result.push_back(&usages_[id]);
    return result;
}

std::vector<std::string> ProjectReferenceIndex::CalledFunctions() const {
    std::vector<std::string> names;
    names.reserve(functions_.size());
    for (const auto& [name, calls] : functions_) names.push_back(name);
    return names;
}

const std::set<std::string>& ProjectReferenceIndex::Outgoing(std::string_view ownerId) const {
    static const std::set<std::string> kEmpty;
    const auto found = outgoing_.find(std::string(ownerId));
    return found == outgoing_.end() ? kEmpty : found->second;
}

} // namespace War3Map::Services
