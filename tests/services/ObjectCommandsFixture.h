#pragma once

#include "War3MapCore/Services/ObjectCommands.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <stdexcept>

namespace War3Map::Tests {
using ObjectRow = std::map<std::string, std::string>;
inline std::vector<std::byte> ObjectBytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char value : text) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}
inline std::string ObjectTable(const std::vector<ObjectRow>& rows) {
    std::vector<std::string> columns{"ID"};
    for (const auto& row : rows) for (const auto& [key, value] : row) {
        (void)value;
        if (std::find(columns.begin(), columns.end(), key) == columns.end()) columns.push_back(key);
    }
    const auto quote = [](std::string_view value) {
        std::string result = "\"";
        for (const char ch : value) { result += ch; if (ch == '"') result += ch; }
        return result + '"';
    };
    std::string result = "ID;PWXL\n";
    for (std::size_t x = 0; x < columns.size(); ++x)
        result += "C;X" + std::to_string(x + 1) + ";Y1;K" + quote(columns[x]) + '\n';
    for (std::size_t y = 0; y < rows.size(); ++y) for (std::size_t x = 0; x < columns.size(); ++x) {
        const auto value = rows[y].find(columns[x]);
        if (value != rows[y].end()) result += "C;X" + std::to_string(x + 1) + ";Y" + std::to_string(y + 2) + ";K" + quote(value->second) + '\n';
    }
    return result + "E\n";
}

// 原创七类目录和元数据，不依赖或打包任何游戏资源。
struct ObjectCommandsFixture {
    struct Kind { ObjectData::ObjectType type; const char* base; const char* custom; const char* nameField; };
    inline static constexpr std::array kinds{
        Kind{ObjectData::ObjectType::Unit, "u001", "U001", "unam"},
        Kind{ObjectData::ObjectType::Ability, "A001", "A101", "anam"},
        Kind{ObjectData::ObjectType::Item, "i001", "I001", "inam"},
        Kind{ObjectData::ObjectType::Buff, "B001", "B101", "bnam"},
        Kind{ObjectData::ObjectType::Upgrade, "R001", "R101", "gnam"},
        Kind{ObjectData::ObjectType::Doodad, "D001", "D101", "dnam"},
        Kind{ObjectData::ObjectType::Destructable, "b001", "b101", "fnam"},
    };
    ObjectData::StandardObjectCatalog standard;
    ObjectData::ObjectFieldSchema schema;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, 7> documents;

    ObjectCommandsFixture() {
        using namespace ObjectData;
        std::map<std::string, std::string> files;
        CatalogLoadOptions catalogOptions; catalogOptions.stringFiles.clear();
        ObjectFieldSchemaOptions schemaOptions; schemaOptions.stringFiles.clear();
        schemaOptions.worldEditDataFile = "world.txt"; schemaOptions.unitEditorDataFile = "editor.txt";
        files["world.txt"] = "[ObjectEditorCategories]\nstats=状态\n";
        files["editor.txt"] = "[unitRace]\n00=human,人类\n01=orc,兽族\nNumValues=2\n";
        for (const auto& kind : kinds) {
            const std::string prefix(ToString(kind.type));
            std::vector<ObjectRow> data{{{"ID", kind.base}, {"Name", "原始对象"}, {"HP", "100"},
                {"scale", "1"}, {"race", "human"}, {"special", "0"}, {"abilList", "A001"},
                {"code", kind.base}, {"DataA1", "10"}, {"DataA2", "20"}}};
            std::vector<ObjectRow> metadata{{{"ID", kind.nameField}, {"field", "Name"}, {"slk", "Profile"},
                {"index", "0"}, {"type", "string"}, {"displayName", "名称"}}};
            if (kind.type == ObjectType::Unit) {
                metadata.push_back({{"ID", "uhpm"}, {"field", "HP"}, {"slk", "UnitBalance"}, {"type", "int"},
                    {"displayName", "生命上限"}, {"minVal", "1"}, {"maxVal", "1000"}});
                metadata.push_back({{"ID", "ussc"}, {"field", "scale"}, {"slk", "UnitUI"}, {"type", "real"}, {"minVal", "0.1"}, {"maxVal", "10"}});
                metadata.push_back({{"ID", "urac"}, {"field", "race"}, {"slk", "UnitData"}, {"type", "unitRace"}});
                metadata.push_back({{"ID", "uspe"}, {"field", "special"}, {"slk", "UnitData"}, {"type", "bool"}});
                metadata.push_back({{"ID", "uabi"}, {"field", "abilList"}, {"slk", "UnitAbilities"}, {"type", "abilityList"}, {"canBeEmpty", "1"}});
            }
            if (kind.type == ObjectType::Ability) metadata.push_back({{"ID", "adat"}, {"field", "Data"},
                {"slk", "AbilityData"}, {"type", "int"}, {"repeat", "4"}, {"data", "1"}, {"useSpecific", "A001"}});
            files[prefix + ".slk"] = ObjectTable(data);
            files[prefix + ".meta"] = ObjectTable(metadata);
            files[prefix + ".txt"] = "[" + std::string(kind.base) + "]\nName=\"原始对象\"\n";
            catalogOptions.sources.push_back({kind.type, {prefix + ".slk"}, {prefix + ".txt"}});
            schemaOptions.sources.push_back({kind.type, prefix + ".meta"});
            auto parsed = LniObjectCodec(kind.type).Parse({});
            const auto* document = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
            if (!document) throw std::runtime_error("fixture empty LNI");
            documents[static_cast<std::size_t>(kind.type)] = std::make_unique<LniObjectDocument>(*document);
        }
        const StandardObjectCatalog::FileReader reader = [&files](const std::string& name) {
            const auto found = files.find(name);
            return found == files.end() ? Result<std::vector<std::byte>>::Failure("Test", "missing", name) :
                Result<std::vector<std::byte>>::Success(ObjectBytes(found->second));
        };
        auto loadedStandard = StandardObjectCatalog::Load(reader, catalogOptions);
        auto loadedSchema = ObjectFieldSchema::Load(reader, schemaOptions);
        if (!loadedStandard.Succeeded() || !loadedSchema.Succeeded()) throw std::runtime_error("fixture catalog/schema");
        standard = std::move(*loadedStandard.value); schema = std::move(*loadedSchema.value);
    }
    Services::ObjectCommandContext Context(ObjectData::ObjectType type) const {
        Services::ObjectCommandContext context;
        context.type = type; context.standard = &standard; context.schema = &schema;
        for (std::size_t i = 0; i < documents.size(); ++i) context.documents[i] = documents[i].get();
        context.document = context.documents[static_cast<std::size_t>(type)];
        return context;
    }
};
} // namespace War3Map::Tests
