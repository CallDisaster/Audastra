#include "War3MapCore/ObjectData/SlkOptimizer.h"

#include "SlkTableText.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace War3Map::ObjectData {
namespace {

using namespace SlkText;
using War3Map::Text::LowerAscii;

constexpr std::array<std::pair<ObjectType, const char*>, 7> kObjectFiles{{
    {ObjectType::Unit, "war3map.w3u"}, {ObjectType::Item, "war3map.w3t"}, {ObjectType::Ability, "war3map.w3a"},
    {ObjectType::Buff, "war3map.w3h"}, {ObjectType::Upgrade, "war3map.w3q"}, {ObjectType::Doodad, "war3map.w3d"},
    {ObjectType::Destructable, "war3map.w3b"}}};

// 自定义物体的 profile 节追加到该类型会加载的一个文件里；新 ID 不与任何已有节同名。
std::string ProfileOutput(ObjectType type) {
    switch (type) {
    case ObjectType::Unit: return "Units\\CampaignUnitStrings.txt";
    case ObjectType::Item: return "Units\\ItemStrings.txt";
    case ObjectType::Ability: return "Units\\CampaignAbilityStrings.txt";
    case ObjectType::Buff: return "Units\\CommonAbilityStrings.txt";
    case ObjectType::Upgrade: return "Units\\CampaignUpgradeStrings.txt";
    default: return {};
    }
}

std::string MemberName(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}
std::string Lower(std::string_view path) { return LowerAscii(MemberName(std::string(path))); }
std::string TextOf(const std::vector<std::byte>& bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
std::vector<std::byte> BytesOf(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

template<class T> Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure("Object.SlkOptimizer", std::move(code), std::move(message));
}

std::string FloatToken(float value) {
    char buffer[64];
    const auto shortest = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string token(buffer, shortest.ptr);
    // 游戏按 double 读入再收窄到 float；逐步加精度直到这条路径也还原同一 float。
    for (int precision : {0, 9, 12, 17}) {
        if (precision) {
            const auto fixed = std::to_chars(buffer, buffer + sizeof(buffer), static_cast<double>(value), std::chars_format::general, precision);
            token.assign(buffer, fixed.ptr);
        }
        if (std::bit_cast<std::uint32_t>(static_cast<float>(std::strtod(token.c_str(), nullptr))) == std::bit_cast<std::uint32_t>(value)) return token;
    }
    return {};
}

std::string KeyText(const ModificationKey& key) {
    return key.fieldId + "#" + std::to_string(key.level.value_or(0)) + "#" + std::to_string(key.dataIndex.value_or(0))
        + "#" + std::to_string(key.variation.value_or(0));
}

// 比较用的规范值：整数按数值、实数按 float 位、字符串按字节；无法解析时保留原文。
struct Normal {
    bool present = false;
    std::string text;
    bool operator==(const Normal&) const = default;
};

Normal Normalize(std::string_view text, std::optional<NativeValueType> type) {
    Normal result{true, {}};
    if (type == NativeValueType::Int32) {
        long long value = 0;
        const auto trimmed = War3Map::Text::TrimCWhitespace(text);
        const auto parsed = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
        result.text = parsed.ec == std::errc{} && parsed.ptr == trimmed.data() + trimmed.size() ? "i" + std::to_string(value) : "r:" + std::string(text);
    } else if (type == NativeValueType::Real || type == NativeValueType::Unreal) {
        const std::string owned(War3Map::Text::TrimCWhitespace(text));
        char* end = nullptr;
        const double value = std::strtod(owned.c_str(), &end);
        result.text = !owned.empty() && end == owned.c_str() + owned.size()
            ? "f" + std::to_string(std::bit_cast<std::uint32_t>(static_cast<float>(value))) : "r:" + owned;
    } else result.text = "s:" + std::string(text);
    return result;
}

struct LoadedText {
    std::string text;
    bool fromMap = false;
};

// 一次读取视图：地图同名成员优先于游戏文件，与游戏加载顺序一致。
class SourceView {
public:
    SourceView(const SlkOptimizerInput& input, const std::map<std::string, std::vector<std::byte>>* overlay)
        : input_(input), overlay_(overlay) {}

    std::optional<std::vector<std::byte>> Member(const std::string& name) const {
        if (overlay_) if (const auto found = overlay_->find(Lower(name)); found != overlay_->end()) return found->second;
        return input_.mapMember(MemberName(name));
    }
    std::optional<LoadedText> Read(const std::string& path) const {
        if (auto member = Member(path)) return LoadedText{TextOf(*member), true};
        auto game = input_.gameFile(path);
        if (!game.Succeeded()) return std::nullopt;
        return LoadedText{TextOf(*game.value), false};
    }

private:
    const SlkOptimizerInput& input_;
    const std::map<std::string, std::vector<std::byte>>* overlay_;
};

struct FamilyData {
    ObjectType type = ObjectType::Unit;
    CatalogSourceSpec spec;
    std::vector<std::pair<std::string, RawSlkTable>> tables; // 与 spec.slkFiles 同序
};

// 物体在各表中的行与合并后的 profile。
struct ObjectSource {
    std::vector<const std::map<int, std::string>*> rows; // 与 tables 同序；无行为空
    std::map<std::string, ProfileEntry> profile;         // 小写键
};

class Profiles {
public:
    explicit Profiles(const SourceView& view) : view_(view) {}
    const RawProfileFile* File(const std::string& path) {
        const auto key = Lower(path);
        if (auto found = files_.find(key); found != files_.end()) return found->second ? &*found->second : nullptr;
        auto loaded = view_.Read(path);
        auto& slot = files_[key];
        if (loaded) slot = ParseRawProfile(std::move(loaded->text));
        return slot ? &*slot : nullptr;
    }
    std::map<std::string, ProfileEntry> Merged(const std::vector<std::string>& paths, std::string_view id) {
        std::map<std::string, ProfileEntry> merged;
        const auto lowerId = LowerAscii(id);
        for (const auto& path : paths) {
            const auto* file = File(path);
            if (!file) continue;
            const auto found = file->sectionsByLowerId.find(lowerId);
            if (found == file->sectionsByLowerId.end()) continue;
            for (const auto index : found->second)
                for (const auto& entry : file->sections[index].entries) merged[LowerAscii(entry.key)] = entry;
        }
        return merged;
    }
    bool HasSection(std::string_view lowerId) const {
        for (const auto& [path, file] : files_)
            if (file && file->sectionsByLowerId.contains(std::string(lowerId))) return true;
        return false;
    }
    const std::map<std::string, std::optional<RawProfileFile>>& Files() const { return files_; }

private:
    const SourceView& view_;
    std::map<std::string, std::optional<RawProfileFile>> files_;
};

Result<FamilyData> LoadFamily(const SourceView& view, const CatalogSourceSpec& spec, Profiles& profiles) {
    FamilyData family{spec.type, spec, {}};
    for (const auto& path : spec.slkFiles) {
        auto loaded = view.Read(path);
        if (!loaded) return Fail<FamilyData>("SLK_TABLE_MISSING", "无法读取物编表 " + path + "。");
        auto parsed = ParseRawSlk(std::move(loaded->text));
        if (!parsed.Succeeded()) return Fail<FamilyData>("SLK_TABLE_INVALID", "物编表 " + path + " 无法解析：" + parsed.diagnostics.front().message);
        family.tables.emplace_back(path, std::move(*parsed.value));
    }
    for (const auto& path : spec.profileFiles) (void)profiles.File(path);
    return Result<FamilyData>::Success(std::move(family));
}

ObjectSource SourceOf(const FamilyData& family, Profiles& profiles, std::string_view id) {
    ObjectSource source;
    for (const auto& [path, table] : family.tables) {
        const auto row = table.rows.find(std::string(id));
        source.rows.push_back(row == table.rows.end() ? nullptr : &table.cells.at(row->second));
    }
    source.profile = profiles.Merged(family.spec.profileFiles, id);
    return source;
}

std::optional<std::size_t> TableFor(const FamilyData& family, const ObjectFieldDefinition& definition, std::string_view column) {
    std::optional<std::size_t> any;
    for (std::size_t index = 0; index < family.tables.size(); ++index) {
        const auto& [path, table] = family.tables[index];
        if (!table.columns.contains(std::string(column))) continue;
        const auto stem = LowerAscii(std::filesystem::path(path).stem().string());
        if (stem == LowerAscii(definition.sourceTable)) return index;
        if (!any) any = index;
    }
    return any;
}

bool IsProfileField(const ObjectFieldDefinition& definition) { return LowerAscii(definition.sourceTable) == "profile"; }

std::string ProfileKey(const ObjectFieldDefinition& definition, const std::string& sourceKey) {
    const auto lower = LowerAscii(definition.sourceField);
    return sourceKey.starts_with(lower) ? definition.sourceField + sourceKey.substr(lower.size()) : sourceKey;
}

ObjectFieldContext ContextOf(ObjectType type, const std::string& rawcode, const FamilyData& family, std::string_view parent) {
    ObjectFieldContext context;
    context.rawcode = rawcode;
    context.baseCode = std::string(parent);
    if (type == ObjectType::Ability || type == ObjectType::Buff) {
        const auto& table = family.tables.front().second;
        const auto row = table.rows.find(std::string(parent));
        const auto column = table.columns.find("code");
        if (row != table.rows.end() && column != table.columns.end())
            if (const auto cell = table.cells.at(row->second).find(column->second); cell != table.cells.at(row->second).end())
                context.baseCode = DecodeToken(cell->second);
    }
    if (type == ObjectType::Unit) {
        context.hero = !rawcode.empty() && rawcode.front() >= 'A' && rawcode.front() <= 'Z';
        const auto& table = family.tables.front().second;
        const auto row = table.rows.find(std::string(parent));
        const auto column = table.columns.find("isbldg");
        if (row != table.rows.end() && column != table.columns.end()) {
            const auto cell = table.cells.at(row->second).find(column->second);
            context.building = cell != table.cells.at(row->second).end() && DecodeToken(cell->second) == "1";
        }
    }
    if (type == ObjectType::Item) context.item = true;
    return context;
}

// 字符串修改值：完整 TRIGSTR 引用按地图字符串表展开；其他嵌入引用无法确定，返回空。
std::optional<std::string> ResolvedText(const ObjectModification& modification, const MapData::StringTableDocument* strings) {
    const auto* text = std::get_if<std::string>(&modification.value);
    if (!text) return std::nullopt;
    if (text->find("TRIGSTR_") == std::string::npos) return *text;
    if (!strings) return std::nullopt;
    auto resolved = strings->Resolve(*text);
    if (!resolved.Succeeded() || resolved.value->find("TRIGSTR_") != std::string::npos) return std::nullopt;
    return *resolved.value;
}

Normal ModificationValue(const ObjectModification& modification, std::optional<NativeValueType> type,
    const MapData::StringTableDocument* strings) {
    if (const auto* value = std::get_if<std::int32_t>(&modification.value)) return Normalize(std::to_string(*value), NativeValueType::Int32);
    if (const auto* value = std::get_if<float>(&modification.value))
        return {true, "f" + std::to_string(std::bit_cast<std::uint32_t>(*value))};
    if (const auto* value = std::get_if<std::string>(&modification.value)) {
        const auto resolved = ResolvedText(modification, strings);
        return Normalize(resolved ? *resolved : *value, type == NativeValueType::String ? type : NativeValueType::String);
    }
    return {true, "opaque:" + ValueToDisplayString(modification.value)};
}

Normal SourceValue(const FamilyData& family, const ObjectSource& source, const ObjectFieldDefinition& definition,
    const ObjectFieldMatch& match) {
    std::optional<std::string> raw;
    if (IsProfileField(definition)) {
        const auto entry = source.profile.find(match.sourceKey);
        if (entry == source.profile.end()) return {};
        auto items = SplitProfileList(entry->second.value);
        if (match.sourceIndex) {
            if (*match.sourceIndex >= items.size()) return {};
            raw = DecodeProfileItem(items[*match.sourceIndex]);
        } else {
            for (auto& item : items) item = DecodeProfileItem(item);
            raw = JoinProfileList(items);
        }
    } else {
        const auto table = TableFor(family, definition, match.sourceKey);
        if (!table || !source.rows[*table]) return {};
        const auto column = family.tables[*table].second.columns.at(match.sourceKey);
        const auto cell = source.rows[*table]->find(column);
        if (cell == source.rows[*table]->end()) return {};
        raw = DecodeToken(cell->second);
    }
    return Normalize(*raw, definition.nativeValueType);
}

std::map<std::string, const ObjectModification*> ModificationIndex(const ObjectRecord* record) {
    std::map<std::string, const ObjectModification*> index;
    if (record) for (const auto& modification : record->modifications) index[KeyText(modification.key)] = &modification;
    return index;
}

ModificationKey KeyFor(ObjectType type, const ObjectFieldDefinition& definition, std::uint32_t level) {
    ModificationKey key{definition.fieldId, {}, {}, {}};
    if (type == ObjectType::Ability || type == ObjectType::Upgrade || type == ObjectType::Doodad) {
        key.dataIndex = static_cast<std::uint32_t>(std::max(0, definition.data));
        if (type == ObjectType::Doodad) key.variation = level; else key.level = level;
    }
    return key;
}

struct Promotion {
    ObjectType type = ObjectType::Unit;
    std::string id;
    std::string parent;
    ObjectFieldContext context;
    std::vector<std::map<int, std::string>> rows;   // 与 family.tables 同序；空表示该表无行
    std::vector<bool> hasRow;
    ProfileSection section;
    std::vector<ObjectModification> residual;
    std::vector<NativeValueType> residualTypes;
    std::vector<ObjectModification> original;       // 原自定义物体的全部修改，用于回读比较
};

struct Planner {
    const SlkOptimizerInput& input;
    const MapData::StringTableDocument* strings;

    // 返回空表示该修改写入了表/节；否则说明为什么留在物编文件里。
    std::optional<std::string> Apply(const FamilyData& family, Promotion& promotion, const NativeObjectDocument& document,
        const ObjectModification& modification, std::map<std::string, std::size_t>& profileIndex) const {
        const auto* definition = input.schema->FindNative(promotion.type, modification.key.fieldId);
        if (!definition) return "未定义字段";
        const auto match = input.schema->MatchNativeField(promotion.type, modification.key, promotion.context);
        if (!match.safeToEdit) return "字段不能可靠写入表格";
        const auto stored = document.ModificationValueType({promotion.type, promotion.id}, modification.key);
        if (!stored || !definition->nativeValueType || *stored != *definition->nativeValueType || modification.valueWasOpaque)
            return "存储类型与元数据不一致";
        std::string text;
        bool isString = false;
        if (const auto* integer = std::get_if<std::int32_t>(&modification.value)) text = std::to_string(*integer);
        else if (const auto* real = std::get_if<float>(&modification.value)) {
            text = FloatToken(*real);
            if (text.empty()) return "实数无法精确写成文本";
        } else if (std::holds_alternative<std::string>(modification.value)) {
            const auto resolved = ResolvedText(modification, strings);
            if (!resolved) return "字符串引用无法展开";
            text = *resolved;
            isString = true;
        } else return "值类型不支持";

        if (IsProfileField(*definition)) {
            if (promotion.section.id.empty()) return "该类型没有 profile";
            auto found = profileIndex.find(match.sourceKey);
            if (match.sourceIndex) {
                if (found == profileIndex.end()) {
                    // 父物体没有该键时只能确定第一项：新建单项列表；更后面的项无法确定前面各项应为何值。
                    if (*match.sourceIndex != 0) return "profile 列表项不存在";
                    const auto token = ProfileItemToken(text);
                    if (!token) return "文本含引号、换行或首尾空白";
                    profileIndex[match.sourceKey] = promotion.section.entries.size();
                    promotion.section.entries.push_back({ProfileKey(*definition, match.sourceKey), *token});
                    return std::nullopt;
                }
                auto& entry = promotion.section.entries[found->second];
                auto items = SplitProfileList(entry.value);
                // 紧接末尾的一项可以追加（如先写按钮 X 再写 Y）；更靠后会留下无法确定的空项。
                if (*match.sourceIndex > items.size()) return "profile 列表项超出现有长度";
                const auto token = ProfileItemToken(text);
                if (!token) return "文本含引号、换行或首尾空白";
                if (*match.sourceIndex == items.size()) items.push_back(*token);
                else items[*match.sourceIndex] = *token;
                entry.value = JoinProfileList(items);
                return std::nullopt;
            }
            std::string token;
            if (isString && definition->isList) {
                auto items = SplitProfileList(text);
                for (auto& item : items) {
                    const auto itemToken = ProfileItemToken(item);
                    if (!itemToken || item.find(',') != std::string::npos) return "列表项无法写出";
                    item = *itemToken;
                }
                token = JoinProfileList(items);
            } else {
                const auto itemToken = ProfileItemToken(text);
                if (!itemToken) return "文本含引号、换行或首尾空白";
                token = *itemToken;
            }
            if (found == profileIndex.end()) {
                profileIndex[match.sourceKey] = promotion.section.entries.size();
                promotion.section.entries.push_back({ProfileKey(*definition, match.sourceKey), token});
            } else promotion.section.entries[found->second].value = token;
            return std::nullopt;
        }
        const auto table = TableFor(family, *definition, match.sourceKey);
        if (!table || !promotion.hasRow[*table]) return "表中没有对应列或行";
        std::string token;
        if (isString) {
            const auto quoted = StringToken(text);
            if (!quoted) return "文本含引号、分号或换行";
            token = *quoted;
        } else token = text;
        promotion.rows[*table][family.tables[*table].second.columns.at(match.sourceKey)] = token;
        return std::nullopt;
    }
};

bool ValidId(std::string_view id) {
    return id.size() == 4 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return c > 32 && c < 127 && c != '"' && c != ';' && c != ',' && c != '[' && c != ']';
    });
}

