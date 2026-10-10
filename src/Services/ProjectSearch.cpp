#include "War3MapCore/Services/ProjectSearch.h"
#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/Services/CommandRegistry.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Services {
namespace {
constexpr std::size_t kTextBytes = 2048;
constexpr std::array<MapDataKind, 5> kMapKinds{MapDataKind::Sounds, MapDataKind::Units,
    MapDataKind::Doodads, MapDataKind::Regions, MapDataKind::Cameras};
constexpr std::array<ProjectSearchCategory, 5> kMapCategories{ProjectSearchCategory::Sound,
    ProjectSearchCategory::UnitPlacement, ProjectSearchCategory::DoodadPlacement,
    ProjectSearchCategory::Region, ProjectSearchCategory::Camera};

std::size_t Index(ProjectSearchCategory category) { return static_cast<std::size_t>(category); }
bool Searchable(ProjectSearchState state) { return state == ProjectSearchState::Ready || state == ProjectSearchState::Partial; }
std::string Hex(std::string_view bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output; output.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) { output += digits[value >> 4]; output += digits[value & 15]; }
    return output;
}
std::string ObjectIdentity(const ObjectData::ObjectKey& key) {
    return std::string(ObjectData::ToString(key.type)) + "/" + Hex(key.rawcode);
}
bool ValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        const unsigned count = first >= 0xc2 && first <= 0xdf ? 1 : first >= 0xe0 && first <= 0xef ? 2
            : first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (!count || count > text.size() - i) return false;
        std::uint32_t value = first & (0x7f >> (count + 1));
        for (unsigned j = 0; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < (count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u) || value > 0x10ffffu
            || (value >= 0xd800u && value <= 0xdfffu)) return false;
    }
    return true;
}
std::string Text(std::string_view text, bool& truncated) {
    if (text.size() <= kTextBytes) return std::string(text);
    std::size_t end = kTextBytes - 3;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    truncated = true;
    return std::string(text.substr(0, end)) + "...";
}
ProjectSearchState State(MapDataQueryState state) {
    switch (state) {
    case MapDataQueryState::Ready: return ProjectSearchState::Ready;
    case MapDataQueryState::Partial: return ProjectSearchState::Partial;
    case MapDataQueryState::Missing: return ProjectSearchState::Missing;
    case MapDataQueryState::Invalid: return ProjectSearchState::Invalid;
    case MapDataQueryState::Ambiguous: return ProjectSearchState::Ambiguous;
    default: return ProjectSearchState::Unavailable;
    }
}
std::size_t Bytes(const ProjectSearchRow& row) {
    return 192 + row.stableId.size() + row.title.size() + row.detail.size() + row.contextRevision.size()
        + row.rowKey.size() + (row.object ? row.object->rawcode.size() : 0)
        + row.codeBinding.size() + (row.codeSource ? row.codeSource->size() : 0)
        + (row.rawValue ? row.rawValue->size() : 0);
}

