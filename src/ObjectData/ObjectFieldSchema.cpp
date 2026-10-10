#include "War3MapCore/ObjectData/ObjectFieldSchema.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <map>
#include <set>

namespace War3Map::ObjectData {
namespace {
using Rows = War3Map::Text::ProfileSections;
using Fields = Rows::mapped_type;

std::string Token(std::string_view text) {
    text = War3Map::Text::TrimCWhitespace(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') { text.remove_prefix(1); text.remove_suffix(1); }
    std::string value;
    for (std::size_t i = 0; i < text.size(); ++i) {
        value += text[i];
        if (text[i] == '"' && i + 1 < text.size() && text[i + 1] == '"') ++i;
    }
    return value;
}
std::vector<std::string> Split(std::string_view text, char separator = ',') {
    std::vector<std::string> values;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') {
            if (quoted && i + 1 < text.size() && text[i + 1] == '"') ++i;
            else quoted = !quoted;
        } else if (text[i] == separator && !quoted) {
            values.emplace_back(text.substr(start, i - start)); start = i + 1;
        }
    }
    values.emplace_back(text.substr(start));
    return values;
}
std::string Get(const Fields& fields, std::string_view key) {
    const auto found = fields.find(War3Map::Text::LowerAscii(key));
    return found == fields.end() ? std::string{} : Token(found->second);
}
std::optional<std::int32_t> Integer(std::string_view text) {
    text = War3Map::Text::TrimCWhitespace(text);
    std::int32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional(value) : std::nullopt;
}
int Int(const Fields& fields, std::string_view key, int fallback = 0) { return Integer(Get(fields, key)).value_or(fallback); }
bool Flag(const Fields& fields, std::string_view key, bool fallback = true) {
    return fields.contains(War3Map::Text::LowerAscii(key)) ? Get(fields, key) == "1" : fallback;
}
std::string_view Text(const std::vector<std::byte>& bytes) {
    if (bytes.empty()) return {};
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return text;
}
Rows Profile(std::string_view text) { return War3Map::Text::ParseProfile(text); }
Result<Rows> Slk(std::string_view text) {
    if (!text.starts_with("ID;")) return Result<Rows>::Failure("Object.Schema", "metadata-header", "metadata SLK 文件头无效。");
    std::map<int, std::map<int, std::string>> cells;
    int x = 1, y = 1;
    bool ended = false;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const auto line = War3Map::Text::TrimCWhitespace(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line == "E") { ended = true; break; }
        if (!line.starts_with("C;")) continue;
        std::optional<std::string> value;
        for (const auto& part : Split(line.substr(2), ';')) {
            if (part.empty()) continue;
            if (part.front() == 'X' || part.front() == 'Y') {
                const auto coordinate = Integer(std::string_view(part).substr(1));
                if (!coordinate || *coordinate < 1 || *coordinate > 100000) {
                    return Result<Rows>::Failure("Object.Schema", "metadata-coordinate", "metadata SLK 坐标无效。");
                }
                (part.front() == 'X' ? x : y) = *coordinate;
            } else if (part.front() == 'K') value = Token(std::string_view(part).substr(1));
        }
        if (value) cells[y][x] = std::move(*value);
    }
    if (!ended || !cells.contains(1) || !cells.at(1).contains(1)) {
        return Result<Rows>::Failure("Object.Schema", "metadata-truncated", "metadata SLK 缺少列标题或结束记录。");
    }
    Rows result;
    for (const auto& [rowNumber, row] : cells) {
        if (rowNumber == 1 || !row.contains(1) || row.at(1).size() != 4) continue;
        auto& fields = result[row.at(1)];
        for (const auto& [column, value] : row) {
            const auto header = cells.at(1).find(column);
            if (header != cells.at(1).end()) fields[War3Map::Text::LowerAscii(header->second)] = value;
        }
    }
    return Result<Rows>::Success(std::move(result));
}
std::vector<std::string> Ids(const std::string& text) {
    std::vector<std::string> result;
    for (const auto& part : Split(text)) {
        const auto id = Token(part);
        if (!id.empty() && id != "_" && id != "-") result.push_back(id);
    }
    return result;
}
// 统一字符串表优先（YDWE 覆盖层 → 游戏 → 英文原版），其次本次读取的 stringFiles。
std::string Resolve(std::string text, const Fields& strings, const Game::WorldEditStrings* table) {
    std::set<std::string> visited;
    while (text.starts_with("WESTRING_") && visited.insert(text).second) {
        if (table != nullptr) {
            if (const auto* found = table->Find(text)) {
                text = Token(*found);
                continue;
            }
        }
        const auto found = strings.find(War3Map::Text::LowerAscii(text));
        if (found == strings.end()) break;
        text = Token(found->second);
    }
    return text;
}
// 原版菜单字符串含助记符；只清洗显示文案，不触碰 token、源键或枚举值。
std::string DisplayLabel(std::string text) {
    // WE 用 0x1E/0x1F 等控制字符给字段列表排序，界面从不显示；去掉后再修剪空白。
    std::erase_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
    text = std::string(War3Map::Text::TrimCWhitespace(text));
    const auto asciiMnemonic = [](unsigned char c) { return c < 128 && std::isalnum(c); };
    if (text.size() >= 4 && text.ends_with(')') && text.compare(text.size() - 4, 2, "(&") == 0 &&
        asciiMnemonic(static_cast<unsigned char>(text[text.size() - 2]))) {
        text.resize(text.size() - 4);
    } else if (text.size() >= 8 && text.ends_with("）") && text.compare(text.size() - 8, 4, "（&") == 0 &&
        asciiMnemonic(static_cast<unsigned char>(text[text.size() - 4]))) {
        text.resize(text.size() - 8);
    }
    std::string result;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') result += text[i];
        else if (i + 1 < text.size() && text[i + 1] == '&') { result += '&'; ++i; }
        else if (i + 1 == text.size()) result += '&';
    }
    return std::string(War3Map::Text::TrimCWhitespace(result));
}
std::string Index(ObjectType type, std::string_view id) { return std::string(ToString(type)) + ':' + std::string(id); }

