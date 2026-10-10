#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>

namespace War3Map::ObjectData {
namespace {

using Fields = std::unordered_map<std::string, std::string>;
using Rows = std::map<std::string, Fields>;

std::string DecodeToken(std::string_view text) {
    text = War3Map::Text::TrimCWhitespace(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        result.push_back(text[index]);
        if (text[index] == '"' && index + 1 < text.size() && text[index + 1] == '"') ++index;
    }
    return result;
}

std::vector<std::string_view> SplitQuoted(std::string_view text, char separator) {
    std::vector<std::string_view> result;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '"') {
            if (quoted && index + 1 < text.size() && text[index + 1] == '"') ++index;
            else quoted = !quoted;
        } else if (!quoted && text[index] == separator) {
            result.push_back(text.substr(start, index - start));
            start = index + 1;
        }
    }
    result.push_back(text.substr(start));
    return result;
}

std::string FirstValue(std::string_view text) {
    const auto parts = SplitQuoted(text, ',');
    return parts.empty() ? std::string{} : DecodeToken(parts.front());
}

bool Present(std::string_view text) {
    return !text.empty() && text != "_" && text != "-";
}

std::string Get(const Fields& fields, std::string_view key) {
    const auto found = fields.find(War3Map::Text::LowerAscii(key));
    return found == fields.end() ? std::string{} : found->second;
}

std::string IndexKey(ObjectType type, std::string_view rawcode) {
    return std::string(ToString(type)) + ':' + std::string(rawcode);
}

std::string_view Text(const std::vector<std::byte>& bytes) {
    if (bytes.empty()) return {};
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return text;
}

// 只处理目录使用的 SYLK C/X/Y/K 记录；F/P/B 等格式记录不影响语义单元格。
Result<Rows> ParseSlk(std::string_view text) {
    auto parsed = War3Map::Text::ParseSlkCells(text, War3Map::Text::SlkCellMode::CatalogStrict);
    if (!parsed.Succeeded() || !parsed.value) {
        Result<Rows> failed;
        failed.diagnostics = std::move(parsed.diagnostics);
        return failed;
    }
    const auto& cells = *parsed.value;
    const auto& headers = cells.at(1);
    Rows rows;
    for (const auto& [rowIndex, row] : cells) {
        if (rowIndex == 1) continue;
        const auto id = row.find(1);
        if (id == row.end() || id->second.size() != 4) continue;
        auto& fields = rows[id->second];
        for (const auto& [column, value] : row) {
            const auto header = headers.find(column);
            if (header != headers.end()) fields[War3Map::Text::LowerAscii(header->second)] = value;
        }
    }
    return Result<Rows>::Success(std::move(rows));
}

Rows ParseProfile(std::string_view text) {
    Rows rows;
    std::string section;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = text.find('\n', start);
        auto line = War3Map::Text::TrimCWhitespace(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        start = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty() || line.starts_with("//") || line.front() == ';') continue;
        if (line.front() == '[') {
            const auto closing = line.find(']');
            section = closing == std::string_view::npos ? std::string{} : std::string(War3Map::Text::TrimCWhitespace(line.substr(1, closing - 1)));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const auto key = War3Map::Text::TrimCWhitespace(line.substr(0, equals));
        if (key.empty()) continue;
        // 保留 CSV/路径/转义符，显示名称和图标再独立取首项。
        rows[section][War3Map::Text::LowerAscii(key)] = std::string(War3Map::Text::TrimCWhitespace(line.substr(equals + 1)));
    }
    return rows;
}

// 统一字符串表优先（YDWE 覆盖层 → 游戏 → 英文原版），其次本次读取的 stringFiles。
std::string Resolve(std::string value, const Fields& strings, const Game::WorldEditStrings* table) {
    std::set<std::string> visited;
    while (value.starts_with("WESTRING_") && visited.insert(value).second) {
        if (table != nullptr) {
            if (const auto* text = table->Find(value)) {
                value = DecodeToken(*text);
                continue;
            }
        }
        const auto found = strings.find(War3Map::Text::LowerAscii(value));
        if (found == strings.end()) break;
        value = DecodeToken(found->second);
    }
    return value;
}

std::string Category(const StandardObjectEntry& entry) {
    const auto& fields = entry.fields;
    switch (entry.key.type) {
    case ObjectType::Unit:
        if (Get(fields, "isbldg") == "1" || Get(fields, "isbld") == "1") return "building";
        if (!entry.key.rawcode.empty() && entry.key.rawcode.front() >= 'A' && entry.key.rawcode.front() <= 'Z') return "hero";
        return "unit";
    case ObjectType::Ability:
        if (Get(fields, "item") == "1") return "item";
        if (Get(fields, "hero") == "1") return "hero";
        return "unit";
    case ObjectType::Buff: return Get(fields, "iseffect") == "1" ? "effect" : "buff";
    case ObjectType::Item:
    case ObjectType::Upgrade: return Get(fields, "class");
    case ObjectType::Doodad:
    case ObjectType::Destructable: return Get(fields, "category");
    }
    return {};
}

