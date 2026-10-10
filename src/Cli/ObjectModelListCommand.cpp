#include "ObjectModelListCommand.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using War3Map::ObjectData::ObjectType;

struct FieldSpec {
    ObjectType type;
    std::string fieldId;
    std::string sourceKey;
    std::string valueType;
    bool repeat = false;
    std::size_t matchedRows = 0;
};

struct Candidate {
    std::string path;
    std::string objectType;
    std::string rawcode;
    std::string field;
    std::size_t references = 0;
};

struct InvalidValue {
    std::string value;
    std::string objectType;
    std::string rawcode;
    std::string field;
};

std::string Lower(std::string_view value) {
    return War3Map::Text::LowerAscii(value);
}

std::string Trim(std::string_view value) {
    return std::string(War3Map::Text::TrimCWhitespace(value));
}

std::string_view Text(const std::vector<std::byte>& bytes) {
    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return text;
}

void PrintDiagnostics(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
}

bool EssentialSourcesLoaded(const War3Map::ObjectData::StandardObjectCatalog& catalog,
    const War3Map::Game::GameDataSource& game) {
    std::set<std::string> loaded(catalog.LoadedFiles().begin(), catalog.LoadedFiles().end());
    for (const auto& source : War3Map::ObjectData::StandardObjectCatalog::DefaultSources()) {
        for (const auto& file : source.slkFiles) {
            if (loaded.contains(file)) continue;
            std::cerr << "Object.ModelList: CATALOG_SOURCE_MISSING: " << file << '\n';
            return false;
        }
    }
    std::set<std::string> metadata;
    for (const auto& source : War3Map::ObjectData::ObjectFieldSchema::DefaultSources())
        metadata.insert(source.metadataFile);
    for (const auto& file : metadata) {
        const auto bytes = game.ReadFileBounded(file, 32 * 1024 * 1024);
        if (bytes.Succeeded()) continue;
        std::cerr << "Object.ModelList: METADATA_SOURCE_UNREADABLE: " << file << '\n';
        PrintDiagnostics(bytes.diagnostics);
        return false;
    }
    return true;
}

bool ModelType(std::string_view valueType) {
    const auto type = Lower(valueType);
    return type == "model" || type == "modellist";
}

std::vector<FieldSpec> ModelFields(const War3Map::ObjectData::ObjectFieldSchema& schema) {
    std::vector<FieldSpec> fields;
    for (const auto& definition : schema.Definitions()) {
        if (!ModelType(definition.valueType)) continue;
        std::string key = Lower(definition.sourceField);
        if (Lower(definition.sourceTable) != "profile" && definition.data > 0 && definition.data <= 26)
            key += static_cast<char>('a' + definition.data - 1);
        fields.push_back({definition.objectType, definition.fieldId, std::move(key),
            definition.valueType, definition.repeat > 0, 0});
    }
    std::sort(fields.begin(), fields.end(), [](const auto& left, const auto& right) {
        if (left.type != right.type) return left.type < right.type;
        if (left.sourceKey != right.sourceKey) return left.sourceKey < right.sourceKey;
        return left.fieldId < right.fieldId;
    });
    return fields;
}

bool Matches(const FieldSpec& spec, std::string_view key) {
    if (key == spec.sourceKey) return true;
    if (!spec.repeat || !key.starts_with(spec.sourceKey)) return false;
    const auto suffix = key.substr(spec.sourceKey.size());
    return !suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}

std::vector<std::string> Values(std::string_view text) {
    std::vector<std::string> values;
    std::string value;
    bool quoted = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '"') {
            if (quoted && index + 1 < text.size() && text[index + 1] == '"') {
                value += '"';
                ++index;
            } else quoted = !quoted;
        } else if (c == ',' && !quoted) {
            values.push_back(Trim(value));
            value.clear();
        } else value += c;
    }
    values.push_back(Trim(value));
    return values;
}

bool Placeholder(std::string_view value) {
    const auto lower = Lower(value);
    return lower.empty() || lower == "_" || lower == "-" || lower == "0" || lower == "none" || lower == "null";
}

std::optional<std::string> ModelPath(std::string value) {
    std::replace(value.begin(), value.end(), '/', '\\');
    const auto lower = Lower(value);
    if (lower.ends_with(".mdl") || lower.ends_with(".mdx")) value.resize(value.size() - 4);
    else {
        const auto dot = value.find_last_of('.');
        const auto slash = value.find_last_of('\\');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return std::nullopt;
    }
    value += ".mdx";
    if (!War3Map::Resources::CanonicalImportPath(value).Succeeded()) return std::nullopt;
    return value;
}

std::optional<std::uint32_t> VariationCount(const War3Map::ObjectData::StandardObjectEntry& entry) {
    const auto raw = Trim(entry.Field("numvar"));
    if (raw.empty()) return 1;
    std::uint32_t count = 0;
    const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() || count < 1 || count > 1024)
        return std::nullopt;
    return count;
}