Status VerifyRows(const FamilyData& before, const FamilyData& after) {
    for (std::size_t index = 0; index < before.tables.size(); ++index) {
        const auto& old = before.tables[index].second;
        const auto& now = after.tables[index].second;
        for (const auto& [row, cells] : old.cells) {
            const auto found = now.cells.find(row);
            if (found == now.cells.end() || found->second != cells)
                return Status::Failure("Object.SlkOptimizer", "SLK_VERIFY_ORIGINAL_ROW",
                    "SLK 优化改变了 " + before.tables[index].first + " 的已有行 " + std::to_string(row) + "。");
        }
    }
    return Status::Ok();
}

} // namespace

std::vector<std::string> SlkOptimizerMemberNames() {
    std::vector<std::string> names;
    const auto add = [&names](std::string name) {
        name = MemberName(std::move(name));
        if (std::none_of(names.begin(), names.end(), [&](const auto& item) { return LowerAscii(item) == LowerAscii(name); })) names.push_back(name);
    };
    for (const auto& [type, file] : kObjectFiles) add(file);
    add("war3map.wts");
    for (const auto* family : {"unit", "item", "ability", "buff", "upgrade", "doodad", "destructable", "misc", "txt"})
        add(std::string("table\\") + family + ".ini");
    for (const auto& spec : StandardObjectCatalog::DefaultSources()) {
        for (const auto& path : spec.slkFiles) add(path);
        for (const auto& path : spec.profileFiles) add(path);
    }
    return names;
}