std::optional<NativeValueType> StorageType(const ObjectFieldDefinition& field) {
    const auto type = War3Map::Text::LowerAscii(field.valueType);
    if (type == "int" || type == "bool") return NativeValueType::Int32;
    if (type == "real") return NativeValueType::Real;
    if (type == "unreal") return NativeValueType::Unreal;
    if (!field.choices.empty()) {
        const bool numbers = std::all_of(field.choices.begin(), field.choices.end(), [](const auto& choice) { return Integer(choice.value).has_value(); });
        return numbers ? NativeValueType::Int32 : NativeValueType::String;
    }
    // 这些是原版 metadata 的字符串/资源/对象引用编码类型，并非字段 ID 白名单。
    static const std::set<std::string> strings{"string", "char", "icon", "model", "texture", "pathingtexture",
        "shadowtexture", "shadowimage", "soundlabel", "unitsound", "ubersplat", "lightningeffect", "orderstring",
        "abilcode", "unitcode", "upgradecode", "upgradeeffect"};
    if (strings.contains(type) || type.ends_with("list") || type.starts_with("pathinglist")) return NativeValueType::String;
    return std::nullopt;
}
std::vector<ObjectType> ReferenceTypes(std::string_view valueType) {
    const auto type = War3Map::Text::LowerAscii(valueType);
    if (type == "unitcode" || type == "unitlist") return {ObjectType::Unit};
    if (type == "abilcode" || type == "abilitylist" || type == "heroabilitylist") return {ObjectType::Ability};
    if (type == "itemlist") return {ObjectType::Item};
    if (type == "bufflist" || type == "effectlist") return {ObjectType::Buff};
    if (type == "upgradecode" || type == "upgradelist") return {ObjectType::Upgrade};
    if (type == "techlist") return {ObjectType::Unit, ObjectType::Upgrade};
    return {};
}
bool Extended(ObjectType type) { return type == ObjectType::Ability || type == ObjectType::Upgrade || type == ObjectType::Doodad; }
bool IsProfile(const ObjectFieldDefinition& field) { return War3Map::Text::LowerAscii(field.sourceTable) == "profile"; }