class Search {
public:
    Search(const ProjectSearchContext& context, std::string_view query, const ProjectSearchOptions& options)
        : context_(context), query_(query), options_(options) {
        result_.contextRevision = context.revision;
        for (std::size_t i = 0; i < result_.coverage.size(); ++i)
            result_.coverage[i].category = static_cast<ProjectSearchCategory>(i);
    }
    Result<ProjectSearchResult> Run() {
        Triggers(); Objects(); Standards(); Imports(); CodeSymbols();
        const auto map = MapRows();
        if (!map.Succeeded()) { Result<ProjectSearchResult> failure; failure.diagnostics = map.diagnostics; return failure; }
        result_.complete = std::all_of(result_.coverage.begin(), result_.coverage.end(), [](const auto& coverage) {
            return coverage.state == ProjectSearchState::Ready || coverage.state == ProjectSearchState::Missing
                || coverage.state == ProjectSearchState::QueryTooShort;
        });
        auto result = Result<ProjectSearchResult>::Success(std::move(result_));
        result.diagnostics = std::move(diagnostics_);
        return result;
    }
private:
    ProjectSearchCoverage& Coverage(ProjectSearchCategory category) { return result_.coverage[Index(category)]; }
    void Warn(const char* code, const char* message) {
        if (diagnostics_.size() < 16 && std::none_of(diagnostics_.begin(), diagnostics_.end(),
            [&](const auto& diagnostic) { return diagnostic.code == code; }))
            diagnostics_.push_back({DiagnosticSeverity::Warning, "Project.Search", code, message, std::nullopt});
    }
    bool Visit(ProjectSearchCoverage& coverage) {
        if (coverage.examined >= options_.maxExaminedPerCategory) {
            coverage.scanTruncated = true; coverage.state = ProjectSearchState::Partial;
            return false;
        }
        ++coverage.examined;
        return true;
    }
    void Add(ProjectSearchRow row) {
        auto& coverage = Coverage(row.category);
        row.contextRevision = context_.revision;
        row.title = Text(row.title, row.textTruncated); row.detail = Text(row.detail, row.textTruncated);
        if (row.rawValue) *row.rawValue = Text(*row.rawValue, row.textTruncated);
        if (!ValidUtf8(row.title) || !ValidUtf8(row.detail) || (row.rawValue && !ValidUtf8(*row.rawValue))) {
            coverage.state = ProjectSearchState::Partial;
            return; // 无法展示的旧编码不是“没有此条数据”。
        }
        if (row.textTruncated) coverage.state = ProjectSearchState::Partial;
        const auto score = FuzzyCommandScore(query_, row.title + " " + row.detail
            + (row.rawValue ? " " + *row.rawValue : ""));
        if (!score) return;
        row.score = *score; ++coverage.matches;
        auto& rows = result_.rows;
        const auto position = std::find_if(rows.begin(), rows.end(), [&](const auto& existing) { return existing.score < row.score; });
        rows.insert(position, std::move(row));
        while (rows.size() > options_.limit) {
            Coverage(rows.back().category).resultsTruncated = true; rows.pop_back(); result_.hasMore = true;
        }
        std::size_t bytes = 0; for (const auto& entry : rows) bytes += Bytes(entry);
        while (!rows.empty() && bytes > options_.maxResultBytes) {
            bytes -= Bytes(rows.back()); Coverage(rows.back().category).resultsTruncated = true;
            rows.pop_back(); result_.hasMore = true;
        }
    }
    bool NameReject(ProjectSearchCoverage& coverage, const char* code, const char* message) {
        coverage.state = ProjectSearchState::Partial;
        Warn(code, message);
        return false;
    }
    bool NameCharge(std::size_t nodes, std::size_t bytes, ProjectSearchCoverage& coverage) {
        if (nodes > options_.maxTriggerNodes - result_.nameNodesCharged)
            return NameReject(coverage, "PROJECT_SEARCH_NAME_NODE_BUDGET", "名称解析超过全局节点预算；保留原值，未扫描后缀或推断唯一名称。");
        if (bytes > options_.maxTriggerTextBytes - result_.nameTextBytesCharged)
            return NameReject(coverage, "PROJECT_SEARCH_NAME_TEXT_BUDGET", "名称解析超过全局字节预算；保留原值，未转换超预算正文。");
        result_.nameNodesCharged += nodes; result_.nameTextBytesCharged += bytes;
        return true;
    }
    bool NameText(std::string_view text, ProjectSearchCoverage& coverage, std::size_t passes = 1) {
        if (text.size() > kTextBytes)
            return NameReject(coverage, "PROJECT_SEARCH_NAME_FIELD_BUDGET", "名称解析所需字段超过2048字节；保留原值，不先复制或解码大字段。");
        if (!NameCharge(0, text.size() * passes, coverage)) return false;
        if (text.find('\0') != std::string_view::npos || !ValidUtf8(text))
            return NameReject(coverage, "PROJECT_SEARCH_NAME_TEXT_INVALID", "名称解析所需字段含坏UTF8或NUL；保留原值与部分覆盖状态。");
        return true;
    }
    bool NameCount(std::size_t count, ProjectSearchCoverage& coverage) {
        return count <= options_.maxExaminedPerCategory || NameReject(coverage,
            "PROJECT_SEARCH_NAME_DOMAIN_BUDGET", "名称解析所需完整域超过扫描窗口；未扫描或截取半个域来猜唯一身份。");
    }
    bool NameValue(const ObjectData::Value& value, ProjectSearchCoverage& coverage) {
        return std::visit([&](const auto& current) {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, std::string>) return NameText(current, coverage, 16);
            else if constexpr (std::is_same_v<T, ObjectData::RawcodeValue> || std::is_same_v<T, ObjectData::EnumValue>
                || std::is_same_v<T, ObjectData::AssetPathValue> || std::is_same_v<T, ObjectData::LocalizedStringValue>)
                return NameText(current.value, coverage, 16);
            else if constexpr (std::is_same_v<T, ObjectData::RawcodeListValue> || std::is_same_v<T, ObjectData::UnknownBytesValue>)
                return NameReject(coverage, "PROJECT_SEARCH_NAME_VALUE_UNSUPPORTED", "名称域含复合或不透明字段；未拼接列表或转换原始字节来猜显示名称。");
            else return NameCharge(0, 64, coverage); // 共用标量格式化的固定上界。
        }, value);
    }
    bool NameStrings(ProjectSearchCoverage& coverage, bool mapOnly = false, std::size_t repetitions = 1) {
        const auto* strings = mapOnly ? nullptr : context_.strings;
        if (!strings && context_.mapData && context_.mapData->OwnedDocument()->Strings())
            strings = &*context_.mapData->OwnedDocument()->Strings();
        if (!strings) return true;
        if (!NameCount(strings->entries.size(), coverage) || !NameCharge(strings->entries.size() * (repetitions + 1), 0, coverage)) return false;
        // 包括不匹配项；核对和解析器重扫/解码均计费，不把半个WTS当完整表。
        for (const auto& entry : strings->entries) if (!NameText(entry.value, coverage, 16 * repetitions)) return false;
        return true;
    }
    bool NameObjectDomain(const ObjectData::ObjectKey& key, ProjectSearchCoverage& coverage) {
        if (key.rawcode.size() != 4 || !NameText(key.rawcode, coverage))
            return NameReject(coverage, "PROJECT_SEARCH_NAME_OBJECT_INVALID", "名称解析物体四字码无效；保留原值。");
        const auto firstNodes = result_.nameNodesCharged, firstBytes = result_.nameTextBytesCharged;
        std::unordered_map<std::string, const ObjectData::ObjectRecord*> records;
        std::size_t total = 0;
        for (const auto& source : context_.objects) {
            if (source.type != key.type) continue;
            if (source.state == ProjectSearchState::Invalid || source.state == ProjectSearchState::Ambiguous)
                return NameReject(coverage, "PROJECT_SEARCH_NAME_OBJECT_INVALID", "名称解析物编来源无效或歧义；未选择其中一份。");
            if (!Searchable(source.state)) continue;
            if (source.records.size() > options_.maxExaminedPerCategory - total)
                return NameReject(coverage, "PROJECT_SEARCH_NAME_DOMAIN_BUDGET", "名称解析所需完整物编类别超过窗口；保留原值，未建部分唯一索引。");
            total += source.records.size();
            for (const auto& record : source.records) {
                if (!NameCharge(1, 0, coverage) || !NameText(record.key.rawcode, coverage)
                    || (record.parentRawcode && !NameText(*record.parentRawcode, coverage))) return false;
                if (record.key.type != key.type || record.key.rawcode.size() != 4
                    || (record.parentRawcode && !record.parentRawcode->empty() && record.parentRawcode->size() != 4)
                    || !records.emplace(record.key.rawcode, &record).second)
                    return NameReject(coverage, "PROJECT_SEARCH_NAME_OBJECT_INVALID", "完整物编名称域含重复或失效身份；未选择第一份。");
                if (!NameCount(record.modifications.size(), coverage)) return false;
                for (const auto& modification : record.modifications) {
                    // 共用Field最多8个名称键与7个图标键；不匹配字段同样扣账。
                    if (!NameCharge(16, 0, coverage) || !NameText(modification.key.fieldId, coverage, 32)
                        || !NameValue(modification.value, coverage)) return false;
                }
            }
        }
        std::unordered_set<std::string> visited;
        std::string current = key.rawcode;
        for (std::size_t depth = 0; depth < 16; ++depth) {
            if (!NameCharge(1, 0, coverage)) return false;
            if (!visited.insert(current).second)
                return NameReject(coverage, "PROJECT_SEARCH_NAME_INHERITANCE_CYCLE", "名称继承链循环；保留原始四字码，不进入256层共用遍历。");
            const auto found = records.find(current);
            if (found != records.end() && found->second->parentRawcode && !found->second->parentRawcode->empty()
                && *found->second->parentRawcode != current) {
                current = *found->second->parentRawcode;
                continue;
            }
            if (Searchable(context_.standardState) && context_.standardObjects) {
                if (!NameCharge(1, 0, coverage)) return false;
                if (const auto* standard = context_.standardObjects->Find(key.type, current))
                    if (!NameText(standard->displayName, coverage, 16) || !NameText(standard->iconPath, coverage, 16)) return false;
            }
            const auto proofNodes = result_.nameNodesCharged - firstNodes;
            const auto proofBytes = result_.nameTextBytesCharged - firstBytes;
            // 先预留共用解析器每层完整查找/Field/转换的保守上界；不足则完全不调用它。
            if (!NameCharge(proofNodes * (depth + 2), proofBytes * (depth + 2), coverage)) return false;
            return NameStrings(coverage);
        }
        return NameReject(coverage, "PROJECT_SEARCH_NAME_INHERITANCE_BUDGET", "名称继承链超过搜索的16层预检上限；保留原值，不继续长链解析。");
    }
    static std::optional<ObjectData::ObjectType> NameObjectType(std::string_view type) {
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
    bool NamePreflight(std::string_view raw, std::string_view type, ProjectSearchCoverage& coverage) {
        if (!NameCharge(1, 0, coverage) || !NameText(raw, coverage, 16) || !NameText(type, coverage, 8)) return false;
        auto value = raw;
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        if (value.starts_with("TRIGSTR_")) return NameStrings(coverage);
        std::string canonical(type);
        std::unordered_set<std::string> seenTypes;
        if (context_.types) for (std::size_t depth = 0; !NameObjectType(canonical) && canonical != "string"; ++depth) {
            if (depth == 16 || !seenTypes.insert(canonical).second)
                return NameReject(coverage, "PROJECT_SEARCH_NAME_TYPE_BUDGET", "名称UI类型链循环或超过16层；保留原值。");
            if (!NameCharge(1, 0, coverage)) return false;
            const auto* info = context_.types->Find(canonical);
            if (!info || info->baseType.empty() || info->baseType == canonical) break;
            if (!NameText(info->baseType, coverage, 8)) return false;
            canonical = info->baseType;
        }
        if (canonical == "string") return NameStrings(coverage);
        if (const auto objectType = NameObjectType(canonical)) return NameObjectDomain({*objectType, std::string(value)}, coverage);
        if (value.starts_with("gg_unit_") || value.starts_with("gg_item_") || value.starts_with("gg_dest_")) {
            const auto* map = context_.mapData ? context_.mapData->OwnedDocument().get() : nullptr;
            if (map && map->Units()) {
                if (!NameCount(map->Units()->units.size(), coverage) || !NameCharge(map->Units()->units.size() * 4, 0, coverage)) return false;
                for (const auto& item : map->Units()->units) if (item.rawcode.size() != 4 || !NameText(item.rawcode, coverage, 8))
                    return NameReject(coverage, "PROJECT_SEARCH_NAME_PLACEMENT_INVALID", "摆放名称域存在无效四字码；未用部分摆放猜身份。");
            }
            if (map && map->Doodads()) {
                if (!NameCount(map->Doodads()->doodads.size(), coverage) || !NameCharge(map->Doodads()->doodads.size() * 4, 0, coverage)) return false;
                for (const auto& item : map->Doodads()->doodads) if (item.rawcode.size() != 4 || !NameText(item.rawcode, coverage, 8))
                    return NameReject(coverage, "PROJECT_SEARCH_NAME_PLACEMENT_INVALID", "摆放名称域存在无效四字码；未用部分摆放猜身份。");
            }
            if (value.size() < 12) return true;
            using enum ObjectData::ObjectType;
            const auto rawcode = std::string(value.substr(8, 4));
            return value.starts_with("gg_dest_")
                ? NameObjectDomain({Destructable, rawcode}, coverage) && NameObjectDomain({Doodad, rawcode}, coverage)
                : NameObjectDomain({Unit, rawcode}, coverage) && NameObjectDomain({Item, rawcode}, coverage);
        }
        if (value.starts_with("gg_trg_") && context_.triggers) {
            if (!NameCount(context_.triggers->triggers.size(), coverage) || !NameCharge(context_.triggers->triggers.size() * 2, 0, coverage)) return false;
            for (const auto& item : context_.triggers->triggers) if (!NameText(item.name.text, coverage, 8)) return false;
        } else if ((value.starts_with("udg_") || canonical == "variable") && context_.triggers) {
            if (!NameCount(context_.triggers->variables.size(), coverage) || !NameCharge(context_.triggers->variables.size() * 2, 0, coverage)) return false;
            for (const auto& item : context_.triggers->variables) if (!NameText(item.name.text, coverage, 8)) return false;
        } else {
            const auto* map = context_.mapData ? context_.mapData->OwnedDocument().get() : nullptr;
            const auto names = [&](const auto& entries) {
                if (!NameCount(entries.size(), coverage) || !NameCharge(entries.size() * 2, 0, coverage)) return false;
                for (const auto& item : entries) if (!NameText(item.name, coverage, 8)) return false;
                return NameStrings(coverage);
            };
            if (value.starts_with("gg_rct_") && map && map->Regions()) return names(map->Regions()->entries);
            if (value.starts_with("gg_cam_") && map && map->Cameras()) return names(map->Cameras()->entries);
            if (value.starts_with("gg_snd_") && map && map->Sounds()) return names(map->Sounds()->entries);
        }
        return true;
    }
    std::optional<NameResolution> SafeName(std::string_view raw, std::string_view type, ProjectSearchCoverage& coverage) {
        if (!NamePreflight(raw, type, coverage)) return {};
        NameResolutionContext names;
        names.objects = context_.objects; names.standardObjects = Searchable(context_.standardState) ? context_.standardObjects : nullptr;
        names.types = context_.types; names.strings = context_.strings; names.triggers = context_.triggers;
        names.mapDocument = context_.mapData ? context_.mapData->OwnedDocument().get() : nullptr;
        return ResolveName(names, raw, type);
    }
    std::string ObjectName(const ObjectData::ObjectRecord& record, ProjectSearchCoverage& coverage) {
        if (!NameObjectDomain(record.key, coverage)) return record.key.rawcode;
        NameResolutionContext names;
        names.objects = context_.objects;
        names.standardObjects = Searchable(context_.standardState) ? context_.standardObjects : nullptr;
        names.strings = context_.strings;
        const auto resolved = ResolveObjectName(names, record.key);
        if (resolved.state != NameResolutionState::Resolved || !resolved.reason.empty()) coverage.state = ProjectSearchState::Partial;
        return ValidUtf8(resolved.displayName) ? resolved.displayName : "0x" + Hex(record.key.rawcode);
    }
    bool TriggerText(std::string_view text, ProjectSearchCoverage& coverage) {
        if (text.size() > options_.maxTriggerTextBytes - coverage.textBytesExamined) {
            Warn("PROJECT_SEARCH_TRIGGER_TEXT_BUDGET", "触发器文本超过扫描字节预算；参数搜索仅有部分覆盖。");
            coverage.scanTruncated = true; coverage.state = ProjectSearchState::Partial; return false;
        }
        coverage.textBytesExamined += text.size();
        if (text.find('\0') != std::string_view::npos || !ValidUtf8(text)) {
            Warn("PROJECT_SEARCH_TRIGGER_TEXT_INVALID", "当前触发器文本含坏UTF8或NUL；保留来源，不输出不可信参数结果。");
            coverage.state = ProjectSearchState::Partial; return false;
        }
        return true;
    }
    struct Parameter {
        std::uint32_t argument = 0, eca = 0;
        std::size_t position = 0;
        bool arrayIndex = false;
    };
    struct TriggerParameters {
        const Trigger::TriggerRecord* trigger = nullptr;
        std::vector<Parameter> parameters;
        bool trusted = true;
    };
    void TriggerGraph(TriggerParameters& group, ProjectSearchCoverage& coverage,
        std::unordered_map<StableId, StableId>& nodeOwners, std::unordered_set<StableId>& invalidOwners) {
        const auto& doc = *context_.triggers;
        struct Frame {
            bool argument = false;
            std::uint32_t index = 0, eca = 0;
            std::size_t position = 0, next = 0;
            bool arrayIndex = false, entered = false;
        };
        std::vector<Frame> stack;
        std::unordered_set<std::uint64_t> seen;
        const auto reject = [&] {
            group.trusted = false; coverage.state = ProjectSearchState::Partial;
            Warn("PROJECT_SEARCH_TRIGGER_GRAPH_INVALID", "触发器节点存在循环、重复身份、失效索引或来源不符；该触发器参数导航不可用。");
        };
        for (const auto root : group.trigger->ecas) {
            stack.push_back({false, root});
            while (!stack.empty()) {
                auto& frame = stack.back();
                if (!frame.entered) {
                    if (coverage.graphNodesExamined >= options_.maxTriggerNodes) {
                        Warn("PROJECT_SEARCH_TRIGGER_NODE_BUDGET", "触发器节点超过扫描预算；未扫描部分的参数不能判为没有匹配。");
                        coverage.scanTruncated = true; coverage.state = ProjectSearchState::Partial; group.trusted = false; return;
                    }
                    ++coverage.graphNodesExamined;
                    const auto key = (static_cast<std::uint64_t>(frame.argument) << 32) | frame.index;
                    if (!seen.insert(key).second) { reject(); stack.pop_back(); continue; }
                    if ((frame.argument && frame.index >= doc.argumentNodes.size())
                        || (!frame.argument && frame.index >= doc.ecaNodes.size())) {
                        reject(); stack.pop_back(); continue;
                    }
                    const auto id = frame.argument ? doc.argumentNodes[frame.index].id : doc.ecaNodes[frame.index].id;
                    const auto [owner, fresh] = nodeOwners.emplace(id, group.trigger->id);
                    if (!id || !fresh) {
                        invalidOwners.insert(group.trigger->id);
                        if (!fresh) invalidOwners.insert(owner->second);
                        reject(); stack.pop_back(); continue;
                    }
                    frame.entered = true;
                    if (frame.argument) {
                        const auto& argument = doc.argumentNodes[frame.index];
                        if ((argument.insertCall != 0) != argument.callEca.has_value()
                            || (argument.insertIndex != 0) != argument.indexArgument.has_value()) reject();
                        if (!TriggerText(argument.value.text, coverage)) group.trusted = false;
                        group.parameters.push_back({frame.index, frame.eca, frame.position, frame.arrayIndex});
                    } else {
                        const auto& eca = doc.ecaNodes[frame.index];
                        if (eca.sourceTriggerId != group.trigger->id) reject();
                        if (!TriggerText(eca.name.text, coverage)) group.trusted = false;
                    }
                }
                // 每次只推进一条边；恶意超大 children/arguments 不会制造无界待处理队列。
                if (frame.argument) {
                    const auto& argument = doc.argumentNodes[frame.index];
                    if (frame.next == 0) {
                        frame.next = 1;
                        if (argument.callEca) { stack.push_back({false, *argument.callEca}); continue; }
                    }
                    if (frame.next == 1) {
                        frame.next = 2;
                        if (argument.indexArgument) {
                            stack.push_back({true, *argument.indexArgument, frame.eca, frame.position, 0, true}); continue;
                        }
                    }
                    stack.pop_back();
                } else {
                    const auto& eca = doc.ecaNodes[frame.index];
                    if (frame.next < eca.arguments.size()) {
                        const auto position = frame.next++;
                        stack.push_back({true, eca.arguments[position], frame.index, position}); continue;
                    }
                    const auto child = frame.next - eca.arguments.size();
                    if (child < eca.children.size()) {
                        ++frame.next; stack.push_back({false, eca.children[child]}); continue;
                    }
                    stack.pop_back();
                }
            }
            if (coverage.scanTruncated) return;
        }
    }
    void AddParameters(const TriggerParameters& group, ProjectSearchCoverage& coverage) {
        const auto& doc = *context_.triggers;
        for (const auto& parameter : group.parameters) {
            if (!Visit(coverage)) return;
            const auto& argument = doc.argumentNodes[parameter.argument];
            const auto& eca = doc.ecaNodes[parameter.eca];
            if (argument.value.text.empty()) continue;
            ProjectSearchRow row; row.category = ProjectSearchCategory::Trigger;
            row.sourceId = group.trigger->id; row.ecaId = eca.id; row.argumentId = argument.id;
            row.stableId = "trigger/" + std::to_string(group.trigger->id) + "/eca/" + std::to_string(eca.id)
                + "/argument/" + std::to_string(argument.id);
            row.rawValue = Text(argument.value.text, row.textTruncated); row.title = *row.rawValue;
            // 在完整域与全局名称预算核对后才调用共用解析；不推导递归Null或局部变量。
            std::string type;
            bool trustedType = true;
            if (parameter.arrayIndex) type = "integer";
            else if (context_.types && parameter.position < 64) {
                if (const auto* definition = context_.types->Catalog().Find(eca.type, eca.name.text)) {
                    const auto& arguments = definition->Active().arguments;
                    bool trusted = NameCount(arguments.size(), coverage) && NameCharge(arguments.size() * 2, 0, coverage);
                    if (trusted) for (const auto& item : arguments) if (!NameText(item.type, coverage, 2)) { trusted = false; break; }
                    if (trusted) type = Trigger::TriggerTypeSystem::DeclaredType(*definition, parameter.position);
                    else trustedType = false;
                }
            }
            if (!row.textTruncated && trustedType) {
                const auto display = SafeName(*row.rawValue, type, coverage);
                if (display && display->displayName.size() <= kTextBytes && ValidUtf8(display->displayName)) row.title = display->displayName;
                else if (!display) coverage.state = ProjectSearchState::Partial;
                else row.textTruncated = true;
                if (display && !display->reason.empty()) coverage.state = ProjectSearchState::Partial;
            } else coverage.state = ProjectSearchState::Partial;
            row.detail = Text(group.trigger->name.text, row.textTruncated) + " · 参数 · "
                + Text(eca.name.text, row.textTruncated) + (parameter.arrayIndex ? " · 数组下标" : "");
            Add(std::move(row));
        }
    }
    void Triggers() {
        auto& triggers = Coverage(ProjectSearchCategory::Trigger);
        auto& variables = Coverage(ProjectSearchCategory::Variable);
        triggers.state = variables.state = context_.triggerState;
        if (!Searchable(context_.triggerState)) return;
        if (!context_.triggers) { triggers.state = variables.state = ProjectSearchState::Invalid; return; }
        variables.totalRows = context_.triggers->variables.size();
        std::unordered_set<StableId> triggerIds, variableIds;
        std::unordered_map<StableId, StableId> nodeOwners;
        std::unordered_set<StableId> invalidOwners;
        std::vector<TriggerParameters> groups;
        for (const auto& trigger : context_.triggers->triggers) {
            if (!Visit(triggers)) break;
            if (!trigger.id || !triggerIds.insert(trigger.id).second) {
                Warn("PROJECT_SEARCH_TRIGGER_ID_INVALID", "触发器身份为空或重复；相关参数不能用于导航。");
                triggers.state = ProjectSearchState::Partial; invalidOwners.insert(trigger.id); continue;
            }
            if (!TriggerText(trigger.name.text, triggers)) continue;
            ProjectSearchRow row; row.category = ProjectSearchCategory::Trigger; row.sourceId = trigger.id;
            row.stableId = "trigger/" + std::to_string(trigger.id); row.title = Text(trigger.name.text, row.textTruncated);
            row.detail = "触发器 · " + std::to_string(trigger.id); Add(std::move(row));
            groups.push_back({&trigger, {}, true});
            TriggerGraph(groups.back(), triggers, nodeOwners, invalidOwners);
        }
        std::size_t total = context_.triggers->triggers.size();
        for (const auto& group : groups) {
            if (!group.trusted || invalidOwners.contains(group.trigger->id)) continue;
            total += group.parameters.size(); AddParameters(group, triggers);
        }
        if (triggers.state == ProjectSearchState::Ready && !triggers.scanTruncated) triggers.totalRows = total;
        for (const auto& variable : context_.triggers->variables) {
            if (!Visit(variables)) break;
            if (!variable.id || !variableIds.insert(variable.id).second) { variables.state = ProjectSearchState::Partial; continue; }
            ProjectSearchRow row; row.category = ProjectSearchCategory::Variable; row.sourceId = variable.id;
            row.stableId = "variable/" + std::to_string(variable.id); row.title = Text(variable.name.text, row.textTruncated);
            row.detail = "变量 · " + variable.typeName.text.substr(0, kTextBytes) + (variable.isArray ? "[]" : "");
            if (variable.hasDefault) {
                const auto initial = SafeName(variable.value.text, variable.typeName.text, variables);
                row.detail += " · " + Text(initial ? std::string_view(initial->displayName) : std::string_view(variable.value.text), row.textTruncated);
                if (initial && !initial->reason.empty()) variables.state = ProjectSearchState::Partial;
            }
            Add(std::move(row));
        }
    }
    void CodeSymbols() {
        auto& coverage = Coverage(ProjectSearchCategory::CodeSymbol);
        coverage.state = context_.codeState;
        if (!Searchable(coverage.state)) return;
        if (!context_.codeDeclarations || context_.codeBinding.empty() || context_.codeBinding.size() > 256) {
            coverage.state = ProjectSearchState::Invalid; return;
        }
        if (!context_.codeDeclarations->completed) coverage.state = ProjectSearchState::Partial;
        coverage.totalRows = context_.codeDeclarations->symbols.size();
        for (const auto& symbol : context_.codeDeclarations->symbols) {
            if (!Visit(coverage)) break;
            if (symbol.source.empty() || symbol.source.size() > 1024 || !symbol.line) {
                coverage.state = ProjectSearchState::Partial; continue;
            }
            ProjectSearchRow row;
            row.category = ProjectSearchCategory::CodeSymbol;
            row.stableId = "code/" + Hex(symbol.source) + "/" + std::to_string(symbol.line) + "/"
                + std::to_string(symbol.byteColumn) + "/" + Hex(symbol.name);
            row.title = symbol.name;
            row.detail = "代码 · " + symbol.source + ":" + std::to_string(symbol.line) + " · " + symbol.type;
            row.codeSource = symbol.source; row.codeLine = symbol.line; row.codeColumn = symbol.byteColumn;
            row.codeBinding = context_.codeBinding;
            Add(std::move(row));
        }
    }
    void Objects() {
        auto& coverage = Coverage(ProjectSearchCategory::MapObject);
        std::array<bool, 7> types{}; std::size_t total = 0; bool available = false;
        for (const auto& source : context_.objects) {
            const auto type = static_cast<std::size_t>(source.type);
            if (type >= types.size() || types[type]) { coverage.state = ProjectSearchState::Ambiguous; return; }
            types[type] = true;
            if (!Searchable(source.state)) continue;
            available = true; total += source.records.size();
            if (source.state == ProjectSearchState::Partial) coverage.state = ProjectSearchState::Partial;
        }
        if (!available) {
            if (std::all_of(types.begin(), types.end(), [](bool provided) { return provided; })
                && std::all_of(context_.objects.begin(), context_.objects.end(),
                [](const auto& source) { return source.state == ProjectSearchState::Missing; })) coverage.state = ProjectSearchState::Missing;
            return;
        }
        if (coverage.state != ProjectSearchState::Partial) coverage.state = ProjectSearchState::Ready;
        const bool allTypes = std::all_of(types.begin(), types.end(), [](bool provided) { return provided; });
        const bool unknown = std::any_of(context_.objects.begin(), context_.objects.end(), [](const auto& source) {
            return !Searchable(source.state) && source.state != ProjectSearchState::Missing;
        });
        if (!allTypes || unknown) coverage.state = ProjectSearchState::Partial;
        else coverage.totalRows = total;
        // 只为本次允许检查的地图记录建借用指针索引；标准目录不复制、不建副本。
        std::size_t indexed = 0;
        for (const auto& source : context_.objects) if (Searchable(source.state)) for (const auto& record : source.records) {
            if (indexed >= options_.maxExaminedPerCategory) break;
            if (record.key.type != source.type || record.key.rawcode.size() != 4) { coverage.state = ProjectSearchState::Invalid; objectIndex_.clear(); return; }
            if (!objectIndex_.emplace(ObjectIdentity(record.key), &record).second) { coverage.state = ProjectSearchState::Ambiguous; objectIndex_.clear(); return; }
            ++indexed;
        }
        for (const auto& source : context_.objects) if (Searchable(source.state)) for (const auto& record : source.records) {
            if (!Visit(coverage)) return;
            ProjectSearchRow row; row.category = ProjectSearchCategory::MapObject; row.object = record.key;
            row.sourceId = record.stableId; row.stableId = "map-object/" + ObjectIdentity(record.key);
            row.title = ObjectName(record, coverage);
            row.detail = std::string(ProjectSearchCategoryName(row.category)) + " · " + std::string(ObjectData::ToString(source.type))
                + " · " + record.key.rawcode + (source.origin == ProjectSearchObjectOrigin::Native ? " · 原生" : " · LNI");
            Add(std::move(row));
        }
    }
    void Standards() {
        auto& coverage = Coverage(ProjectSearchCategory::StandardObject);
        const auto characters = std::count_if(query_.begin(), query_.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
        if (characters < 2) { coverage.state = ProjectSearchState::QueryTooShort; return; }
        coverage.state = context_.standardState;
        if (!Searchable(coverage.state)) return;
        if (!context_.standardObjects) { coverage.state = ProjectSearchState::Invalid; return; }
        const auto entries = context_.standardObjects->Entries(); coverage.totalRows = entries.size();
        for (const auto& entry : entries) {
            if (!Visit(coverage)) break;
            if (!entry.editorVisible) continue;
            ProjectSearchRow row; row.category = ProjectSearchCategory::StandardObject; row.object = entry.key;
            row.stableId = "standard-object/" + ObjectIdentity(entry.key);
            row.title = NameCharge(1, 0, coverage) && NameText(entry.displayName, coverage, 2)
                ? Text(entry.displayName, row.textTruncated) : entry.key.rawcode;
            row.detail = "原版物体 · " + std::string(ObjectData::ToString(entry.key.type)) + " · " + entry.key.rawcode;
            if (!entry.nameResolved) { coverage.state = ProjectSearchState::Partial; row.detail += " · 名称未解析"; }
            Add(std::move(row));
        }
    }
    void Imports() {
        auto& coverage = Coverage(ProjectSearchCategory::ImportedFile); coverage.state = context_.importState;
        if (!Searchable(coverage.state)) return;
        if (context_.usesImportedFiles) {
            coverage.totalRows = context_.currentFiles.size();
            std::unordered_set<std::string> ids;
            for (std::size_t i = 0; i < context_.currentFiles.size(); ++i) {
                if (!Visit(coverage)) break;
                const auto& file = context_.currentFiles[i];
                if (file.stableId.empty() || file.stableId.size() > 512 || !ValidUtf8(file.stableId)
                    || !ids.insert(std::string(file.stableId)).second) { coverage.state = ProjectSearchState::Partial; continue; }
                ProjectSearchRow row; row.category = ProjectSearchCategory::ImportedFile; row.sourceIndex = i;
                row.stableId = "import/" + std::string(file.stableId); row.rowKey = file.stableId;
                row.title = Text(file.path, row.textTruncated); row.detail = "输入文件 · 当前编辑"; Add(std::move(row));
            }
            return;
        }
        if (!context_.imports) { coverage.state = ProjectSearchState::Invalid; return; }
        const auto& entries = context_.imports->Entries(); coverage.totalRows = entries.size();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!Visit(coverage)) break;
            ProjectSearchRow row; row.category = ProjectSearchCategory::ImportedFile; row.sourceIndex = i;
            row.stableId = "import/" + std::to_string(i); row.rowKey = row.stableId;
            row.title = Text(entries[i].path, row.textTruncated); row.detail = "输入文件 · flags=" + std::to_string(entries[i].flags); Add(std::move(row));
        }
    }
    std::string PlacementName(const MapDataQueryRow& record, MapDataKind kind, ProjectSearchCoverage& coverage) {
        const std::array<ObjectData::ObjectType, 3> types{kind == MapDataKind::Units ? ObjectData::ObjectType::Unit
            : ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Item, ObjectData::ObjectType::Destructable};
        std::string name; unsigned matches = 0;
        for (const auto type : types) {
            if ((kind == MapDataKind::Units && type == ObjectData::ObjectType::Destructable)
                || (kind == MapDataKind::Doodads && type == ObjectData::ObjectType::Item)) continue;
            const auto found = objectIndex_.find(ObjectIdentity({type, record.rawcodeBytes}));
            if (found != objectIndex_.end()) { name = ObjectName(*found->second, coverage); ++matches; }
            else if (context_.standardObjects && Searchable(context_.standardState)) {
                if (const auto* entry = context_.standardObjects->Find(type, record.rawcodeBytes)) {
                    if (!entry->nameResolved) coverage.state = ProjectSearchState::Partial;
                    if (!NameCharge(1, 0, coverage) || !NameText(entry->displayName, coverage, 2)) return record.rawcodeBytes;
                    name = entry->displayName; ++matches;
                }
            }
        }
        if (matches == 1) return name;
        if (matches > 1) { coverage.state = ProjectSearchState::Partial; return record.rawcodeBytes + "（类型歧义）"; }
        return record.rawcodeBytes.empty() ? std::string(ProjectSearchCategoryName(coverage.category)) : record.rawcodeBytes;
    }
    std::optional<std::string_view> MapRawName(MapDataKind kind, std::size_t index) const {
        const auto& document = *context_.mapData->OwnedDocument();
        if (kind == MapDataKind::Sounds && document.Sounds() && index < document.Sounds()->entries.size()) return document.Sounds()->entries[index].name;
        if (kind == MapDataKind::Regions && document.Regions() && index < document.Regions()->entries.size()) return document.Regions()->entries[index].name;
        if (kind == MapDataKind::Cameras && document.Cameras() && index < document.Cameras()->entries.size()) return document.Cameras()->entries[index].name;
        return {};
    }
    bool MapNameBatch(MapDataKind kind, std::size_t first, std::size_t count, ProjectSearchCoverage& coverage) {
        const auto& document = *context_.mapData->OwnedDocument();
        std::size_t stringReferences = 0;
        for (std::size_t index = first; index < first + count; ++index) {
            const auto name = MapRawName(kind, index);
            if (!name) return NameReject(coverage, "PROJECT_SEARCH_NAME_MAP_INCONSISTENT", "地图成员计数与可信名称行不符；未生成替代身份。");
            if (!NameCharge(1, 0, coverage) || !NameText(*name, coverage, 4)) return false;
            if (StringReferenceId(*name)) ++stringReferences;
            if (kind == MapDataKind::Regions) {
                if (!NameText(document.Regions()->entries[index].ambientSound, coverage, 4)) return false;
            } else if (kind == MapDataKind::Sounds) {
                const auto& sound = document.Sounds()->entries[index];
                if (!NameText(sound.filePath, coverage, 4) || !NameText(sound.eaxSetting, coverage, 4)) return false;
                if (sound.extended) {
                    const auto& extended = *sound.extended;
                    for (const auto* text : {&extended.repeatVariableName, &extended.soundName, &extended.repeatSoundPath,
                            &extended.unknown2, &extended.unknown6, &extended.unknown7, &extended.facialAnimationLabel,
                            &extended.facialAnimationGroupLabel, &extended.facialAnimationSetFilepath})
                        if (!NameText(*text, coverage, 4)) return false;
                    if (extended.unknown5 && !NameText(*extended.unknown5, coverage, 4)) return false;
                }
            }
        }
        // Typed page内部也走N1；按本页引用数预付actual snapshot的完整WTS重扫上界。
        return !stringReferences || NameStrings(coverage, true, stringReferences);
    }
    void RawMapNames(MapDataKind kind, std::size_t first, std::size_t count, ProjectSearchCoverage& coverage) {
        for (std::size_t index = first; index < first + count; ++index) {
            if (!Visit(coverage)) break;
            const auto name = MapRawName(kind, index);
            if (!name || name->size() > kTextBytes || name->find('\0') != std::string_view::npos || !ValidUtf8(*name)) {
                NameReject(coverage, "PROJECT_SEARCH_NAME_MAP_ROW_OMITTED", "名称行无法产生有界可信身份；该行省略，未猜导航目标。");
                continue;
            }
            // 身份仍来自同一不可变snapshot的共用row-key，不重建或裁剪MapDocument。
            const auto identity = MapData::MapFieldRowKey(*context_.mapData->OwnedDocument(), kind, index);
            if (!identity.Succeeded() || !identity.value) { coverage.state = ProjectSearchState::Partial; continue; }
            ProjectSearchRow row; row.category = coverage.category; row.mapKind = kind; row.sourceIndex = index;
            row.rowKey = *identity.value; row.title = *name;
            row.stableId = "map/" + std::to_string(static_cast<unsigned>(kind)) + "/" + std::to_string(index) + "/" + row.rowKey;
            row.detail = std::string(ProjectSearchCategoryName(row.category)) + " · " + std::to_string(index) + " · 仅原始名称";
            Add(std::move(row));
        }
    }
    Status MapRows() {
        if (!context_.mapData) return Status::Ok();
        const auto description = DescribeMapData(*context_.mapData, context_.mapDataBinding);
        if (!description.Succeeded()) return {description.diagnostics};
        for (std::size_t i = 0; i < kMapKinds.size(); ++i) {
            const auto kind = kMapKinds[i]; auto& coverage = Coverage(kMapCategories[i]);
            const auto& member = description.value->members[static_cast<std::size_t>(kind)];
            coverage.state = State(member.state); coverage.totalRows = member.recordCount;
            if (!Searchable(coverage.state)) continue;
            std::optional<MapDataQueryCursor> cursor;
            do {
                if (coverage.examined >= options_.maxExaminedPerCategory) {
                    if (coverage.totalRows.value_or(0) > coverage.examined) { coverage.state = ProjectSearchState::Partial; coverage.scanTruncated = true; }
                    break;
                }
                const auto first = static_cast<std::size_t>(cursor ? cursor->nextIndex : 0);
                const auto count = member.recordCount.value_or(0);
                if (first >= count) break;
                const auto pageLimit = std::min({std::size_t{64}, options_.maxExaminedPerCategory - coverage.examined, count - first});
                if ((kind == MapDataKind::Sounds || kind == MapDataKind::Regions || kind == MapDataKind::Cameras)
                    && !MapNameBatch(kind, first, pageLimit, coverage)) {
                    RawMapNames(kind, first, pageLimit, coverage);
                    const auto next = first + pageLimit;
                    if (next < count) cursor = MapDataQueryCursor{context_.mapDataBinding, kind, next};
                    else cursor.reset();
                    continue;
                }
                const auto page = QueryMapDataPage(*context_.mapData, context_.mapDataBinding, kind, cursor, pageLimit);
                if (!page.Succeeded()) {
                    coverage.state = ProjectSearchState::Invalid;
                    for (auto diagnostic : page.diagnostics) if (diagnostics_.size() < 16) {
                        diagnostic.severity = DiagnosticSeverity::Warning;
                        bool truncated = false; diagnostic.message = Text(diagnostic.message, truncated);
                        diagnostics_.push_back(std::move(diagnostic));
                    }
                    break;
                }
                for (const auto& record : page.value->rows) {
                    if (!Visit(coverage)) break;
                    ProjectSearchRow row; row.category = coverage.category; row.mapKind = kind;
                    row.sourceIndex = record.sourceIndex; row.rowKey = record.rowKey;
                    row.stableId = "map/" + std::to_string(static_cast<unsigned>(kind)) + "/"
                        + std::to_string(record.sourceIndex) + "/" + record.rowKey;
                    row.title = (kind == MapDataKind::Units || kind == MapDataKind::Doodads) ? PlacementName(record, kind, coverage)
                        : record.name.displayBytes.value_or(record.name.rawBytes);
                    row.detail = std::string(ProjectSearchCategoryName(row.category)) + " · " + std::to_string(record.sourceIndex);
                    if (!record.rawcodeBytes.empty()) row.detail += " · " + record.rawcodeBytes;
                    if (record.resourcePathBytes) row.detail += " · " + Text(*record.resourcePathBytes, row.textTruncated);
                    if (!record.name.displayBytes && !record.name.rawBytes.empty()) coverage.state = ProjectSearchState::Partial;
                    Add(std::move(row));
                }
                cursor = page.value->next;
            } while (cursor);
        }
        return Status::Ok();
    }
    const ProjectSearchContext& context_;
    std::string_view query_;
    const ProjectSearchOptions& options_;
    ProjectSearchResult result_;
    std::vector<Diagnostic> diagnostics_;
    std::unordered_map<std::string, const ObjectData::ObjectRecord*> objectIndex_;
};
} // namespace