Result<SlkOptimizerResult> OptimizeObjectDataToSlk(const SlkOptimizerInput& input) {
    if (!input.mapMember || !input.gameFile || !input.schema)
        return Fail<SlkOptimizerResult>("SLK_INPUT", "SLK 优化缺少地图、游戏数据或字段定义。");
    const SourceView before(input, nullptr);
    for (const auto* family : {"unit", "item", "ability", "buff", "upgrade", "doodad", "destructable", "misc", "txt"}) {
        if (before.Member(std::string("table\\") + family + ".ini"))
            return Fail<SlkOptimizerResult>("SLK_LNI_UNSUPPORTED", "地图含 LNI 物编（table 目录），SLK 优化只处理已保存的物编文件，不能忽略它的内容。");
    }
    std::optional<MapData::StringTableDocument> strings;
    if (const auto wts = before.Member("war3map.wts")) {
        auto parsed = MapData::ParseStringTable(*wts);
        if (parsed.Succeeded()) strings = std::move(*parsed.value);
    }
    const auto* stringTable = strings ? &*strings : nullptr;
    const Planner planner{input, stringTable};

    SlkOptimizerResult result;
    Profiles beforeProfiles(before);
    std::map<ObjectType, FamilyData> families;
    std::map<ObjectType, std::unique_ptr<NativeObjectDocument>> documents;
    std::map<ObjectType, std::vector<Promotion>> promotions;
    std::set<std::string> promotedLower;
    std::map<std::string, std::vector<ProfileSection>> appendedSections; // 小写路径 → 节
    std::map<std::string, std::string> profilePaths;                     // 小写路径 → 原路径
    std::map<std::string, std::pair<std::size_t, std::string>> residualReasons; // 原因 → 数量与首例

    // 先装载所有类型的 profile，保证新 ID 与任何已有节都不重名。
    std::vector<CatalogSourceSpec> specs = StandardObjectCatalog::DefaultSources();
    for (const auto& spec : specs) for (const auto& path : spec.profileFiles) (void)beforeProfiles.File(path);

    for (const auto& [type, objectFile] : kObjectFiles) {
        const auto bytes = before.Member(objectFile);
        if (!bytes) continue;
        auto parsed = NativeObjectCodec(type).Parse(*bytes);
        auto* parsedDocument = dynamic_cast<NativeObjectDocument*>(parsed.document.get());
        if (!parsed.Succeeded() || !parsedDocument || !parsedDocument->IsStructurallySafe())
            return Fail<SlkOptimizerResult>("SLK_OBJECT_INVALID", std::string(objectFile) + " 无法完整解析，不能做 SLK 优化。");
        std::unique_ptr<NativeObjectDocument> document(static_cast<NativeObjectDocument*>(parsed.document.release()));
        std::vector<const ObjectRecord*> customs;
        for (const auto& record : document->Objects()) if (record.parentRawcode) customs.push_back(&record);
        if (customs.empty()) continue;
        if (!document->Supports(NativeObjectOperation::EditStructure)) {
            result.keptCustomObjects += customs.size();
            result.notes.push_back(std::string(objectFile) + " 的格式版本不支持结构修改，" + std::to_string(customs.size()) + " 个自定义物体保留在物编文件里。");
            continue;
        }
        const auto spec = std::find_if(specs.begin(), specs.end(), [type](const auto& item) { return item.type == type; });
        auto family = LoadFamily(before, *spec, beforeProfiles);
        if (!family.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = family.diagnostics; return failed; }
        const auto& tables = family.value->tables;
        // 首选文件不存在时退回该类型会加载的第一个 profile 文件。
        auto profileOutput = ProfileOutput(type);
        if (!profileOutput.empty() && !beforeProfiles.File(profileOutput)) {
            profileOutput.clear();
            for (const auto& path : spec->profileFiles) if (beforeProfiles.File(path)) { profileOutput = MemberName(path); break; }
        }
        for (const auto* record : customs) {
            const auto& id = record->key.rawcode;
            const auto& parent = *record->parentRawcode;
            std::string reason;
            if (!ValidId(id)) reason = "ID 不能写入表格";
            else if (!tables.front().second.rows.contains(parent)) reason = "父物体不在原版表中";
            else if (std::any_of(tables.begin(), tables.end(), [&](const auto& table) { return table.second.rows.contains(id); }))
                reason = "表中已有同 ID 的行";
            else if (beforeProfiles.HasSection(LowerAscii(id)) || promotedLower.contains(LowerAscii(id)))
                reason = "profile 中已有同名节";
            else if (std::any_of(record->modifications.begin(), record->modifications.end(),
                         [](const auto& modification) { return modification.key.fieldId.size() != 4; }))
                reason = "含非标准字段 ID";
            else {
                // 不适用字段在游戏里仍按字段 ID 写到同一列，可能与转换进表的值互相覆盖；整个物体保持原样。
                const auto context = ContextOf(type, id, *family.value, parent);
                if (std::any_of(record->modifications.begin(), record->modifications.end(), [&](const auto& modification) {
                        const auto* definition = input.schema->FindNative(type, modification.key.fieldId);
                        return definition && !input.schema->AppliesTo(*definition, context);
                    })) reason = "含不适用于该物体的字段修改";
            }
            if (!reason.empty()) {
                ++result.keptCustomObjects;
                result.notes.push_back(id + "：" + reason + "，保留在物编文件里。");
                continue;
            }
            Promotion promotion;
            promotion.type = type; promotion.id = id; promotion.parent = parent;
            promotion.context = ContextOf(type, id, *family.value, parent);
            promotion.original = record->modifications;
            const auto source = SourceOf(*family.value, beforeProfiles, parent);
            for (std::size_t index = 0; index < tables.size(); ++index) {
                promotion.hasRow.push_back(source.rows[index] != nullptr);
                auto row = source.rows[index] ? *source.rows[index] : std::map<int, std::string>{};
                if (source.rows[index]) row[1] = *StringToken(id);
                promotion.rows.push_back(std::move(row));
            }
            std::map<std::string, std::size_t> profileIndex;
            if (!profileOutput.empty()) {
                promotion.section.id = id;
                for (const auto& [key, entry] : source.profile) {
                    profileIndex[key] = promotion.section.entries.size();
                    promotion.section.entries.push_back(entry);
                }
            }
            for (const auto& modification : record->modifications) {
                if (const auto reasonText = planner.Apply(*family.value, promotion, *document, modification, profileIndex)) {
                    const auto stored = document->ModificationValueType({type, id}, modification.key);
                    if (!stored) return Fail<SlkOptimizerResult>("SLK_OBJECT_TYPE", id + " 的修改缺少存储类型，不能保留到物编文件。");
                    promotion.residual.push_back(modification);
                    promotion.residualTypes.push_back(*stored);
                    auto& tally = residualReasons[*reasonText];
                    if (tally.first++ == 0) tally.second = id + "." + modification.key.fieldId;
                }
            }
            promotedLower.insert(LowerAscii(id));
            promotions[type].push_back(std::move(promotion));
        }
        if (promotions[type].empty()) continue;
        // 物编文件：删除已转换的自定义物体；写不进表的修改改挂在同 ID 的原版覆盖下。
        for (const auto& promotion : promotions[type]) {
            auto removed = document->DeleteCustomObject({type, promotion.id});
            if (!removed.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = removed.diagnostics; return failed; }
            if (promotion.residual.empty()) continue;
            auto added = document->AddOriginalObject(promotion.id);
            if (!added.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = added.diagnostics; return failed; }
            for (std::size_t index = 0; index < promotion.residual.size(); ++index) {
                auto status = document->AddModification({type, promotion.id}, promotion.residual[index].key,
                    promotion.residualTypes[index], promotion.residual[index].value);
                if (!status.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = status.diagnostics; return failed; }
            }
            result.residualFields += promotion.residual.size();
        }
        result.convertedObjects += promotions[type].size();
        auto encoded = NativeObjectCodec(type).Serialize(*document);
        if (!encoded.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = encoded.diagnostics; return failed; }
        result.replacements.push_back({objectFile, std::move(*encoded.value)});
        for (std::size_t index = 0; index < tables.size(); ++index) {
            std::vector<std::map<int, std::string>> rows;
            for (const auto& promotion : promotions[type]) if (promotion.hasRow[index]) rows.push_back(promotion.rows[index]);
            if (rows.empty()) continue;
            auto text = AppendSlkRows(tables[index].second, rows);
            if (!text.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = text.diagnostics; return failed; }
            result.replacements.push_back({MemberName(tables[index].first), BytesOf(*text.value)});
        }
        if (!profileOutput.empty()) {
            profilePaths[Lower(profileOutput)] = profileOutput;
            for (const auto& promotion : promotions[type]) appendedSections[Lower(profileOutput)].push_back(promotion.section);
        }
        families.emplace(type, std::move(*family.value));
        documents.emplace(type, std::move(document));
    }
    for (const auto& [reason, tally] : residualReasons)
        result.notes.push_back("留在物编文件的修改（" + reason + "）：" + std::to_string(tally.first) + " 项，例如 " + tally.second + "。");
    for (const auto& [lowerPath, sections] : appendedSections) {
        const auto* file = beforeProfiles.File(profilePaths.at(lowerPath));
        if (!file) return Fail<SlkOptimizerResult>("SLK_PROFILE_MISSING", "无法读取 " + profilePaths.at(lowerPath) + "，不能写入自定义物体说明。");
        result.replacements.push_back({profilePaths.at(lowerPath), BytesOf(AppendProfileSections(*file, sections))});
    }
    if (result.convertedObjects == 0) return Result<SlkOptimizerResult>::Success(std::move(result));

    // 回读：以写出结果覆盖地图成员，按同一规则重新装载，逐字段比较每个被转换物体。
    std::map<std::string, std::vector<std::byte>> overlay;
    for (const auto& replacement : result.replacements) overlay[Lower(replacement.name)] = replacement.bytes;
    const SourceView after(input, &overlay);
    Profiles afterProfiles(after);
    for (const auto& spec : specs) for (const auto& path : spec.profileFiles) (void)afterProfiles.File(path);
    for (const auto& [path, file] : beforeProfiles.Files()) {
        const auto* now = afterProfiles.File(path);
        if (!file) continue;
        if (!now || now->sections.size() < file->sections.size()
            || !std::equal(file->sections.begin(), file->sections.end(), now->sections.begin(), [](const auto& a, const auto& b) {
                return a.id == b.id && a.entries.size() == b.entries.size() && std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(),
                    [](const auto& x, const auto& y) { return x.key == y.key && x.value == y.value; });
            }))
            return Fail<SlkOptimizerResult>("SLK_VERIFY_ORIGINAL_PROFILE", "SLK 优化改变了 " + path + " 的已有内容。");
    }
    for (const auto& [type, family] : families) {
        auto reloaded = LoadFamily(after, family.spec, afterProfiles);
        if (!reloaded.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = reloaded.diagnostics; return failed; }
        if (auto rows = VerifyRows(family, *reloaded.value); !rows.Succeeded()) { Result<SlkOptimizerResult> failed; failed.diagnostics = rows.diagnostics; return failed; }
        const auto objectFile = std::find_if(kObjectFiles.begin(), kObjectFiles.end(), [type = type](const auto& item) { return item.first == type; })->second;
        const auto bytes = after.Member(objectFile);
        auto parsed = NativeObjectCodec(type).Parse(*bytes);
        const auto* afterDocument = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
        if (!parsed.Succeeded() || !afterDocument) return Fail<SlkOptimizerResult>("SLK_VERIFY_OBJECT", std::string(objectFile) + " 回读失败。");
        // 未转换的物体：父物体与全部修改逐项不变。
        std::set<std::string> promotedIds;
        for (const auto& promotion : promotions.at(type)) promotedIds.insert(promotion.id);
        const auto originalBytes = before.Member(objectFile);
        auto originalParsed = NativeObjectCodec(type).Parse(*originalBytes);
        const auto* originalDocument = dynamic_cast<const NativeObjectDocument*>(originalParsed.document.get());
        for (const auto& record : originalDocument->Objects()) {
            if (record.parentRawcode && promotedIds.contains(record.key.rawcode)) continue;
            const ObjectRecord* now = nullptr;
            for (const auto& candidate : afterDocument->Objects())
                if (candidate.key == record.key && candidate.parentRawcode == record.parentRawcode) now = &candidate;
            if (!now || now->modifications.size() != record.modifications.size()
                || !std::equal(record.modifications.begin(), record.modifications.end(), now->modifications.begin(),
                    [](const auto& a, const auto& b) { return a.key == b.key && a.value == b.value; }))
                return Fail<SlkOptimizerResult>("SLK_VERIFY_UNCHANGED_OBJECT", record.key.rawcode + " 的物编修改在优化后不一致。");
        }
        for (const auto& promotion : promotions.at(type)) {
            const ObjectRecord* residual = nullptr;
            for (const auto& candidate : afterDocument->Objects())
                if (candidate.key.rawcode == promotion.id) {
                    if (candidate.parentRawcode || residual) return Fail<SlkOptimizerResult>("SLK_VERIFY_OBJECT", promotion.id + " 在物编文件里仍是自定义物体或重复出现。");
                    residual = &candidate;
                }
            const ObjectRecord original{0, {type, promotion.id}, promotion.parent, promotion.original, 0};
            const auto beforeMods = ModificationIndex(&original);
            const auto afterMods = ModificationIndex(residual);
            const auto beforeSource = SourceOf(family, beforeProfiles, promotion.parent);
            const auto afterSource = SourceOf(*reloaded.value, afterProfiles, promotion.id);
            std::uint32_t levels = 4;
            for (const auto& modification : promotion.original)
                levels = std::max({levels, modification.key.level.value_or(0), modification.key.variation.value_or(0)});
            const auto compare = [&](const ModificationKey& key) -> std::optional<std::string> {
                const auto* definition = input.schema->FindNative(type, key.fieldId);
                const auto match = input.schema->MatchNativeField(type, key, promotion.context);
                const auto text = KeyText(key);
                const auto oldMod = beforeMods.find(text);
                const auto newMod = afterMods.find(text);
                const std::optional<NativeValueType> storedType = definition ? definition->nativeValueType : std::optional<NativeValueType>{};
                const Normal left = oldMod != beforeMods.end() ? ModificationValue(*oldMod->second, storedType, stringTable)
                    : definition && match.definition ? SourceValue(family, beforeSource, *definition, match) : Normal{};
                const Normal right = newMod != afterMods.end() ? ModificationValue(*newMod->second, storedType, stringTable)
                    : definition && match.definition ? SourceValue(*reloaded.value, afterSource, *definition, match) : Normal{};
                ++result.verifiedValues;
                if (left == right) return std::nullopt;
                return promotion.id + " 的字段 " + key.fieldId + "（等级 " + std::to_string(key.level.value_or(key.variation.value_or(0)))
                    + "）：" + (left.present ? left.text : "无") + " → " + (right.present ? right.text : "无");
            };
            for (const auto& definition : input.schema->Definitions()) {
                // 不适用于该物体的字段（同列的其他技能专用字段）不属于它的有效值。
                if (definition.objectType != type || !input.schema->AppliesTo(definition, promotion.context)) continue;
                const bool extended = type == ObjectType::Ability || type == ObjectType::Upgrade || type == ObjectType::Doodad;
                if (definition.repeat > 0 && !extended) continue;
                for (std::uint32_t level = definition.repeat > 0 ? 1 : 0; level <= (definition.repeat > 0 ? levels : 0); ++level)
                    if (auto difference = compare(KeyFor(type, definition, level)))
                        return Fail<SlkOptimizerResult>("SLK_VERIFY_VALUE", "SLK 优化回读不一致，已停止：" + *difference);
            }
            for (const auto& modification : promotion.original)
                if (auto difference = compare(modification.key))
                    return Fail<SlkOptimizerResult>("SLK_VERIFY_VALUE", "SLK 优化回读不一致，已停止：" + *difference);
        }
    }
    return Result<SlkOptimizerResult>::Success(std::move(result));
}

} // namespace War3Map::ObjectData