std::string SlkBase(const ObjectFieldDefinition& field) {
    std::string base = War3Map::Text::LowerAscii(field.sourceField);
    if (field.data > 0 && field.data <= 26) base += static_cast<char>('a' + field.data - 1);
    return base;
}
ObjectFieldMatch Address(const ObjectFieldDefinition& definition, std::uint32_t level) {
    ObjectFieldMatch match;
    match.definition = &definition;
    match.modification.fieldId = definition.fieldId;
    if (Extended(definition.objectType)) {
        match.modification.dataIndex = static_cast<std::uint32_t>(std::max(0, definition.data));
        if (definition.objectType == ObjectType::Doodad) match.modification.variation = level;
        else match.modification.level = level;
    }
    match.sourceKey = War3Map::Text::LowerAscii(definition.sourceField);
    if (IsProfile(definition)) {
        if (definition.repeat > 0 && definition.appendIndex != 0) {
            if (level > 1) match.sourceKey += std::to_string(level - 1);
        } else if (definition.repeat > 0) {
            if (level > 0) match.sourceIndex = static_cast<std::uint32_t>(std::max(0, definition.index)) + level - 1;
        } else if (definition.index >= 0) match.sourceIndex = static_cast<std::uint32_t>(definition.index);
    } else {
        match.sourceKey = SlkBase(definition);
        if (definition.repeat > 0 && level > 0) {
            if (definition.objectType == ObjectType::Doodad && level < 10) match.sourceKey += '0';
            match.sourceKey += std::to_string(level);
        }
    }
    match.safeToEdit = definition.nativeValueType.has_value();
    if (!match.safeToEdit) match.reason = "metadata 类型尚无可靠的原生值编码映射。";
    if (definition.repeat > 0 && level == 0) { match.safeToEdit = false; match.reason = "需要明确等级或变体。"; }
    if (level > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) || definition.data < 0 || definition.data > 26) {
        match.safeToEdit = false; match.reason = "字段索引超出可靠的原生编码范围。";
    }
    if (definition.objectType == ObjectType::Doodad && definition.repeat > 0) {
        match.safeToEdit = false; match.reason = "变体字段已识别，原生变体编号写回尚待独立验证。";
    }
    return match;
}
} // namespace

std::vector<ObjectFieldSchemaSource> ObjectFieldSchema::DefaultSources() {
    return {{ObjectType::Unit, "Units/UnitMetaData.slk"}, {ObjectType::Item, "Units/UnitMetaData.slk"},
        {ObjectType::Ability, "Units/AbilityMetaData.slk"}, {ObjectType::Buff, "Units/AbilityBuffMetaData.slk"},
        {ObjectType::Upgrade, "Units/UpgradeMetaData.slk"}, {ObjectType::Doodad, "Doodads/DoodadMetaData.slk"},
        {ObjectType::Destructable, "Units/DestructableMetaData.slk"}};
}

