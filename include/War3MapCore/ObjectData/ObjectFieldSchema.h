#pragma once

#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <functional>
#include <unordered_map>

namespace War3Map::Game {
class GameDataSource;
class WorldEditStrings;
}

namespace War3Map::ObjectData {

struct ObjectFieldChoice {
    std::string value;
    std::string displayName;
    std::vector<std::string> aliases;
};

struct ObjectFieldDefinition {
    ObjectType objectType = ObjectType::Unit;
    std::string fieldId;       // 原生四字节 ID，严格区分大小写，例如 Hbz1。
    std::string sourceField;   // metadata 的 field，保留大小写，例如 HP、Data。
    std::string sourceTable;   // UnitBalance、AbilityData、Profile 等。
    std::string displayName;
    std::string displayNameToken;
    std::string category;
    std::string categoryDisplayName;
    std::string valueType;     // metadata 原始 type；未知类型保留，不猜测可编辑性。
    std::optional<NativeValueType> nativeValueType;
    std::int32_t repeat = 0;
    std::int32_t data = 0;
    std::int32_t index = -1;
    std::int32_t appendIndex = 0;
    std::int32_t effectIndex = 0;
    std::string effectType;
    std::vector<std::string> useSpecific;
    std::vector<std::string> notSpecific;
    bool useUnit = true;
    bool useHero = true;
    bool useBuilding = true;
    bool useItem = true;
    bool useCreep = true;
    bool canBeEmpty = false;
    bool isList = false;
    std::vector<ObjectType> referenceTypes;
    std::vector<ObjectFieldChoice> choices;
    std::string minimum;
    std::string maximum;
    std::string sort;
    bool displayNameResolved = false;
    std::unordered_map<std::string, std::string> metadata;
};

// rawcode 为当前对象或继承的原版对象；baseCode 为其 SLK code。
// 未提供对象身份时，useSpecific 的 Data 字段不允许猜测。
struct ObjectFieldContext {
    std::string rawcode;
    std::string baseCode;
    std::optional<bool> hero;
    std::optional<bool> building;
    std::optional<bool> item;
    std::optional<bool> creep;
};

struct ObjectFieldMatch {
    const ObjectFieldDefinition* definition = nullptr;
    ModificationKey modification;
    std::string sourceKey;     // 小写 SLK/profile 键，重复 SLK 字段含等级/变体后缀。
    std::optional<std::uint32_t> sourceIndex; // profile CSV 的零基分量/等级索引。
    bool safeToEdit = false;
    std::string reason;
};

struct ObjectFieldSchemaSource {
    ObjectType type = ObjectType::Unit;
    std::string metadataFile;
};

struct ObjectFieldSchemaOptions {
    std::vector<ObjectFieldSchemaSource> sources;
    std::vector<std::string> stringFiles{"UI/WorldEditStrings.txt", "UI/WorldEditGameStrings.txt"};
    // 统一字符串表（YDWE 覆盖层 → 游戏 → 英文原版）；提供时优先于 stringFiles 解析字段名与选项名。
    const Game::WorldEditStrings* strings = nullptr;
    std::string worldEditDataFile = "UI/WorldEditData.txt";
    std::string unitEditorDataFile = "UI/UnitEditorData.txt";
    std::size_t maximumFileBytes = 32 * 1024 * 1024;
};

class ObjectFieldSchema {
public:
    using FileReader = std::function<Result<std::vector<std::byte>>(const std::string&)>;
    [[nodiscard]] static Result<ObjectFieldSchema> Load(
        const Game::GameDataSource& source, const ObjectFieldSchemaOptions& options = {});
    [[nodiscard]] static Result<ObjectFieldSchema> Load(
        const FileReader& reader, const ObjectFieldSchemaOptions& options = {});
    [[nodiscard]] static std::vector<ObjectFieldSchemaSource> DefaultSources();
    [[nodiscard]] std::span<const ObjectFieldDefinition> Definitions() const noexcept { return definitions_; }
    [[nodiscard]] const ObjectFieldDefinition* FindNative(ObjectType type, std::string_view fieldId) const;
    [[nodiscard]] bool AppliesTo(const ObjectFieldDefinition& definition, const ObjectFieldContext& context) const;

    // 保留歧义：没有唯一、可确定地址的匹配时 definition=nullptr 或 safeToEdit=false。
    // sourceIndex 用于 Buttonpos 等分量以及 profile 的等级序列；调用方负责 CSV 保真写回。
    [[nodiscard]] ObjectFieldMatch MatchSourceField(ObjectType type, std::string_view sourceKey,
        const ObjectFieldContext& context = {}, std::optional<std::uint32_t> sourceIndex = {}) const;
    [[nodiscard]] ObjectFieldMatch MatchNativeField(ObjectType type, const ModificationKey& key,
        const ObjectFieldContext& context = {}) const;

private:
    std::vector<ObjectFieldDefinition> definitions_;
    std::unordered_map<std::string, std::size_t> nativeIndex_;
};

} // namespace War3Map::ObjectData