std::pair<std::string, std::string> PresentationValue(
    const StandardObjectEntry& entry, const std::vector<std::string>& candidates,
    const std::string& parentField, const std::map<std::string, StandardObjectEntry>& entries,
    const Rows& profiles) {
    const Fields* current = &entry.fields;
    std::string currentRawcode = entry.key.rawcode;
    std::set<std::string> visited;
    while (current && visited.insert(currentRawcode).second) {
        bool explicitlySet = false;
        for (const auto& candidate : candidates) {
            const auto found = current->find(War3Map::Text::LowerAscii(candidate));
            if (found == current->end()) continue;
            explicitlySet = true;
            const auto value = FirstValue(found->second);
            if (Present(value)) return {value, currentRawcode};
        }
        // 显式清空的字段不应被基类图标/名称重新填上。
        if (explicitlySet || parentField.empty()) break;
        currentRawcode = Get(*current, parentField);
        const auto parent = entries.find(currentRawcode);
        if (parent != entries.end()) current = &parent->second.fields;
        else {
            // 部分原生 code 只有 profile 原型，不在 SLK 可选择物体集合中。
            // 只借用展示字段，不能把这些原型变成额外目录成员。
            const auto donor = profiles.find(currentRawcode);
            current = donor == profiles.end() ? nullptr : &donor->second;
        }
    }
    return {};
}

std::vector<std::string> Profiles(std::string_view suffix, bool ability = false) {
    std::vector<std::string> paths;
    std::vector<std::string> prefixes{"Human", "Orc", "Undead", "NightElf", "Neutral", "Campaign"};
    if (ability) { prefixes.insert(prefixes.begin(), "Common"); prefixes.push_back("Item"); }
    for (const auto& prefix : prefixes) {
        paths.push_back("Units/" + prefix + std::string(suffix) + "Func.txt");
        paths.push_back("Units/" + prefix + std::string(suffix) + "Strings.txt");
    }
    return paths;
}

} // namespace

std::string_view StandardObjectEntry::Field(std::string_view name) const {
    const auto found = fields.find(War3Map::Text::LowerAscii(name));
    return found == fields.end() ? std::string_view{} : found->second;
}

std::vector<CatalogSourceSpec> StandardObjectCatalog::DefaultSources() {
    return {
        {ObjectType::Unit, {"Units/UnitData.slk", "Units/UnitUI.slk", "Units/UnitBalance.slk",
            "Units/UnitWeapons.slk", "Units/UnitAbilities.slk"}, Profiles("Unit")},
        {ObjectType::Ability, {"Units/AbilityData.slk"}, Profiles("Ability", true),
            {"name", "editorname", "bufftip"}, {"art", "researchart", "icon"}, "code"},
        {ObjectType::Item, {"Units/ItemData.slk"}, {"Units/ItemFunc.txt", "Units/ItemStrings.txt"}},
        {ObjectType::Buff, {"Units/AbilityBuffData.slk"}, Profiles("Ability", true),
            {"editorname", "name", "bufftip"}, {"buffart", "art", "icon"}, "code"},
        {ObjectType::Upgrade, {"Units/UpgradeData.slk"}, Profiles("Upgrade")},
        {ObjectType::Doodad, {"Doodads/Doodads.slk"}, {}},
        {ObjectType::Destructable, {"Units/DestructableData.slk"}, {}},
    };
}

Result<StandardObjectCatalog> StandardObjectCatalog::Load(
    const Game::GameDataSource& source, const CatalogLoadOptions& options) {
    return Load([&source](const std::string& path) { return source.ReadFile(path); }, options);
}