Result<ObjectFieldSchema> ObjectFieldSchema::Load(const Game::GameDataSource& source, const ObjectFieldSchemaOptions& options) {
    return Load([&source](const auto& path) { return source.ReadFile(path); }, options);
}
Result<ObjectFieldSchema> ObjectFieldSchema::Load(const FileReader& reader, const ObjectFieldSchemaOptions& options) {
    ObjectFieldSchema schema;
    std::vector<Diagnostic> diagnostics;
    auto read = [&](const std::string& path, bool required) -> std::optional<std::vector<std::byte>> {
        if (path.empty()) return std::nullopt;
        auto bytes = reader(path);
        if (!bytes.Succeeded() || bytes.value->size() > options.maximumFileBytes) {
            if (required) diagnostics.push_back({DiagnosticSeverity::Warning, "Object.Schema", "metadata-unavailable", "无法读取 metadata: " + path, std::nullopt});
            return std::nullopt;
        }
        return std::move(*bytes.value);
    };
    Fields strings;
    for (const auto& path : options.stringFiles) if (const auto bytes = read(path, false)) {
        for (const auto& [section, fields] : Profile(Text(*bytes))) {
            (void)section;
            for (const auto& [key, value] : fields) strings[key] = value;
        }
    }
    Rows worldData;
    if (const auto bytes = read(options.worldEditDataFile, false)) worldData = Profile(Text(*bytes));
    Rows editorData;
    if (const auto bytes = read(options.unitEditorDataFile, false)) editorData = Profile(Text(*bytes));
    std::unordered_map<std::string, std::vector<ObjectFieldChoice>> choices;
    for (const auto& [type, fields] : editorData) {
        for (const auto& [key, value] : fields) {
            if (key.empty() || !std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
            const auto tokens = Split(value);
            if (tokens.size() < 2) continue;
            choices[type].push_back({Token(tokens[0]), DisplayLabel(Resolve(Token(tokens[1]), strings, options.strings)), Ids(Get(fields, key + "_alt"))});
        }
        if (auto found = choices.find(type); found != choices.end()) {
            std::sort(found->second.begin(), found->second.end(), [](const auto& a, const auto& b) { return a.value < b.value; });
        }
    }
    std::unordered_map<std::string, Rows> tables;
    for (const auto& source : options.sources.empty() ? DefaultSources() : options.sources) {
        if (!tables.contains(source.metadataFile)) {
            const auto bytes = read(source.metadataFile, true);
            if (!bytes) { tables[source.metadataFile] = {}; continue; }
            auto parsed = Slk(Text(*bytes));
            if (!parsed.Succeeded()) {
                diagnostics.push_back({DiagnosticSeverity::Warning, "Object.Schema", "metadata-invalid", "无效 metadata: " + source.metadataFile, std::nullopt});
                tables[source.metadataFile] = {}; continue;
            }
            tables[source.metadataFile] = std::move(*parsed.value);
        }
        for (const auto& [id, metadata] : tables.at(source.metadataFile)) {
            if (source.type == ObjectType::Item && !Flag(metadata, "useitem")) continue;
            if (source.type == ObjectType::Unit && !Flag(metadata, "useunit") && !Flag(metadata, "usehero") && !Flag(metadata, "usebuilding")) continue;
            ObjectFieldDefinition field;
            field.objectType = source.type; field.fieldId = id; field.metadata = metadata;
            field.sourceField = Get(metadata, "field"); field.sourceTable = Get(metadata, "slk");
            if (field.sourceField.empty() || field.sourceTable.empty()) continue;
            field.displayNameToken = Get(metadata, "displayname");
            field.displayName = DisplayLabel(Resolve(field.displayNameToken, strings, options.strings));
            field.displayNameResolved = !field.displayName.empty() && !field.displayName.starts_with("WESTRING_");
            if (field.displayName.empty()) field.displayName = field.sourceField;
            field.category = Get(metadata, "category");
            field.categoryDisplayName = field.category;
            if (const auto categories = worldData.find("objecteditorcategories"); categories != worldData.end()) {
                const auto name = Get(categories->second, field.category);
                if (!name.empty()) field.categoryDisplayName = DisplayLabel(Resolve(name, strings, options.strings));
            }
            field.valueType = Get(metadata, "type");
            if (const auto found = choices.find(War3Map::Text::LowerAscii(field.valueType)); found != choices.end()) field.choices = found->second;
            field.repeat = Int(metadata, "repeat"); field.data = Int(metadata, "data"); field.index = Int(metadata, "index", -1);
            field.appendIndex = Int(metadata, "appendindex"); field.effectIndex = Int(metadata, "effectindex"); field.effectType = Get(metadata, "effecttype");
            field.useSpecific = Ids(Get(metadata, "usespecific")); field.notSpecific = Ids(Get(metadata, "notspecific"));
            field.useUnit = Flag(metadata, "useunit"); field.useHero = Flag(metadata, "usehero");
            field.useBuilding = Flag(metadata, "usebuilding"); field.useItem = Flag(metadata, "useitem"); field.useCreep = Flag(metadata, "usecreep");
            field.canBeEmpty = Flag(metadata, "canbeempty", false); field.minimum = Get(metadata, "minval"); field.maximum = Get(metadata, "maxval");
            field.sort = Get(metadata, "sort"); field.isList = War3Map::Text::LowerAscii(field.valueType).ends_with("list") || War3Map::Text::LowerAscii(field.valueType).starts_with("pathinglist");
            field.referenceTypes = ReferenceTypes(field.valueType); field.nativeValueType = StorageType(field);
            const auto key = Index(field.objectType, field.fieldId);
            if (const auto old = schema.nativeIndex_.find(key); old != schema.nativeIndex_.end()) schema.definitions_[old->second] = std::move(field);
            else { schema.nativeIndex_[key] = schema.definitions_.size(); schema.definitions_.push_back(std::move(field)); }
        }
    }
    if (schema.definitions_.empty()) return Result<ObjectFieldSchema>::Failure("Object.Schema", "schema-empty", "未读取到物体字段定义。");
    auto result = Result<ObjectFieldSchema>::Success(std::move(schema));
    result.diagnostics = std::move(diagnostics);
    return result;
}

const ObjectFieldDefinition* ObjectFieldSchema::FindNative(ObjectType type, std::string_view fieldId) const {
    const auto found = nativeIndex_.find(Index(type, fieldId));
    return found == nativeIndex_.end() ? nullptr : &definitions_[found->second];
}
bool ObjectFieldSchema::AppliesTo(const ObjectFieldDefinition& field, const ObjectFieldContext& context) const {
    auto matches = [&](const auto& ids) { return std::find(ids.begin(), ids.end(), context.rawcode) != ids.end()
        || (!context.baseCode.empty() && std::find(ids.begin(), ids.end(), context.baseCode) != ids.end()); };
    if (!field.useSpecific.empty() && !matches(field.useSpecific)) return false;
    if (!field.notSpecific.empty() && matches(field.notSpecific)) return false;
    if (context.hero == true && !field.useHero) return false;
    if (context.building == true && !field.useBuilding) return false;
    if ((field.objectType == ObjectType::Item || context.item == true) && !field.useItem) return false;
    if (context.creep == true && !field.useCreep) return false;
    if (context.hero == false && context.building == false && context.item == false && context.creep == false && !field.useUnit) return false;
    return true;
}

ObjectFieldMatch ObjectFieldSchema::MatchSourceField(ObjectType type, std::string_view sourceKey,
    const ObjectFieldContext& context, std::optional<std::uint32_t> sourceIndex) const {
    const auto key = War3Map::Text::LowerAscii(sourceKey);
    std::vector<ObjectFieldMatch> matches;
    for (const auto& field : definitions_) {
        if (field.objectType != type || !AppliesTo(field, context)) continue;
        std::uint32_t level = 0;
        if (IsProfile(field)) {
            const auto base = War3Map::Text::LowerAscii(field.sourceField);
            if (field.repeat > 0 && field.appendIndex != 0) {
                if (key == base) level = 1;
                else if (key.starts_with(base)) {
                    const auto suffix = Integer(std::string_view(key).substr(base.size()));
                    if (!suffix || *suffix < 1) continue;
                    level = static_cast<std::uint32_t>(*suffix) + 1;
                } else continue;
            } else {
                if (key != base) continue;
                if (field.repeat > 0) {
                    if (sourceIndex && *sourceIndex >= static_cast<std::uint32_t>(std::max(0, field.index)))
                        level = *sourceIndex - static_cast<std::uint32_t>(std::max(0, field.index)) + 1;
                } else if (sourceIndex && field.index >= 0 && *sourceIndex != static_cast<std::uint32_t>(field.index)) continue;
            }
        } else {
            const auto base = SlkBase(field);
            if (field.repeat > 0) {
                if (!key.starts_with(base)) continue;
                const auto suffix = Integer(std::string_view(key).substr(base.size()));
                if (!suffix || *suffix < 1) continue;
                level = static_cast<std::uint32_t>(*suffix);
            } else if (key != base) continue;
        }
        matches.push_back(Address(field, level));
    }
    if (matches.size() == 1) return std::move(matches.front());
    ObjectFieldMatch result;
    result.sourceKey = key;
    result.reason = matches.empty() ? "未找到适用于该对象的字段定义。" : "原始键对应多个字段，需要明确分量或适用对象。";
    return result;
}

ObjectFieldMatch ObjectFieldSchema::MatchNativeField(ObjectType type, const ModificationKey& key,
    const ObjectFieldContext& context) const {
    const auto* field = FindNative(type, key.fieldId);
    if (!field) return {nullptr, key, {}, {}, false, "未知原生字段 ID。"};
    const auto level = type == ObjectType::Doodad ? key.variation.value_or(0) : key.level.value_or(0);
    auto result = Address(*field, level);
    if (!AppliesTo(*field, context)) { result.safeToEdit = false; result.reason = "字段不适用于当前对象，或缺少对象身份。"; }
    if (key.dataIndex.value_or(0) != static_cast<std::uint32_t>(std::max(0, field->data))) {
        result.safeToEdit = false; result.reason = "原生 data 索引与 metadata 不一致。";
    }
    if ((!Extended(type) && (key.level.value_or(0) || key.variation.value_or(0))) || (field->repeat == 0 && level != 0)) {
        result.safeToEdit = false; result.reason = "非重复字段不能携带等级或变体索引。";
    }
    return result;
}

} // namespace War3Map::ObjectData