std::string_view ProjectSearchCategoryName(ProjectSearchCategory category) noexcept {
    constexpr std::array<std::string_view, 11> names{"触发器", "变量", "地图物体", "原版物体", "声音", "输入文件", "单位实例", "地物实例", "区域", "镜头", "代码声明"};
    const auto index = Index(category); return index < names.size() ? names[index] : std::string_view{};
}
Result<ProjectSearchResult> SearchProject(const ProjectSearchContext& context,
    std::string_view expectedContextRevision, std::string_view queryUtf8, const ProjectSearchOptions& options) {
    if (context.revision.empty() || context.revision.size() > 512 || context.revision != expectedContextRevision
        || context.revision.find('\0') != std::string::npos || !ValidUtf8(context.revision))
        return Result<ProjectSearchResult>::Failure("Project.Search", "PROJECT_SEARCH_STALE", "工程或当前编辑修订已变化；不能激活旧搜索结果。");
    if (queryUtf8.size() > 256 || queryUtf8.find('\0') != std::string_view::npos || !ValidUtf8(queryUtf8))
        return Result<ProjectSearchResult>::Failure("Project.Search", "PROJECT_SEARCH_QUERY_INVALID", "查询必须为最多 256 字节的 UTF8。");
    if (!options.limit || options.limit > 128 || !options.maxExaminedPerCategory || options.maxExaminedPerCategory > 32768
        || !options.maxResultBytes || options.maxResultBytes > 256 * 1024 || context.objects.size() > 7
        || !options.maxTriggerNodes || options.maxTriggerNodes > 65536
        || !options.maxTriggerTextBytes || options.maxTriggerTextBytes > 8 * 1024 * 1024)
        return Result<ProjectSearchResult>::Failure("Project.Search", "PROJECT_SEARCH_BUDGET", "结果数、扫描数、字节预算或物编来源数超出有界查询限制。");
    while (!queryUtf8.empty() && (queryUtf8.front() == ' ' || queryUtf8.front() == '\t')) queryUtf8.remove_prefix(1);
    while (!queryUtf8.empty() && (queryUtf8.back() == ' ' || queryUtf8.back() == '\t')) queryUtf8.remove_suffix(1);
    try { return Search(context, queryUtf8, options).Run(); }
    catch (const std::bad_alloc&) { return Result<ProjectSearchResult>::Failure("Project.Search", "PROJECT_SEARCH_ALLOCATION", "工程搜索内存分配失败。"); }
}

