#include "War3MapCore/Services/NameResolution.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <unordered_set>

namespace War3Map::Services {
namespace {

std::string_view Unquote(std::string_view value) {
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
        return value.substr(1, value.size() - 2);
    return value;
}

std::string PlainName(std::string_view value) {
    const auto decoded = Trigger::DecodeSourceText(std::as_bytes(std::span(value.data(), value.size())));
    value = decoded.text;
    std::string result;
    const auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
    for (std::size_t i = 0; i < value.size();) {
        if (i + 10 <= value.size() && value[i] == '|' && (value[i + 1] == 'c' || value[i + 1] == 'C')
            && std::all_of(value.begin() + i + 2, value.begin() + i + 10, hex)) { i += 10; continue; }
        if (i + 2 <= value.size() && value[i] == '|') {
            if (value[i + 1] == 'r' || value[i + 1] == 'R') { i += 2; continue; }
            if (value[i + 1] == 'n' || value[i + 1] == 'N') { result += ' '; i += 2; continue; }
        }
        result += value[i++];
    }
    result = Text::TrimCWhitespaceCopy(result);
    return result.empty() ? std::string(value) : result;
}

NameResolution Failure(std::string_view raw, NameResolutionState state, std::string reason) {
    NameResolution result;
    result.rawValue = raw;
    result.displayName = raw;
    result.state = state;
    result.reason = std::move(reason);
    return result;
}

const MapData::StringTableDocument* Strings(const NameResolutionContext& context) {
    if (context.strings) return context.strings;
    return context.mapDocument && context.mapDocument->Strings() ? &*context.mapDocument->Strings() : nullptr;
}

NameResolution StringName(const NameResolutionContext& context, std::string_view raw, std::string_view value) {
    const auto display = [&](std::string_view bytes) {
        return context.preserveStringFormatting
            ? Trigger::DecodeSourceText(std::as_bytes(std::span(bytes.data(), bytes.size()))).text : PlainName(bytes);
    };
    if (!value.starts_with("TRIGSTR_")) {
        auto result = Failure(raw, NameResolutionState::Literal, {});
        result.displayName = display(value);
        return result;
    }
    const auto id = StringReferenceId(value);
    if (!id)
        return Failure(raw, NameResolutionState::Invalid, "地图字符串引用格式不支持：" + std::string(value));
    const auto* strings = Strings(context);
    if (!strings) return Failure(raw, NameResolutionState::MissingString, "地图字符串表未加载");
    const MapData::StringTableEntry* entry = nullptr;
    for (const auto& candidate : strings->entries) if (candidate.id == *id) {
        if (entry) return Failure(raw, NameResolutionState::Ambiguous, "地图字符串表有重复条目：" + std::string(value));
        entry = &candidate;
    }
    if (!entry) return Failure(raw, NameResolutionState::MissingString, "地图字符串表缺少条目：" + std::string(value));
    auto result = Failure(raw, NameResolutionState::Resolved, {});
    result.displayName = entry->value.empty() && !context.preserveStringFormatting ? std::string(raw) : display(entry->value);
    result.reference = value;
    if (entry->value.empty()) result.reason = "地图字符串条目为空";
    return result;
}

std::optional<ObjectData::ObjectType> TypeFor(std::string_view type) {
    using enum ObjectData::ObjectType;
    if (type == "unitcode") return Unit;
    if (type == "itemcode") return Item;
    if (type == "abilcode" || type == "heroskillcode") return Ability;
    if (type == "buffcode") return Buff;
    if (type == "techcode") return Upgrade;
    if (type == "doodadcode") return Doodad;
    if (type == "destructablecode") return Destructable;
    return {};
}

const ObjectData::ObjectRecord* FindObject(const NameResolutionContext& context, const ObjectData::ObjectKey& key,
    bool& invalid, bool& ambiguous) {
    const ObjectData::ObjectRecord* found = nullptr;
    for (const auto& source : context.objects) {
        if (source.type != key.type) continue;
        if (source.state == ProjectSearchState::Ambiguous) {
            ambiguous = true;
            continue;
        }
        if (source.state == ProjectSearchState::Invalid) {
            invalid = true;
            continue;
        }
        if (source.state != ProjectSearchState::Ready && source.state != ProjectSearchState::Partial) continue;
        for (const auto& record : source.records) if (record.key == key) {
            if (found) ambiguous = true;
            found = &record;
        }
    }
    return found;
}

std::optional<std::string> Field(const ObjectData::ObjectRecord& record, std::span<const std::string_view> keys) {
    for (auto key : keys) for (const auto& modification : record.modifications)
        if (Text::LowerAscii(modification.key.fieldId) == key)
            return std::string(Unquote(ObjectData::ValueToDisplayString(modification.value)));
    return std::nullopt;
}

NameResolution MatchedNames(std::string_view raw, std::vector<std::string> names, std::string missing) {
    if (names.empty()) return Failure(raw, NameResolutionState::MissingDefinition, std::move(missing));
    if (names.size() > 1) {
        auto result = Failure(raw, NameResolutionState::Ambiguous, "标识映射到多个名称，无法唯一解析");
        result.candidates = std::move(names);
        return result;
    }
    auto result = Failure(raw, NameResolutionState::Resolved, {});
    result.displayName = PlainName(names.front());
    return result;
}

// symbol 的四字码/编号仍须逐条与真实布局数据匹配，不能仅凭 gg_ 前缀授予身份。
std::string PlacementSymbol(std::string_view prefix, std::string_view rawcode, std::int32_t id) {
    auto number = std::to_string(id);
    if (id >= 0 && number.size() < 4) number.insert(0, 4 - number.size(), '0');
    return std::string(prefix) + std::string(rawcode) + "_" + number;
}

NameResolution PlacedName(const NameResolutionContext& context, std::string_view raw) {
    using enum ObjectData::ObjectType;
    if (context.mapObjects) if (const auto* object = context.mapObjects->Find(raw))
        return ResolveObjectName(context, {object->type == "item" ? Item : Unit, object->rawcode});
    const auto* map = context.mapDocument;
    std::optional<NameResolution> classificationFailure;
    const auto classify = [&](std::string_view rawcode, ObjectData::ObjectType a, ObjectData::ObjectType b) -> std::optional<ObjectData::ObjectType> {
        const auto known = [&](ObjectData::ObjectType type) {
            bool invalid = false, ambiguous = false;
            const auto* record = FindObject(context, {type, std::string(rawcode)}, invalid, ambiguous);
            bool provided = false;
            for (const auto& source : context.objects) if (source.type == type) {
                provided = true;
                if (source.state != ProjectSearchState::Ready && source.state != ProjectSearchState::Missing
                    && !(source.state == ProjectSearchState::Partial && record)) invalid = true;
            }
            if (!provided) invalid = true;
            if ((invalid || ambiguous) && (!classificationFailure || ambiguous)) {
                classificationFailure = Failure(raw, ambiguous ? NameResolutionState::Ambiguous : NameResolutionState::Invalid,
                    ambiguous ? "预放置对象的候选类别存在歧义，不能唯一分类" : "预放置对象的候选类别不可安全解析，不能唯一分类");
            }
            return !invalid && !ambiguous && (record != nullptr ||
                (context.standardObjects && context.standardObjects->Find(type, rawcode)));
        };
        const bool first = known(a), second = known(b);
        if (classificationFailure) return {};
        return first != second ? std::optional(first ? a : b) : std::nullopt;
    };
    std::vector<ObjectData::ObjectKey> found;
    if (map && map->Units()) for (const auto& object : map->Units()->units) {
        if (raw != PlacementSymbol("gg_unit_", object.rawcode, object.creationId)
            && raw != PlacementSymbol("gg_item_", object.rawcode, object.creationId)) continue;
        const auto type = classify(object.rawcode, Unit, Item);
        if (classificationFailure) return *classificationFailure;
        if (type && raw == PlacementSymbol(*type == Item ? "gg_item_" : "gg_unit_", object.rawcode, object.creationId))
            found.push_back({*type, object.rawcode});
    }
    if (map && map->Doodads()) for (const auto& object : map->Doodads()->doodads) {
        if (raw != PlacementSymbol("gg_dest_", object.rawcode, object.creationId)) continue;
        const auto type = classify(object.rawcode, Destructable, Doodad);
        if (classificationFailure) return *classificationFailure;
        if (type == Destructable && raw == PlacementSymbol("gg_dest_", object.rawcode, object.creationId))
            found.push_back({*type, object.rawcode});
    }
    if (found.size() != 1) return Failure(raw, found.empty() ? NameResolutionState::MissingDefinition : NameResolutionState::Ambiguous,
        found.empty() ? "预放置对象未找到，或其类型缺少有效分类依据" : "预放置对象标识重复");
    return ResolveObjectName(context, found.front());
}

} // namespace

std::optional<std::uint32_t> StringReferenceId(std::string_view rawValue) noexcept {
    const auto value = Unquote(rawValue);
    if (!value.starts_with("TRIGSTR_")) return {};
    const auto digits = value.substr(8);
    std::uint32_t id = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return {};
    return id;
}

std::string_view NameResolutionStateName(NameResolutionState state) noexcept {
    switch (state) {
    case NameResolutionState::Resolved: return "resolved";
    case NameResolutionState::Literal: return "literal";
    case NameResolutionState::MissingDefinition: return "missing-definition";
    case NameResolutionState::MissingString: return "missing-string";
    case NameResolutionState::Ambiguous: return "ambiguous";
    case NameResolutionState::Unsupported: return "unsupported";
    case NameResolutionState::Invalid: return "invalid";
    }
    return "invalid";
}

NameResolution ResolveObjectName(const NameResolutionContext& context, const ObjectData::ObjectKey& key) {
    static constexpr std::array<std::string_view, 8> names{"name", "unam", "inam", "anam", "fnam", "gnam", "dnam", "bnam"};
    static constexpr std::array<std::string_view, 7> icons{"art", "uico", "iico", "aart", "fart", "gar1", "bico"};
    if (key.rawcode.size() != 4) return Failure(key.rawcode, NameResolutionState::Invalid, "物体四字码长度必须为四字节");
    std::unordered_set<std::string> visited;
    std::string current = key.rawcode, name, reason;
    // 显式空图标也是当前层的有效覆盖，不能再次从父对象或原版目录补回。
    std::optional<std::string> icon;
    for (std::size_t depth = 0; depth < 256; ++depth) {
        if (!visited.insert(current).second) { reason = "物体继承链循环：" + current; break; }
        bool invalid = false, ambiguous = false;
        const auto* record = FindObject(context, {key.type, current}, invalid, ambiguous);
        if (invalid || ambiguous) return Failure(key.rawcode, ambiguous ? NameResolutionState::Ambiguous : NameResolutionState::Invalid,
            ambiguous ? "当前物体定义有重复来源" : "当前物体定义不可安全解析");
        if (record) {
            if (name.empty()) name = Field(*record, names).value_or(std::string{});
            if (!icon) icon = Field(*record, icons);
            const auto parent = record->parentRawcode.value_or(current);
            if (!parent.empty() && parent != current) { current = parent; continue; }
        }
        const auto* standard = context.standardObjects ? context.standardObjects->Find(key.type, current) : nullptr;
        if (standard) {
            if (name.empty() && standard->nameResolved) name = standard->displayName;
            if (!icon) icon = standard->iconPath;
            if (name.empty()) reason = "原版对象存在，但名称未能解析：" + current;
        } else reason = context.standardObjects ? "缺少物体或父对象定义：" + current : "原版物体目录未加载：" + current;
        break;
    }
    if (visited.size() >= 256 && reason.empty()) reason = "物体继承链超过安全上限";
    if (name.empty()) return Failure(key.rawcode, NameResolutionState::MissingDefinition,
        reason.empty() ? "物体没有可解析的名称" : reason);
    auto result = StringName(context, key.rawcode, name);
    // 解析失败时始终展示引用的原始值，不把 TRIGSTR 当作已解析名称。
    if (result.state == NameResolutionState::Literal) result.state = NameResolutionState::Resolved;
    result.iconPath = icon.value_or(std::string{});
    if (!reason.empty() && result.reason.empty()) result.reason = std::move(reason);
    return result;
}

NameResolution ResolveName(const NameResolutionContext& context, std::string_view rawValue, std::string_view uiType) {
    const auto value = Unquote(rawValue);
    if (value.starts_with("TRIGSTR_")) return StringName(context, rawValue, value);
    std::string canonicalType(uiType);
    if (context.types) for (std::size_t depth = 0; depth < 64 && !TypeFor(canonicalType) && canonicalType != "string"; ++depth) {
        const auto* type = context.types->Find(canonicalType);
        if (!type || type->baseType.empty() || type->baseType == canonicalType) break;
        canonicalType = type->baseType;
    }
    uiType = canonicalType;
    if (uiType == "string") return StringName(context, rawValue, context.preserveStringFormatting ? rawValue : value);
    if (const auto type = TypeFor(uiType)) {
        auto result = ResolveObjectName(context, {*type, std::string(value)});
        result.rawValue = rawValue;
        if (result.state != NameResolutionState::Resolved) result.displayName = rawValue;
        return result;
    }
    if (value.starts_with("gg_unit_") || value.starts_with("gg_item_") || value.starts_with("gg_dest_")) {
        auto result = PlacedName(context, value);
        result.rawValue = rawValue;
        if (result.state != NameResolutionState::Resolved) result.displayName = rawValue;
        return result;
    }
    std::vector<std::string> matches;
    if (value.starts_with("gg_trg_")) {
        if (context.triggers) {
            using Trigger::TriggerDirectoryEditor;
            bool ambiguous = false;
            if (const auto* found = TriggerDirectoryEditor::FindTriggerBySymbol(*context.triggers, value, &ambiguous)) {
                matches.push_back(found->name.text);
            } else if (ambiguous) {
                for (const auto& trigger : context.triggers->triggers)
                    if (trigger.type == 0 && TriggerDirectoryEditor::IsTriggerSymbolOf(value, trigger.name)) matches.push_back(trigger.name.text);
            }
        }
        return MatchedNames(rawValue, std::move(matches), "触发器标识没有对应定义");
    }
    if (value.starts_with("udg_") || uiType == "variable") {
        if (context.triggers) for (const auto& variable : context.triggers->variables)
            if (value == variable.name.text || value == "udg_" + variable.name.text) matches.push_back(variable.name.text);
        return MatchedNames(rawValue, std::move(matches), "全局变量没有对应声明");
    }
    const auto* map = context.mapDocument;
    if (value.starts_with("gg_rct_")) {
        if (map && map->Regions()) for (const auto& region : map->Regions()->entries)
            if (value == "gg_rct_" + Text::ObjectScriptName(region.name)) matches.push_back(region.name);
        if (matches.size() == 1) return StringName(context, rawValue, matches.front());
        return MatchedNames(rawValue, std::move(matches), "区域标识没有对应定义");
    }
    if (value.starts_with("gg_cam_")) {
        if (map && map->Cameras()) for (const auto& camera : map->Cameras()->entries)
            if (value == "gg_cam_" + Text::ObjectScriptName(camera.name)) matches.push_back(camera.name);
        if (matches.size() == 1) return StringName(context, rawValue, matches.front());
        return MatchedNames(rawValue, std::move(matches), "镜头标识没有对应定义");
    }
    if (value.starts_with("gg_snd_")) {
        if (map && map->Sounds()) for (const auto& sound : map->Sounds()->entries)
            if (value == (sound.name.starts_with("gg_snd_") ? sound.name : "gg_snd_" + Text::ObjectScriptName(sound.name)))
                matches.push_back(sound.name);
        if (matches.size() == 1) return StringName(context, rawValue, matches.front());
        return MatchedNames(rawValue, std::move(matches), "声音标识没有对应定义");
    }
    if (value.starts_with("gg_")) return Failure(rawValue, NameResolutionState::Unsupported, "此种生成标识尚不支持名称解析");
    return StringName(context, rawValue, value);
}

} // namespace War3Map::Services
