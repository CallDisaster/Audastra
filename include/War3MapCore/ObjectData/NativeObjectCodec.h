#pragma once

#include "War3MapCore/Codec.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::ObjectData {

enum class NativeObjectTable : std::uint8_t {
    Original,
    Custom,
};

enum class NativeValueType : std::int32_t {
    Int32 = 0,
    Real = 1,
    Unreal = 2,
    String = 3,
};

// 各操作单独授权；既有值可改不代表可以新增对象或接受模板整类替换。
enum class NativeObjectOperation : std::uint8_t {
    PreserveSource,
    EditExistingValue,
    EditStructure,
    ReplaceFromTemplate,
};

class NativeObjectDocument final : public CodecDocument {
public:
    // 显式创建经典 v2 空表；未知版本不会被猜测为可写格式。
    [[nodiscard]] static Result<NativeObjectDocument> CreateEmpty(
        ObjectType type, std::int32_t version = 2);
    [[nodiscard]] std::string FormatName() const override;
    [[nodiscard]] ObjectType Type() const noexcept;
    [[nodiscard]] std::int32_t Version() const noexcept;
    [[nodiscard]] std::span<const ObjectRecord> Objects() const noexcept;
    [[nodiscard]] const ObjectRecord* FindObject(const ObjectKey& key) const noexcept;
    // v3 在对象 ID 与字段数量之间的整数段尚无可靠语义；只暴露原值和顺序。
    [[nodiscard]] std::optional<std::span<const std::int32_t>> Version3UnknownValues(
        const ObjectKey& object) const noexcept;
    // 只读取得实际二进制存储类型；缺对象/字段、不安全或歧义文档返回空，绝不从 Value 猜 Real/Unreal。
    [[nodiscard]] std::optional<NativeValueType> ModificationValueType(
        const ObjectKey& object, const ModificationKey& modification) const noexcept;
    // 只读计算经典 v2 的实际 wire 大小，不序列化、不复制字段，也不从元数据猜存储类型。
    // 不安全、非 v2 或内部存储不一致时返回空；对象大小包含十二字节对象头。
    [[nodiscard]] std::optional<std::size_t> SerializedByteSize() const noexcept;
    [[nodiscard]] std::optional<std::size_t> ObjectSerializedByteSize(
        const ObjectKey& object) const noexcept;
    [[nodiscard]] bool IsStructurallySafe() const noexcept;
    [[nodiscard]] bool Supports(NativeObjectOperation operation) const noexcept;
    // 兼容旧调用者：只表示完整结构编辑能力，不包含仅允许既有值修改的v1档位。
    [[nodiscard]] bool IsEditable() const noexcept;
    [[nodiscard]] std::size_t OriginalObjectCount() const noexcept;
    [[nodiscard]] std::size_t CustomObjectCount() const noexcept;

    // 修改既有字段的值（类型必须保持不变）。
    [[nodiscard]] Status SetModification(
        const ObjectKey& object,
        const ModificationKey& modification,
        Value value);

    // 向已有对象追加新的 modification 字段。
    [[nodiscard]] Status AddModification(
        const ObjectKey& object,
        const ModificationKey& modification,
        NativeValueType valueType,
        Value value);

    // 移除一条精确身份的覆盖，恢复原版/父对象的继承值。
    [[nodiscard]] Status RemoveModification(
        const ObjectKey& object, const ModificationKey& modification);

    // 创建新的自定义对象（继承自 parentRawcode）。
    [[nodiscard]] Status AddCustomObject(
        const std::string& rawcode,
        const std::string& parentRawcode);

    // 在 Original 表添加原版对象的空覆盖，不创建自定义对象或改变游戏目录。
    [[nodiscard]] Status AddOriginalObject(const std::string& rawcode);

    // 复制 Custom 表中的对象及全部字段，保留父对象、原始值类型、等级/数据/变体。
    // 源字节中可安全透传的字段不经展示层重编码。
    [[nodiscard]] Status CloneCustomObject(
        const ObjectKey& source,
        const std::string& newRawcode);

    // 仅移除该 Original 表记录及其字段覆盖，恢复全部标准值；不删除游戏中的原版对象。
    [[nodiscard]] Status RemoveOriginalObjectOverrides(const ObjectKey& object);

    // 删除自定义对象及其全部 modification。仅允许删除 Custom 表中的对象。
    [[nodiscard]] Status DeleteCustomObject(const ObjectKey& object);

private:
    friend class NativeObjectCodec;

    struct ModificationStorage {
        std::array<std::byte, 4> fieldId{};
        NativeValueType valueType = NativeValueType::Int32;
        std::int32_t levelOrVariation = 0;
        std::int32_t dataIndex = 0;
        std::array<std::byte, 4> endMarker{};
        std::size_t sourceOffset = 0;
        std::size_t sourceLength = 0;
        std::size_t valueOffset = 0;
        std::size_t valueLength = 0;
        std::size_t publicModificationIndex = 0;
        Value value = std::int32_t{0};
        bool changed = false;
        bool isNew = false;  // 新增的 modification，无源字节
    };

    struct ObjectStorage {
        NativeObjectTable table = NativeObjectTable::Original;
        std::array<std::byte, 4> originalId{};
        std::array<std::byte, 4> newId{};
        std::size_t publicObjectIndex = 0;
        std::vector<std::int32_t> version3UnknownValues;
        std::vector<ModificationStorage> modifications;
        bool deleted = false;  // 标记删除的自定义对象
    };

    [[nodiscard]] Status RequireEditable(std::string_view operation) const;
    [[nodiscard]] Status RemoveObjectRecord(const ObjectKey& object, NativeObjectTable table);
    [[nodiscard]] std::optional<std::size_t> StoredObjectSerializedByteSize(
        const ObjectStorage& object) const noexcept;

    ObjectType type_ = ObjectType::Unit;
    std::int32_t version_ = 0;
    bool structurallySafe_ = false;
    bool editable_ = false;
    bool edited_ = false;
    std::size_t originalObjectCount_ = 0;
    std::size_t customObjectCount_ = 0;
    std::string sourceSha256_;
    std::vector<std::byte> sourceBytes_;
    std::vector<ObjectRecord> objects_;
    std::vector<ObjectStorage> storage_;
    std::vector<Diagnostic> parseDiagnostics_;
};

class NativeObjectCodec final : public IFormatCodec {
public:
    explicit NativeObjectCodec(ObjectType type) noexcept;

    [[nodiscard]] ObjectType Type() const noexcept;
    [[nodiscard]] std::string Name() const override;
    [[nodiscard]] CodecParseResult Parse(
        std::span<const std::byte> input) const override;
    [[nodiscard]] Status Validate(const CodecDocument& document) const override;
    [[nodiscard]] Result<std::vector<std::byte>> Serialize(
        const CodecDocument& document) const override;
    [[nodiscard]] SemanticDiff Diff(
        const CodecDocument& before,
        const CodecDocument& after) const override;
    [[nodiscard]] CodecInspection Inspect(
        const CodecDocument& document) const override;
    [[nodiscard]] Result<War3Map::ObjectPatch> BuildPatch(
        const NativeObjectDocument& document,
        std::filesystem::path relativePath,
        std::string summary) const;

private:
    ObjectType type_;
};

} // namespace War3Map::ObjectData