Result<ProjectSearchPage> SearchProjectPage(const ProjectSearchContext& context,
    std::string_view expectedContextRevision, std::string_view queryUtf8, const ProjectSearchOptions& options,
    std::size_t pageLimit, const ProjectSearchCursor* cursor) {
    const auto fail = [](const char* code, const char* message) {
        return Result<ProjectSearchPage>::Failure("Project.Search", code, message);
    };
    if (!pageLimit || pageLimit > 128)
        return fail("PROJECT_SEARCH_PAGE_BUDGET", "页长必须为1..128；未查询或修改文档。");
    if (cursor && (cursor->contextRevision != expectedContextRevision || cursor->queryUtf8 != queryUtf8
        || cursor->windowSha256.size() != 64 || !cursor->nextIndex || cursor->nextIndex % pageLimit))
        return fail("PROJECT_SEARCH_CURSOR_STALE", "游标的查询、修订或分页范围不符；请重新搜索。");
    auto searched = SearchProject(context, expectedContextRevision, queryUtf8, options);
    if (!searched.Succeeded()) { Result<ProjectSearchPage> failure; failure.diagnostics = std::move(searched.diagnostics); return failure; }
    try {
        auto& window = *searched.value;
        std::string identity;
        const auto text = [&](std::string_view value) {
            identity += std::to_string(value.size()); identity += ':'; identity.append(value); identity += ';';
        };
        const auto number = [&](auto value) { text(std::to_string(value)); };
        text("project-search-page-v1"); text(window.contextRevision); text(queryUtf8);
        number(options.limit); number(options.maxExaminedPerCategory); number(options.maxResultBytes);
        number(options.maxTriggerNodes); number(options.maxTriggerTextBytes); number(pageLimit);
        number(window.complete); number(window.hasMore); number(window.rows.size());
        number(window.nameNodesCharged); number(window.nameTextBytesCharged);
        for (const auto& coverage : window.coverage) {
            number(Index(coverage.category)); number(static_cast<unsigned>(coverage.state));
            number(coverage.examined); number(coverage.matches); number(coverage.totalRows.has_value());
            if (coverage.totalRows) number(*coverage.totalRows);
            number(coverage.scanTruncated); number(coverage.resultsTruncated);
            number(coverage.graphNodesExamined); number(coverage.textBytesExamined);
        }
        for (const auto& row : window.rows) {
            number(Index(row.category)); text(row.stableId); text(row.title); text(row.detail); text(row.contextRevision);
            number(row.score); text(row.sourceId ? std::to_string(*row.sourceId) : "none");
            text(row.ecaId ? std::to_string(*row.ecaId) : "none"); text(row.argumentId ? std::to_string(*row.argumentId) : "none");
            number(row.rawValue.has_value()); if (row.rawValue) text(*row.rawValue);
            text(row.object ? ObjectIdentity(*row.object) : "none");
            text(row.mapKind ? std::to_string(static_cast<unsigned>(*row.mapKind)) : "none");
            text(row.sourceIndex ? std::to_string(*row.sourceIndex) : "none"); text(row.rowKey);
            number(row.textTruncated); number(row.codeSource.has_value()); if (row.codeSource) text(*row.codeSource);
            number(row.codeLine); number(row.codeColumn); text(row.codeBinding);
        }
        const auto digest = Sha256Text(identity);
        if (!digest.Succeeded()) { Result<ProjectSearchPage> failure; failure.diagnostics = digest.diagnostics; return failure; }
        const auto first = cursor ? cursor->nextIndex : 0;
        if (cursor && (cursor->windowSha256 != *digest.value || first >= window.rows.size()))
            return fail("PROJECT_SEARCH_CURSOR_STALE", "搜索窗口、覆盖情况或预算已变化；旧游标不能继续。");
        ProjectSearchPage page; page.contextRevision = window.contextRevision; page.queryUtf8 = queryUtf8;
        page.windowSha256 = *digest.value; page.coverage = window.coverage;
        page.totalWindowRows = window.rows.size(); page.complete = window.complete; page.windowHasMore = window.hasMore;
        page.nameNodesCharged = window.nameNodesCharged; page.nameTextBytesCharged = window.nameTextBytesCharged;
        const auto end = first + std::min(pageLimit, window.rows.size() - first);
        for (auto index = first; index < end; ++index) page.rows.push_back(std::move(window.rows[index]));
        if (end < window.rows.size()) page.next = ProjectSearchCursor{page.contextRevision, page.queryUtf8, page.windowSha256, end};
        auto result = Result<ProjectSearchPage>::Success(std::move(page)); result.diagnostics = std::move(searched.diagnostics);
        return result;
    } catch (const std::bad_alloc&) {
        return fail("PROJECT_SEARCH_ALLOCATION", "工程搜索分页内存分配失败。");
    }
}
} // namespace War3Map::Services