Result<StandardObjectCatalog> StandardObjectCatalog::Load(
    const FileReader& reader, const CatalogLoadOptions& options) {
    StandardObjectCatalog catalog;
    std::vector<Diagnostic> diagnostics;
    std::unordered_map<std::string, Rows> profileCache;
    auto warn = [&](std::string code, std::string message) {
        diagnostics.push_back({DiagnosticSeverity::Warning, "Object.Catalog", std::move(code), std::move(message), std::nullopt});
    };
    auto read = [&](const std::string& path, bool required) -> std::optional<std::vector<std::byte>> {
        auto loaded = reader(path);
        if (!loaded.Succeeded()) {
            if (required) warn("source-unavailable", "无法读取物体目录表: " + path);
            return std::nullopt;
        }
        if (loaded.value->size() > options.maximumFileBytes) {
            warn("source-too-large", "目录数据超过读取大小限制: " + path);
            return std::nullopt;
        }
        if (std::find(catalog.loadedFiles_.begin(), catalog.loadedFiles_.end(), path) == catalog.loadedFiles_.end()) {
            catalog.loadedFiles_.push_back(path);
        }
        return std::move(*loaded.value);
    };
    Fields strings;
    for (const auto& path : options.stringFiles) {
        if (const auto bytes = read(path, false)) {
            for (const auto& [section, values] : ParseProfile(Text(*bytes))) {
                (void)section;
                for (const auto& [key, value] : values) strings[key] = value;
            }
        }
    }
    const auto sources = options.sources.empty() ? DefaultSources() : options.sources;
    for (const auto& spec : sources) {
        if (spec.slkFiles.empty()) continue;
        std::map<std::string, StandardObjectEntry> entries;
        Rows presentationProfiles;
        for (std::size_t table = 0; table < spec.slkFiles.size(); ++table) {
            const auto& path = spec.slkFiles[table];
            const auto bytes = read(path, true);
            if (!bytes) continue;
            auto parsed = ParseSlk(Text(*bytes));
            if (!parsed.Succeeded()) {
                warn("slk-invalid", "未接受无效 SLK: " + path + " — " + parsed.diagnostics.front().message);
                continue;
            }
            for (const auto& [id, fields] : *parsed.value) {
                if (table != 0 && !entries.contains(id)) continue;
                auto& entry = entries[id];
                entry.key = {spec.type, id};
                for (const auto& [key, value] : fields) entry.fields[key] = value;
                entry.sourceFiles.push_back(path);
            }
        }
        for (const auto& path : spec.profileFiles) {
            if (!profileCache.contains(path)) {
                const auto bytes = read(path, false);
                profileCache[path] = bytes ? ParseProfile(Text(*bytes)) : Rows{};
            }
            for (const auto& [id, fields] : profileCache.at(path)) {
                if (id.size() == 4) {
                    for (const auto& [key, value] : fields) presentationProfiles[id][key] = value;
                }
                const auto found = entries.find(id);
                if (found == entries.end()) continue;
                for (const auto& [key, value] : fields) found->second.fields[key] = value;
                found->second.sourceFiles.push_back(path);
            }
        }
        std::size_t unresolved = 0;
        for (auto& [id, entry] : entries) {
            const auto name = PresentationValue(entry, spec.nameFields, spec.presentationParentField, entries, presentationProfiles);
            entry.displayName = Resolve(name.first, strings, options.strings);
            entry.nameSourceRawcode = name.second;
            entry.nameResolved = Present(entry.displayName) && !entry.displayName.starts_with("WESTRING_");
            if (!entry.nameResolved) ++unresolved;
            if (entry.displayName.empty()) entry.displayName = id;
            const auto icon = PresentationValue(entry, spec.iconFields, spec.presentationParentField, entries, presentationProfiles);
            entry.iconPath = icon.first;
            entry.iconSourceRawcode = icon.second;
            entry.race = FirstValue(Get(entry.fields, "race"));
            entry.category = Category(entry);
            entry.editorVisible = Get(entry.fields, "ineditor") != "0" &&
                Get(entry.fields, "useineditor") != "0" && Get(entry.fields, "hiddenineditor") != "1";
            const auto key = IndexKey(entry.key.type, id);
            if (const auto existing = catalog.index_.find(key); existing != catalog.index_.end()) {
                catalog.entries_[existing->second] = entry;
            } else {
                catalog.index_[key] = catalog.entries_.size();
                // 同一表里稍后的别名仍可能读取本行的展示字段。
                catalog.entries_.push_back(entry);
            }
        }
        if (unresolved) warn("name-unresolved", std::string(ToString(spec.type)) + ": " +
            std::to_string(unresolved) + " 个物体没有可解析的显示名称，保留原字符串或 rawcode。");
    }
    if (catalog.entries_.empty()) return Result<StandardObjectCatalog>::Failure(
        "Object.Catalog", "catalog-empty", "未读取到任何标准物体；请检查游戏资源与目录布局。");
    auto result = Result<StandardObjectCatalog>::Success(std::move(catalog));
    result.diagnostics = std::move(diagnostics);
    return result;
}

const StandardObjectEntry* StandardObjectCatalog::Find(ObjectType type, std::string_view rawcode) const {
    const auto found = index_.find(IndexKey(type, rawcode));
    return found == index_.end() ? nullptr : &entries_[found->second];
}

std::size_t StandardObjectCatalog::Count(ObjectType type) const noexcept {
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(),
        [type](const auto& entry) { return entry.key.type == type; }));
}

} // namespace War3Map::ObjectData