std::string Cell(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char c : value) {
        if (c == '\t') result += "\\t";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result;
}

bool WriteNew(const std::filesystem::path& path, std::string_view contents) {
    if (std::filesystem::exists(path)) {
        std::cerr << "Object.ModelList: OUTPUT_EXISTS: " << path.string() << '\n';
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.flush();
    if (out) return true;
    std::cerr << "Object.ModelList: OUTPUT_WRITE_FAILED: " << path.string() << '\n';
    return false;
}

} // namespace

int RunObjectModelList(const std::filesystem::path& gameRoot, const std::filesystem::path& outputList) {
    auto mounted = War3Map::Game::GameDataSource::MountStrict(gameRoot);
    if (!mounted.Succeeded()) { PrintDiagnostics(mounted.diagnostics); return 1; }
    const auto& game = *mounted.value;
    auto catalog = War3Map::ObjectData::StandardObjectCatalog::Load(game);
    if (!catalog.Succeeded()) { PrintDiagnostics(catalog.diagnostics); return 1; }
    if (!EssentialSourcesLoaded(*catalog.value, game)) return 1;
    auto schema = War3Map::ObjectData::ObjectFieldSchema::Load(game);
    if (!schema.Succeeded()) { PrintDiagnostics(schema.diagnostics); return 1; }
    for (const auto& diagnostic : schema.diagnostics) {
        if (diagnostic.code == "metadata-unavailable" || diagnostic.code == "metadata-invalid") {
            PrintDiagnostics(schema.diagnostics);
            return 1;
        }
    }
    auto specs = ModelFields(*schema.value);
    if (specs.empty()) {
        std::cerr << "Object.ModelList: NO_MODEL_FIELDS: standard metadata defines no model fields\n";
        return 1;
    }
    std::map<std::string, Candidate> candidates;
    std::vector<InvalidValue> invalid;
    std::array<std::size_t, 7> objects{};
    std::size_t fieldValues = 0, portraitPaths = 0;
    const auto add = [&](const std::string& path, const War3Map::ObjectData::StandardObjectEntry& entry,
                         std::string_view key) {
        auto canonical = War3Map::Resources::CanonicalImportPath(path);
        if (!canonical.Succeeded()) {
            invalid.push_back({path, std::string(War3Map::ObjectData::ToString(entry.key.type)), entry.key.rawcode, std::string(key)});
            return false;
        }
        auto& candidate = candidates[*canonical.value];
        const bool inserted = candidate.references++ == 0;
        if (inserted) {
            candidate.path = path;
            candidate.objectType = War3Map::ObjectData::ToString(entry.key.type);
            candidate.rawcode = entry.key.rawcode;
            candidate.field = key;
        }
        return inserted;
    };
    const auto consume = [&](const War3Map::ObjectData::StandardObjectEntry& entry) -> bool {
        std::vector<std::string> keys;
        keys.reserve(entry.fields.size());
        for (const auto& [key, value] : entry.fields) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
            bool modelField = false;
            for (auto& spec : specs) {
                if (spec.type == entry.key.type && Matches(spec, key)) {
                    ++spec.matchedRows;
                    modelField = true;
                }
            }
            if (!modelField) continue;
            for (auto value : Values(entry.Field(key))) {
                if (Placeholder(value)) continue;
                ++fieldValues;
                auto path = ModelPath(value);
                if (!path) {
                    invalid.push_back({value, std::string(War3Map::ObjectData::ToString(entry.key.type)), entry.key.rawcode, key});
                    continue;
                }
                const bool variant = (entry.key.type == ObjectType::Doodad || entry.key.type == ObjectType::Destructable) && key == "file";
                if (variant) {
                    const auto count = VariationCount(entry);
                    if (!count) {
                        std::cerr << "Object.ModelList: VARIATION_INVALID: " << entry.key.rawcode << '\n';
                        return false;
                    }
                    for (std::uint32_t variation = 0; variation < *count; ++variation)
                        add(War3Map::Render::MdxModelFile(*path, variation, *count), entry, key);
                } else add(*path, entry, key);
                if (entry.key.type == ObjectType::Unit && key == "file") {
                    std::string portrait = path->substr(0, path->size() - 4) + "_Portrait.mdx";
                    if (game.HasFile(portrait)) {
                        portraitPaths += add(portrait, entry, "file:_Portrait") ? 1 : 0;
                    }
                }
            }
        }
        return true;
    };
    for (const auto& entry : catalog.value->Entries()) {
        ++objects[static_cast<std::size_t>(entry.key.type)];
        if (!consume(entry)) return 1;
    }
    const auto catalogPaths = candidates.size();
    std::size_t sourceRows = 0;
    // 目录有意不把 profile 原型与补充 SLK 行当作可选择对象；扫描器仍需看见这些
    // 原版数据里实际写出的模型路径，因此把原始行只读地并入同一候选集合。
    for (const auto& source : War3Map::ObjectData::StandardObjectCatalog::DefaultSources()) {
        for (const auto& file : source.slkFiles) {
            auto bytes = game.ReadFileBounded(file, 32 * 1024 * 1024);
            if (!bytes.Succeeded()) { PrintDiagnostics(bytes.diagnostics); return 1; }
            auto cells = War3Map::Text::ParseSlkCells(Text(*bytes.value), War3Map::Text::SlkCellMode::CatalogStrict);
            if (!cells.Succeeded()) { PrintDiagnostics(cells.diagnostics); return 1; }
            const auto& headers = cells.value->at(1);
            for (const auto& [rowNumber, row] : *cells.value) {
                if (rowNumber == 1) continue;
                const auto id = row.find(1);
                if (id == row.end() || id->second.size() != 4) continue;
                War3Map::ObjectData::StandardObjectEntry entry;
                entry.key = {source.type, id->second};
                for (const auto& [column, value] : row) {
                    const auto header = headers.find(column);
                    if (header != headers.end()) entry.fields[Lower(header->second)] = value;
                }
                if (!consume(entry)) return 1;
                ++sourceRows;
            }
        }
        for (const auto& file : source.profileFiles) {
            if (!game.HasFile(file)) continue;
            auto bytes = game.ReadFileBounded(file, 32 * 1024 * 1024);
            if (!bytes.Succeeded()) { PrintDiagnostics(bytes.diagnostics); return 1; }
            for (const auto& [section, fields] : War3Map::Text::ParseProfile(Text(*bytes.value))) {
                if (section.size() != 4) continue;
                War3Map::ObjectData::StandardObjectEntry entry;
                entry.key = {source.type, section};
                entry.fields = fields;
                if (!consume(entry)) return 1;
                ++sourceRows;
            }
        }
    }
    if (candidates.empty() || candidates.size() > 100000) {
        std::cerr << "Object.ModelList: CANDIDATE_LIMIT: no models or over 100000 models\n";
        return 1;
    }
    std::string list, missing = "path\tstate\tobject_type\trawcode\tfield\treferences\n";
    std::size_t readable = 0, missingCount = 0;
    for (const auto& [canonical, candidate] : candidates) {
        list += candidate.path + '\n';
        const auto header = game.ReadFileHead(candidate.path, 4);
        if (header.Succeeded()) { ++readable; continue; }
        ++missingCount;
        missing += Cell(candidate.path) + '\t' + (game.HasFile(candidate.path) ? "unreadable" : "absent") + '\t'
            + candidate.objectType + '\t' + candidate.rawcode + '\t' + Cell(candidate.field) + '\t'
            + std::to_string(candidate.references) + '\n';
    }
    for (const auto& item : invalid) {
        missing += Cell(item.value) + "\tinvalid_path\t" + item.objectType + '\t' + item.rawcode + '\t'
            + Cell(item.field) + "\t1\n";
    }
    std::string fields = "object_type\tfield_id\tsource_key\tvalue_type\trepeat\tmatched_rows\n";
    for (const auto& spec : specs) {
        fields += std::string(War3Map::ObjectData::ToString(spec.type)) + '\t' + spec.fieldId + '\t'
            + spec.sourceKey + '\t' + spec.valueType + '\t' + (spec.repeat ? "1" : "0") + '\t'
            + std::to_string(spec.matchedRows) + '\n';
    }
    auto missingPath = outputList;
    missingPath += ".missing.tsv";
    auto fieldPath = outputList;
    fieldPath += ".fields.tsv";
    if (std::filesystem::exists(outputList) || std::filesystem::exists(missingPath) || std::filesystem::exists(fieldPath)) {
        std::cerr << "Object.ModelList: OUTPUT_EXISTS: use a new output path\n";
        return 1;
    }
    if (!WriteNew(missingPath, missing) || !WriteNew(fieldPath, fields) || !WriteNew(outputList, list)) return 1;
    std::cout << "MODEL_LIST_SUMMARY entries=" << catalog.value->Entries().size() << " model_fields=" << specs.size()
        << " source_rows=" << sourceRows << " catalog_paths=" << catalogPaths
        << " field_values=" << fieldValues << " paths=" << candidates.size() << " readable=" << readable
        << " missing=" << missingCount << " invalid=" << invalid.size() << " portraits=" << portraitPaths
        << " list=" << outputList.string() << " missing_tsv=" << missingPath.string()
        << " fields_tsv=" << fieldPath.string() << '\n';
    constexpr std::array types{ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
    for (const auto type : types) {
        const auto count = static_cast<std::size_t>(std::count_if(specs.begin(), specs.end(), [type](const auto& spec) {
            return spec.type == type;
        }));
        std::cout << "MODEL_LIST_TYPE type=" << War3Map::ObjectData::ToString(type)
            << " objects=" << objects[static_cast<std::size_t>(type)] << " model_fields=" << count << '\n';
    }
    return 0;
}
