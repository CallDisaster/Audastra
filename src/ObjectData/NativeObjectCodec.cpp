#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include "War3MapCore/Hash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace War3Map::ObjectData {
namespace {

constexpr std::string_view kSubsystem = "ObjectData.Native";
constexpr std::int32_t kSupportedVersion = 2;
constexpr std::int32_t kClassicVersion1 = 1;
constexpr std::int32_t kReadOnlyVersion3 = 3;
constexpr std::size_t kMaximumInputBytes = 1024ull * 1024ull * 1024ull;
constexpr std::int32_t kMaximumObjectCount = 1'000'000;
constexpr std::int32_t kMaximumModificationsPerObject = 1'000'000;
constexpr std::size_t kMaximumTotalModifications = 10'000'000;
constexpr std::int32_t kMaximumVersion3IntegersPerObject = 100'000;
constexpr std::size_t kMaximumTotalVersion3Integers = 1'000'000;
constexpr std::size_t kMaximumStringBytes = 64ull * 1024ull * 1024ull;

class BinaryReader {
public:
    explicit BinaryReader(std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {
    }

    [[nodiscard]] std::size_t Position() const noexcept {
        return position_;
    }

    [[nodiscard]] std::size_t Remaining() const noexcept {
        return bytes_.size() - position_;
    }

    [[nodiscard]] bool ReadBytes(std::span<std::byte> output) noexcept {
        if (output.size() > Remaining()) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), output.size(), output.begin());
        position_ += output.size();
        return true;
    }

    [[nodiscard]] bool ReadInt32(std::int32_t& output) noexcept {
        if (Remaining() < sizeof(std::uint32_t)) {
            return false;
        }
        const auto* source = bytes_.data() + position_;
        const std::uint32_t bits =
            std::to_integer<std::uint32_t>(source[0])
            | (std::to_integer<std::uint32_t>(source[1]) << 8u)
            | (std::to_integer<std::uint32_t>(source[2]) << 16u)
            | (std::to_integer<std::uint32_t>(source[3]) << 24u);
        output = std::bit_cast<std::int32_t>(bits);
        position_ += sizeof(std::uint32_t);
        return true;
    }

    [[nodiscard]] bool ReadFloat(float& output) noexcept {
        std::int32_t signedBits = 0;
        if (!ReadInt32(signedBits)) {
            return false;
        }
        output = std::bit_cast<float>(std::bit_cast<std::uint32_t>(signedBits));
        return true;
    }

    [[nodiscard]] bool ReadCString(std::string& output, std::size_t& encodedLength) noexcept {
        const std::size_t start = position_;
        const std::size_t limit = std::min(bytes_.size(), start + kMaximumStringBytes + 1);
        while (position_ < limit && bytes_[position_] != std::byte{0}) {
            ++position_;
        }
        if (position_ >= bytes_.size() || position_ >= limit) {
            position_ = start;
            return false;
        }
        output.assign(
            reinterpret_cast<const char*>(bytes_.data() + start),
            position_ - start);
        ++position_;
        encodedLength = position_ - start;
        return true;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

[[nodiscard]] Diagnostic MakeDiagnostic(
    DiagnosticSeverity severity,
    std::string code,
    std::string message,
    std::uint64_t offset = 0,
    std::uint64_t length = 0) {
    Diagnostic diagnostic{
        severity,
        std::string(kSubsystem),
        std::move(code),
        std::move(message),
        std::nullopt,
    };
    if (length != 0) {
        diagnostic.origin = OriginSpan{{}, offset, length, {}};
    }
    return diagnostic;
}

[[nodiscard]] bool HasLevelData(ObjectType type) noexcept {
    return type == ObjectType::Ability
        || type == ObjectType::Upgrade
        || type == ObjectType::Doodad;
}

[[nodiscard]] bool SupportsClassicV1ValueEdits(ObjectType type) noexcept {
    return type == ObjectType::Unit || type == ObjectType::Ability || type == ObjectType::Item
        || type == ObjectType::Upgrade || type == ObjectType::Doodad
        || type == ObjectType::Destructable;
}

[[nodiscard]] bool IsZeroId(const std::array<std::byte, 4>& id) noexcept {
    return std::all_of(id.begin(), id.end(), [](std::byte value) {
        return value == std::byte{0};
    });
}

[[nodiscard]] bool IsValidObjectId(const std::array<std::byte, 4>& id) noexcept {
    return std::none_of(id.begin(), id.end(), [](std::byte value) {
        return value == std::byte{0};
    });
}

[[nodiscard]] std::string IdToString(const std::array<std::byte, 4>& id) {
    // 4 字节 ID 常为短名的 NUL 填充（如 "Crs\0"）：裁掉尾部 NUL，
    // 否则写入 LNI 文本会产生 NUL 字节并破坏行框定。
    std::size_t length = id.size();
    while (length > 0 && id[length - 1] == std::byte{0}) {
        --length;
    }
    return std::string(reinterpret_cast<const char*>(id.data()), length);
}

[[nodiscard]] std::string IdForDiagnostic(const std::array<std::byte, 4>& id) {
    std::ostringstream stream;
    stream << '"';
    for (const std::byte value : id) {
        const unsigned int byte = std::to_integer<unsigned int>(value);
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\' && byte != '"') {
            stream << static_cast<char>(byte);
        } else {
            constexpr char digits[] = "0123456789ABCDEF";
            stream << "\\x" << digits[(byte >> 4u) & 0xfu] << digits[byte & 0xfu];
        }
    }
    stream << '"';
    return stream.str();
}

[[nodiscard]] bool IsKnownEndMarker(
    const std::array<std::byte, 4>& marker,
    const std::array<std::byte, 4>& originalId,
    const std::array<std::byte, 4>& newId) noexcept {
    return IsZeroId(marker) || marker == originalId || (!IsZeroId(newId) && marker == newId);
}

[[nodiscard]] std::string ModificationIdentity(const ModificationKey& key) {
    std::string result = key.fieldId;
    result.push_back('\0');
    if (key.level.has_value()) {
        result += std::to_string(*key.level);
    }
    result.push_back('\0');
    if (key.dataIndex.has_value()) {
        result += std::to_string(*key.dataIndex);
    }
    result.push_back('\0');
    if (key.variation.has_value()) {
        result += std::to_string(*key.variation);
    }
    return result;
}

[[nodiscard]] ModificationKey MakeModificationKey(
    ObjectType type,
    const std::array<std::byte, 4>& fieldId,
    std::int32_t levelOrVariation,
    std::int32_t dataIndex,
    bool hasLevelDataInWire) {
    ModificationKey key;
    key.fieldId = IdToString(fieldId);
    if (hasLevelDataInWire) {
        key.dataIndex = static_cast<std::uint32_t>(dataIndex);
        if (type == ObjectType::Doodad) {
            key.variation = static_cast<std::uint32_t>(levelOrVariation);
        } else {
            key.level = static_cast<std::uint32_t>(levelOrVariation);
        }
    }
    return key;
}

[[nodiscard]] std::optional<std::string> StringValue(const Value& value) {
    return std::visit(
        [](const auto& current) -> std::optional<std::string> {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return current;
            } else if constexpr (
                std::is_same_v<T, RawcodeValue>
                || std::is_same_v<T, EnumValue>
                || std::is_same_v<T, AssetPathValue>
                || std::is_same_v<T, LocalizedStringValue>) {
                return current.value;
            } else {
                return std::nullopt;
            }
        },
        value);
}

[[nodiscard]] Result<Value> NormalizeValue(NativeValueType type, Value value) {
    switch (type) {
    case NativeValueType::Int32:
        if (const auto* integer = std::get_if<std::int32_t>(&value)) {
            return Result<Value>::Success(Value{*integer});
        }
        break;
    case NativeValueType::Real:
    case NativeValueType::Unreal:
        if (const auto* floating = std::get_if<float>(&value); floating != nullptr && std::isfinite(*floating)) {
            return Result<Value>::Success(Value{*floating});
        }
        // 整数值形式的浮点字段（如 "325"）：LNI 文本往返后变体为 int32，安全提升。
        if (const auto* integer = std::get_if<std::int32_t>(&value)) {
            return Result<Value>::Success(Value{static_cast<float>(*integer)});
        }
        break;
    case NativeValueType::String:
        if (auto text = StringValue(value); text.has_value()
            && text->find('\0') == std::string::npos
            && text->size() <= kMaximumStringBytes) {
            return Result<Value>::Success(Value{std::move(*text)});
        }
        break;
    }
    return Result<Value>::Failure(
        std::string(kSubsystem),
        "native-value-type-mismatch",
        "修改值与 native 字段中记录的值类型不兼容，已拒绝写入。");
}

void AppendInt32(std::vector<std::byte>& output, std::int32_t value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    output.push_back(static_cast<std::byte>(bits & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 8u) & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 16u) & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 24u) & 0xffu));
}

void AppendFloat(std::vector<std::byte>& output, float value) {
    AppendInt32(output, std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value)));
}

void AppendValue(std::vector<std::byte>& output, NativeValueType type, const Value& value) {
    switch (type) {
    case NativeValueType::Int32:
        AppendInt32(output, std::get<std::int32_t>(value));
        break;
    case NativeValueType::Real:
    case NativeValueType::Unreal:
        AppendFloat(output, std::get<float>(value));
        break;
    case NativeValueType::String: {
        const auto& text = std::get<std::string>(value);
        output.insert(output.end(), reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()));
        output.push_back(std::byte{0});
        break;
    }
    }
}

template <std::size_t Size>
void AppendArray(std::vector<std::byte>& output, const std::array<std::byte, Size>& value) {
    output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] std::string ChangeSummary(const ObjectChange& change) {
    std::string result = "修改 native 对象 " + change.object.rawcode;
    if (change.modification.has_value()) {
        result += " 字段 " + change.modification->fieldId;
    }
    return result;
}

} // namespace

Result<NativeObjectDocument> NativeObjectDocument::CreateEmpty(ObjectType type, std::int32_t version) {
    if (version != kSupportedVersion) {
        return Result<NativeObjectDocument>::Failure(std::string(kSubsystem), "native-version-unsupported",
            "空文档只能显式创建为经典 native v2 格式。");
    }
    switch (type) {
    case ObjectType::Unit: case ObjectType::Ability: case ObjectType::Item: case ObjectType::Buff:
    case ObjectType::Upgrade: case ObjectType::Doodad: case ObjectType::Destructable: break;
    default:
        return Result<NativeObjectDocument>::Failure(std::string(kSubsystem), "native-object-type-invalid",
            "无法为未知对象类型创建 native 文档。");
    }
    NativeObjectDocument document;
    document.type_ = type;
    document.version_ = version;
    AppendInt32(document.sourceBytes_, version);
    AppendInt32(document.sourceBytes_, 0);
    AppendInt32(document.sourceBytes_, 0);
    auto hash = Sha256(document.sourceBytes_);
    if (!hash.Succeeded()) {
        Result<NativeObjectDocument> result;
        result.diagnostics = std::move(hash.diagnostics);
        return result;
    }
    document.sourceSha256_ = std::move(*hash.value);
    document.structurallySafe_ = true;
    document.editable_ = true;
    return Result<NativeObjectDocument>::Success(std::move(document));
}

std::string NativeObjectDocument::FormatName() const {
    return "Warcraft III Native Object Data (" + std::string(NativeFileName(type_)) + ')';
}

ObjectType NativeObjectDocument::Type() const noexcept {
    return type_;
}

std::int32_t NativeObjectDocument::Version() const noexcept {
    return version_;
}

std::span<const ObjectRecord> NativeObjectDocument::Objects() const noexcept {
    return objects_;
}

const ObjectRecord* NativeObjectDocument::FindObject(const ObjectKey& key) const noexcept {
    if (key.type != type_) {
        return nullptr;
    }
    const auto iterator = std::find_if(
        objects_.begin(),
        objects_.end(),
        [&key](const ObjectRecord& object) {
            return object.key == key;
        });
    return iterator == objects_.end() ? nullptr : &*iterator;
}

std::optional<std::span<const std::int32_t>> NativeObjectDocument::Version3UnknownValues(
    const ObjectKey& object) const noexcept {
    if (version_ != kReadOnlyVersion3 || !structurallySafe_) return std::nullopt;
    if (std::count_if(objects_.begin(), objects_.end(), [&object](const ObjectRecord& entry) {
        return entry.key == object;
    }) != 1) return std::nullopt;
    const auto record = FindObject(object);
    if (record == nullptr) return std::nullopt;
    const auto publicIndex = static_cast<std::size_t>(record - objects_.data());
    const auto stored = std::find_if(storage_.begin(), storage_.end(), [publicIndex](const ObjectStorage& entry) {
        return entry.publicObjectIndex == publicIndex;
    });
    if (stored == storage_.end()) return std::nullopt;
    return std::span<const std::int32_t>(stored->version3UnknownValues);
}

std::optional<NativeValueType> NativeObjectDocument::ModificationValueType(
    const ObjectKey& object, const ModificationKey& modification) const noexcept {
    if (!Supports(NativeObjectOperation::EditExistingValue)) return std::nullopt;
    const auto* record = FindObject(object);
    if (!record) return std::nullopt;
    const auto field = std::find_if(record->modifications.begin(), record->modifications.end(),
        [&modification](const ObjectModification& current) { return current.key == modification; });
    if (field == record->modifications.end()) return std::nullopt;
    const auto objectIndex = static_cast<std::size_t>(record - objects_.data());
    const auto fieldIndex = static_cast<std::size_t>(field - record->modifications.begin());
    for (const auto& stored : storage_) {
        if (stored.deleted || stored.publicObjectIndex != objectIndex) continue;
        for (const auto& storedField : stored.modifications) {
            if (storedField.publicModificationIndex == fieldIndex) return storedField.valueType;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> NativeObjectDocument::StoredObjectSerializedByteSize(
    const ObjectStorage& object) const noexcept {
    if (object.deleted || object.publicObjectIndex >= objects_.size()
        || !object.version3UnknownValues.empty()
        || object.modifications.size() != objects_[object.publicObjectIndex].modifications.size()) {
        return std::nullopt;
    }
    std::size_t bytes = 12; // originalId、newId、字段数量。
    const auto add = [&bytes](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - bytes) return false;
        bytes += amount;
        return true;
    };
    for (const auto& field : object.modifications) {
        if (field.publicModificationIndex >= object.modifications.size()) return std::nullopt;
        if (!field.changed && !field.isNew) {
            if (field.sourceOffset > sourceBytes_.size()
                || field.sourceLength > sourceBytes_.size() - field.sourceOffset
                || !add(field.sourceLength)) return std::nullopt;
            continue;
        }
        // 新字段和值/结束标记变更都按 private storage 的实际 tag 编码。
        if (!add(HasLevelData(type_) ? 20 : 12)) return std::nullopt;
        switch (field.valueType) {
        case NativeValueType::Int32:
            if (!std::holds_alternative<std::int32_t>(field.value) || !add(4)) return std::nullopt;
            break;
        case NativeValueType::Real:
        case NativeValueType::Unreal:
            if (!std::holds_alternative<float>(field.value) || !add(4)) return std::nullopt;
            break;
        case NativeValueType::String: {
            const auto* text = std::get_if<std::string>(&field.value);
            if (!text || !add(text->size()) || !add(1)) return std::nullopt;
            break;
        }
        default:
            return std::nullopt;
        }
    }
    return bytes;
}

std::optional<std::size_t> NativeObjectDocument::SerializedByteSize() const noexcept {
    if (!Supports(NativeObjectOperation::EditStructure)) return std::nullopt;
    std::size_t bytes = 12; // 版本和两张对象表的数量。
    std::size_t originals = 0, customs = 0;
    for (const auto& object : storage_) {
        if (object.deleted) continue;
        const auto size = StoredObjectSerializedByteSize(object);
        if (!size || *size > std::numeric_limits<std::size_t>::max() - bytes) return std::nullopt;
        bytes += *size;
        if (object.table == NativeObjectTable::Original) ++originals;
        else ++customs;
    }
    if (originals != originalObjectCount_ || customs != customObjectCount_
        || originals + customs != objects_.size()) return std::nullopt;
    return bytes;
}

std::optional<std::size_t> NativeObjectDocument::ObjectSerializedByteSize(
    const ObjectKey& object) const noexcept {
    if (!Supports(NativeObjectOperation::EditStructure)) return std::nullopt;
    const auto* record = FindObject(object);
    if (!record) return std::nullopt;
    const auto index = static_cast<std::size_t>(record - objects_.data());
    const auto stored = std::find_if(storage_.begin(), storage_.end(), [index](const ObjectStorage& item) {
        return !item.deleted && item.publicObjectIndex == index;
    });
    if (stored == storage_.end()) return std::nullopt;
    return StoredObjectSerializedByteSize(*stored);
}

bool NativeObjectDocument::IsStructurallySafe() const noexcept {
    return structurallySafe_;
}

bool NativeObjectDocument::IsEditable() const noexcept {
    return Supports(NativeObjectOperation::EditStructure);
}

bool NativeObjectDocument::Supports(NativeObjectOperation operation) const noexcept {
    if (!structurallySafe_) return false;
    const bool classicV1 = version_ == kClassicVersion1 && SupportsClassicV1ValueEdits(type_);
    switch (operation) {
    case NativeObjectOperation::PreserveSource:
        // v2 的歧义文档沿用既有原字节透传；v1 重复身份在解析时即拒绝。
        return version_ == kSupportedVersion || (classicV1 && editable_);
    case NativeObjectOperation::EditExistingValue:
        return editable_ && (version_ == kSupportedVersion || classicV1);
    case NativeObjectOperation::EditStructure:
    case NativeObjectOperation::ReplaceFromTemplate:
        return editable_ && version_ == kSupportedVersion;
    }
    return false;
}

std::size_t NativeObjectDocument::OriginalObjectCount() const noexcept {
    return originalObjectCount_;
}

std::size_t NativeObjectDocument::CustomObjectCount() const noexcept {
    return customObjectCount_;
}

Status NativeObjectDocument::RequireEditable(std::string_view operation) const {
    if (Supports(NativeObjectOperation::EditExistingValue) && version_ == kClassicVersion1) {
        return Status::Failure(std::string(kSubsystem), "native-v1-structure-unsupported",
            std::string(operation) + "涉及结构编辑；经典 native v1 仅允许修改已存在字段的值，原对象和字段结构必须保留。");
    }
    if (!structurallySafe_ || version_ != kSupportedVersion) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-document-unsafe",
            std::string(operation) + "要求完整解析且版本受支持的 native 文档。");
    }
    if (!editable_) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-document-read-only",
            std::string(operation) + "因对象或字段身份存在歧义而被拒绝；原始字节仍可只读查看。");
    }
    return Status::Ok();
}

Status NativeObjectDocument::SetModification(
    const ObjectKey& object,
    const ModificationKey& modification,
    Value value) {
    if (!Supports(NativeObjectOperation::EditExistingValue)) {
        return RequireEditable("修改已有 native 字段");
    }
    if (object.type != type_) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-type-mismatch",
            "对象类型与当前 native 文档不匹配。");
    }

    const auto publicObject = std::find_if(
        objects_.begin(),
        objects_.end(),
        [&object](const ObjectRecord& record) {
            return record.key == object;
        });
    if (publicObject == objects_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-not-found",
            "native 文档中不存在请求修改的对象。");
    }
    const std::size_t publicObjectIndex = static_cast<std::size_t>(publicObject - objects_.begin());
    const auto storedObject = std::find_if(
        storage_.begin(),
        storage_.end(),
        [publicObjectIndex](const ObjectStorage& current) {
            return current.publicObjectIndex == publicObjectIndex;
        });
    if (storedObject == storage_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-internal-object-missing",
            "native 对象索引不完整，已安全停止修改。");
    }

    const auto publicModification = std::find_if(
        publicObject->modifications.begin(),
        publicObject->modifications.end(),
        [&modification](const ObjectModification& current) {
            return current.key == modification;
        });
    if (publicModification == publicObject->modifications.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-modification-not-found",
            "基线 codec 不会在缺少字段元数据时新增 native modification。");
    }
    const std::size_t publicModificationIndex = static_cast<std::size_t>(
        publicModification - publicObject->modifications.begin());
    auto storedModification = std::find_if(
        storedObject->modifications.begin(),
        storedObject->modifications.end(),
        [publicModificationIndex](const ModificationStorage& current) {
            return current.publicModificationIndex == publicModificationIndex;
        });
    if (storedModification == storedObject->modifications.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-internal-modification-missing",
            "native 字段索引不完整，已安全停止修改。");
    }

    Result<Value> normalized = NormalizeValue(storedModification->valueType, std::move(value));
    if (!normalized.Succeeded()) {
        Status result;
        result.diagnostics = std::move(normalized.diagnostics);
        return result;
    }
    storedModification->value = *normalized.value;
    storedModification->changed = true;
    publicModification->value = std::move(*normalized.value);
    edited_ = true;
    return Status::Ok();
}

Status NativeObjectDocument::AddModification(
    const ObjectKey& object,
    const ModificationKey& requestedModification,
    NativeValueType valueType,
    Value value) {
    Status editable = RequireEditable("新增 native 字段");
    if (!editable.Succeeded()) {
        return editable;
    }
    if (object.type != type_) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-type-mismatch",
            "对象类型与当前 native 文档不匹配。");
    }
    if (requestedModification.fieldId.size() != 4 || requestedModification.fieldId.find('\0') != std::string::npos) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-field-id-invalid",
            "native modification 的字段 ID 必须恰好为 4 字节 rawcode。");
    }
    ModificationKey modification = requestedModification;
    const auto maximumIndex = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (modification.level.value_or(0) > maximumIndex || modification.dataIndex.value_or(0) > maximumIndex ||
        modification.variation.value_or(0) > maximumIndex ||
        (!HasLevelData(type_) && (modification.level.value_or(0) || modification.dataIndex.value_or(0) || modification.variation.value_or(0))) ||
        (type_ == ObjectType::Doodad && modification.level.value_or(0)) ||
        (HasLevelData(type_) && type_ != ObjectType::Doodad && modification.variation.value_or(0))) {
        return Status::Failure(std::string(kSubsystem), "native-modification-index-invalid",
            "字段等级、数据或变体索引无法由当前经典格式无损编码。");
    }
    // 与重解析后的键一致，避免缺省索引和显式零索引形成同一原生地址的重复覆盖。
    if (HasLevelData(type_)) {
        modification.dataIndex = modification.dataIndex.value_or(0);
        if (type_ == ObjectType::Doodad) { modification.level.reset(); modification.variation = modification.variation.value_or(0); }
        else { modification.variation.reset(); modification.level = modification.level.value_or(0); }
    } else {
        modification.level.reset(); modification.dataIndex.reset(); modification.variation.reset();
    }

    const auto publicObject = std::find_if(
        objects_.begin(), objects_.end(),
        [&object](const ObjectRecord& record) { return record.key == object; });
    if (publicObject == objects_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-not-found",
            "native 文档中不存在请求修改的对象。");
    }

    // 检查字段是否已存在
    const auto existing = std::find_if(
        publicObject->modifications.begin(), publicObject->modifications.end(),
        [&modification](const ObjectModification& current) { return current.key == modification; });
    if (existing != publicObject->modifications.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-modification-already-exists",
            "该字段已存在于对象中；若要修改值请使用 SetModification。");
    }

    Result<Value> normalized = NormalizeValue(valueType, std::move(value));
    if (!normalized.Succeeded()) {
        Status result;
        result.diagnostics = std::move(normalized.diagnostics);
        return result;
    }

    const std::size_t publicObjectIndex = static_cast<std::size_t>(publicObject - objects_.begin());
    auto storedObject = std::find_if(
        storage_.begin(), storage_.end(),
        [publicObjectIndex](const ObjectStorage& current) {
            return current.publicObjectIndex == publicObjectIndex;
        });
    if (storedObject == storage_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-internal-object-missing",
            "native 对象索引不完整，已安全停止修改。");
    }

    // 构建新的 ModificationStorage
    ModificationStorage newStorage;
    std::copy_n(
        reinterpret_cast<const std::byte*>(modification.fieldId.data()), 4,
        newStorage.fieldId.begin());
    newStorage.valueType = valueType;
    newStorage.levelOrVariation = 0;
    newStorage.dataIndex = 0;
    if (HasLevelData(type_)) {
        if (modification.level.has_value()) {
            newStorage.levelOrVariation = static_cast<std::int32_t>(*modification.level);
        } else if (modification.variation.has_value()) {
            newStorage.levelOrVariation = static_cast<std::int32_t>(*modification.variation);
        }
        if (modification.dataIndex.has_value()) {
            newStorage.dataIndex = static_cast<std::int32_t>(*modification.dataIndex);
        }
    }
    // endMarker 使用对象的 effective ID
    const std::array<std::byte, 4>& effectiveId =
        storedObject->table == NativeObjectTable::Original
            ? storedObject->originalId
            : storedObject->newId;
    newStorage.endMarker = effectiveId;
    newStorage.sourceOffset = 0;
    newStorage.sourceLength = 0;
    newStorage.publicModificationIndex = publicObject->modifications.size();
    newStorage.value = *normalized.value;
    newStorage.changed = true;
    newStorage.isNew = true;

    // 构建公开的 ObjectModification
    ObjectModification publicModification;
    publicModification.key = modification;
    publicModification.value = *normalized.value;
    publicModification.sourceKey = modification.fieldId;

    storedObject->modifications.push_back(std::move(newStorage));
    publicObject->modifications.push_back(std::move(publicModification));
    edited_ = true;
    return Status::Ok();
}

Status NativeObjectDocument::RemoveModification(
    const ObjectKey& object, const ModificationKey& modification) {
    const auto editable = RequireEditable("重置 native 字段");
    if (!editable.Succeeded()) return editable;
    const auto record = std::find_if(objects_.begin(), objects_.end(),
        [&object](const ObjectRecord& value) { return value.key == object; });
    if (object.type != type_ || record == objects_.end()) {
        return Status::Failure(std::string(kSubsystem), "native-object-not-found",
            "当前文档中不存在请求重置的对象。");
    }
    const auto field = std::find_if(record->modifications.begin(), record->modifications.end(),
        [&modification](const ObjectModification& value) { return value.key == modification; });
    if (field == record->modifications.end()) {
        return Status::Failure(std::string(kSubsystem), "native-modification-not-found",
            "当前对象没有此字段覆盖。");
    }
    const auto objectIndex = static_cast<std::size_t>(record - objects_.begin());
    const auto fieldIndex = static_cast<std::size_t>(field - record->modifications.begin());
    const auto stored = std::find_if(storage_.begin(), storage_.end(),
        [objectIndex](const ObjectStorage& value) { return !value.deleted && value.publicObjectIndex == objectIndex; });
    if (stored == storage_.end()) {
        return Status::Failure(std::string(kSubsystem), "native-internal-object-missing", "native 对象索引不完整。");
    }
    const auto storedField = std::find_if(stored->modifications.begin(), stored->modifications.end(),
        [fieldIndex](const ModificationStorage& value) { return value.publicModificationIndex == fieldIndex; });
    if (storedField == stored->modifications.end()) {
        return Status::Failure(std::string(kSubsystem), "native-internal-modification-missing", "native 字段索引不完整。");
    }
    stored->modifications.erase(storedField);
    record->modifications.erase(field);
    for (auto& remaining : stored->modifications) {
        if (remaining.publicModificationIndex > fieldIndex) --remaining.publicModificationIndex;
    }
    edited_ = true;
    return Status::Ok();
}

Status NativeObjectDocument::AddCustomObject(
    const std::string& rawcode,
    const std::string& parentRawcode) {
    Status editable = RequireEditable("新增自定义对象");
    if (!editable.Succeeded()) {
        return editable;
    }
    if (rawcode.size() != 4 || parentRawcode.size() != 4) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-rawcode-invalid",
            "rawcode 和 parentRawcode 必须恰好为 4 字节。");
    }
    if (rawcode.find('\0') != std::string::npos || parentRawcode.find('\0') != std::string::npos) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-rawcode-nul",
            "rawcode 不得包含 NUL 字节。");
    }
    if (rawcode == parentRawcode) {
        return Status::Failure(std::string(kSubsystem), "native-custom-self-parent",
            "自定义对象必须使用与父对象不同的 rawcode；修改原版对象应创建 Original 表覆盖。");
    }
    if (customObjectCount_ >= static_cast<std::size_t>(kMaximumObjectCount)) {
        return Status::Failure(std::string(kSubsystem), "native-object-count-limit", "自定义对象数量达到格式读取上限。");
    }

    // 检查是否已存在
    const ObjectKey newKey{type_, rawcode};
    if (FindObject(newKey) != nullptr) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-already-exists",
            "对象 rawcode 已存在于文档中。");
    }

    // 构建 ObjectStorage
    ObjectStorage newStorage;
    newStorage.table = NativeObjectTable::Custom;
    std::copy_n(
        reinterpret_cast<const std::byte*>(parentRawcode.data()), 4,
        newStorage.originalId.begin());
    std::copy_n(
        reinterpret_cast<const std::byte*>(rawcode.data()), 4,
        newStorage.newId.begin());
    newStorage.publicObjectIndex = objects_.size();
    newStorage.deleted = false;

    // 构建公开 ObjectRecord
    ObjectRecord publicObject;
    publicObject.key = newKey;
    publicObject.stableId = MakeObjectStableId(newKey);
    publicObject.parentRawcode = parentRawcode;

    objects_.push_back(std::move(publicObject));
    storage_.push_back(std::move(newStorage));
    ++customObjectCount_;
    edited_ = true;
    return Status::Ok();
}

Status NativeObjectDocument::AddOriginalObject(const std::string& rawcode) {
    const auto editable = RequireEditable("新增原版对象覆盖");
    if (!editable.Succeeded()) return editable;
    if (rawcode.size() != 4 || rawcode.find('\0') != std::string::npos) {
        return Status::Failure(std::string(kSubsystem), "native-rawcode-invalid",
            "原版对象 rawcode 必须恰好为 4 字节且不能包含 NUL。");
    }
    const ObjectKey key{type_, rawcode};
    if (FindObject(key)) {
        return Status::Failure(std::string(kSubsystem), "native-object-already-exists",
            "对象 rawcode 已存在于原版覆盖或自定义表中。");
    }
    if (originalObjectCount_ >= static_cast<std::size_t>(kMaximumObjectCount)) {
        return Status::Failure(std::string(kSubsystem), "native-object-count-limit", "原版覆盖对象数量达到格式读取上限。");
    }
    ObjectStorage stored;
    stored.table = NativeObjectTable::Original;
    std::copy_n(reinterpret_cast<const std::byte*>(rawcode.data()), 4, stored.originalId.begin());
    stored.publicObjectIndex = objects_.size();
    ObjectRecord record;
    record.key = key;
    record.stableId = MakeObjectStableId(key);
    objects_.push_back(std::move(record));
    storage_.push_back(std::move(stored));
    ++originalObjectCount_;
    edited_ = true;
    return Status::Ok();
}

Status NativeObjectDocument::CloneCustomObject(
    const ObjectKey& source, const std::string& newRawcode) {
    const Status editable = RequireEditable("复制自定义对象");
    if (!editable.Succeeded()) return editable;
    if (source.type != type_ || source.rawcode.size() != 4 || source.rawcode.find('\0') != std::string::npos) {
        return Status::Failure(std::string(kSubsystem), "native-clone-source-invalid",
            "复制来源的类型或 rawcode 无效。");
    }
    if (newRawcode.size() != 4 || newRawcode.find('\0') != std::string::npos) {
        return Status::Failure(std::string(kSubsystem), "native-clone-id-invalid",
            "新对象的 rawcode 必须恰好为四字节且不能包含 NUL。");
    }
    const ObjectRecord* original = FindObject(source);
    if (original == nullptr) {
        return Status::Failure(std::string(kSubsystem), "native-clone-source-missing",
            "当前文档中不存在复制来源。");
    }
    if (FindObject({type_, newRawcode}) != nullptr) {
        return Status::Failure(std::string(kSubsystem), "native-clone-id-conflict",
            "新 rawcode 已被当前文档占用。");
    }
    if (customObjectCount_ >= static_cast<std::size_t>(kMaximumObjectCount)) {
        return Status::Failure(std::string(kSubsystem), "native-object-count-limit",
            "自定义对象数量达到格式读取上限。");
    }
    const std::size_t sourceIndex = static_cast<std::size_t>(original - objects_.data());
    const auto stored = std::find_if(storage_.begin(), storage_.end(),
        [sourceIndex](const ObjectStorage& item) {
            return !item.deleted && item.publicObjectIndex == sourceIndex;
        });
    if (stored == storage_.end()) {
        return Status::Failure(std::string(kSubsystem), "native-clone-storage-missing",
            "复制来源的原始字段存储不存在，未修改文档。");
    }
    if (stored->table != NativeObjectTable::Custom || !original->parentRawcode.has_value()) {
        return Status::Failure(std::string(kSubsystem), "native-clone-original-override",
            "原版对象覆盖不能复制为自定义对象；请选择 Custom 对象。");
    }
    if (newRawcode == *original->parentRawcode) {
        return Status::Failure(std::string(kSubsystem), "native-clone-self-parent",
            "新 rawcode 不能与父对象相同。");
    }
    if (stored->modifications.size() != original->modifications.size()
        || stored->modifications.size() > static_cast<std::size_t>(kMaximumModificationsPerObject)) {
        return Status::Failure(std::string(kSubsystem), "native-clone-storage-mismatch",
            "复制来源的字段索引或数量不一致，未修改文档。");
    }
    std::size_t totalModifications = 0;
    for (const ObjectStorage& item : storage_) totalModifications += item.modifications.size();
    if (totalModifications > kMaximumTotalModifications
        || stored->modifications.size() > kMaximumTotalModifications - totalModifications) {
        return Status::Failure(std::string(kSubsystem), "native-clone-modification-limit",
            "复制后字段总数将超出格式读取上限。");
    }
    std::vector<bool> seen(original->modifications.size(), false);
    for (const ModificationStorage& field : stored->modifications) {
        if (field.publicModificationIndex >= seen.size() || seen[field.publicModificationIndex]) {
            return Status::Failure(std::string(kSubsystem), "native-clone-field-index-invalid",
                "复制来源存在重复或越界的字段索引，未修改文档。");
        }
        seen[field.publicModificationIndex] = true;
    }

    NativeObjectDocument candidate = *this;
    ObjectStorage copiedStorage = *stored;
    ObjectRecord copiedRecord = *original;
    const std::array<std::byte, 4> oldId = copiedStorage.newId;
    std::copy_n(reinterpret_cast<const std::byte*>(newRawcode.data()), 4, copiedStorage.newId.begin());
    copiedStorage.publicObjectIndex = candidate.objects_.size();
    for (ModificationStorage& field : copiedStorage.modifications) {
        if (field.endMarker == oldId) {
            field.endMarker = copiedStorage.newId;
            field.changed = true; // 只有引用来源 rawcode 的结束标记需要重编码。
        }
    }
    copiedRecord.key.rawcode = newRawcode;
    copiedRecord.stableId = MakeObjectStableId(copiedRecord.key);
    for (ObjectModification& field : copiedRecord.modifications) field.origin.reset();
    candidate.objects_.push_back(std::move(copiedRecord));
    candidate.storage_.push_back(std::move(copiedStorage));
    ++candidate.customObjectCount_;
    candidate.edited_ = true;

    const NativeObjectCodec codec(type_);
    const auto encoded = codec.Serialize(candidate);
    if (!encoded.Succeeded() || !encoded.value.has_value()) {
        return Status::Failure(std::string(kSubsystem), "native-clone-serialization-failed",
            "复制候选无法完整序列化，已拒绝写入文档。");
    }
    const CodecParseResult verified = codec.Parse(*encoded.value);
    const auto* parsed = dynamic_cast<const NativeObjectDocument*>(verified.document.get());
    const ObjectRecord* parsedClone = parsed == nullptr ? nullptr : parsed->FindObject({type_, newRawcode});
    if (!verified.Succeeded() || parsed == nullptr || !parsed->IsEditable()
        || parsedClone == nullptr || parsedClone->modifications.size() != original->modifications.size()
        || parsed->CustomObjectCount() != candidate.customObjectCount_) {
        return Status::Failure(std::string(kSubsystem), "native-clone-verification-failed",
            "复制候选无法完整重解析，已拒绝写入文档。");
    }
    *this = std::move(candidate);
    return Status::Ok();
}

Status NativeObjectDocument::DeleteCustomObject(const ObjectKey& object) {
    return RemoveObjectRecord(object, NativeObjectTable::Custom);
}

Status NativeObjectDocument::RemoveOriginalObjectOverrides(const ObjectKey& object) {
    return RemoveObjectRecord(object, NativeObjectTable::Original);
}

Status NativeObjectDocument::RemoveObjectRecord(const ObjectKey& object, NativeObjectTable table) {
    Status editable = RequireEditable(table == NativeObjectTable::Original ? "清除原版对象覆盖" : "删除自定义对象");
    if (!editable.Succeeded()) {
        return editable;
    }
    if (object.type != type_) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-type-mismatch",
            "对象类型与当前 native 文档不匹配。");
    }

    const auto publicObject = std::find_if(
        objects_.begin(), objects_.end(),
        [&object](const ObjectRecord& record) { return record.key == object; });
    if (publicObject == objects_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-not-found",
            "native 文档中不存在请求删除的对象。");
    }
    const std::size_t publicObjectIndex = static_cast<std::size_t>(publicObject - objects_.begin());
    auto storedObject = std::find_if(
        storage_.begin(), storage_.end(),
        [publicObjectIndex](const ObjectStorage& current) {
            return current.publicObjectIndex == publicObjectIndex;
        });
    if (storedObject == storage_.end()) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-internal-object-missing",
            "native 对象索引不完整，已安全停止删除。");
    }
    if (storedObject->table != table) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-cannot-delete-original",
            "请求移除的覆盖表与对象实际归属不一致，未修改游戏对象或其他表记录。");
    }
    if (storedObject->deleted) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-object-already-deleted",
            "该对象已被标记删除。");
    }

    storage_.erase(storedObject);
    objects_.erase(publicObject);
    for (auto& remaining : storage_) {
        if (remaining.publicObjectIndex > publicObjectIndex) --remaining.publicObjectIndex;
    }
    if (table == NativeObjectTable::Original) --originalObjectCount_;
    else --customObjectCount_;
    edited_ = true;
    return Status::Ok();
}

NativeObjectCodec::NativeObjectCodec(ObjectType type) noexcept
    : type_(type) {
}

ObjectType NativeObjectCodec::Type() const noexcept {
    return type_;
}

std::string NativeObjectCodec::Name() const {
    return "native-object-" + std::string(ToString(type_));
}

CodecParseResult NativeObjectCodec::Parse(std::span<const std::byte> input) const {
    CodecParseResult result;
    auto document = std::make_unique<NativeObjectDocument>();
    document->type_ = type_;

    const auto fail = [&document](
                          std::string code,
                          std::string message,
                          std::size_t offset = 0,
                          std::size_t length = 0) {
        document->structurallySafe_ = false;
        document->editable_ = false;
        document->parseDiagnostics_.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            std::move(code),
            std::move(message),
            offset,
            length));
    };
    const auto makeReadOnly = [&document](
                                  std::string code,
                                  std::string message,
                                  std::size_t offset = 0,
                                  std::size_t length = 0) {
        document->editable_ = false;
        document->parseDiagnostics_.push_back(MakeDiagnostic(
            DiagnosticSeverity::Warning,
            std::move(code),
            std::move(message),
            offset,
            length));
    };

    if (input.size() > kMaximumInputBytes) {
        fail("native-input-too-large", "native 对象文件超过 1 GiB 安全上限。");
        result.diagnostics = document->parseDiagnostics_;
        result.document = std::move(document);
        return result;
    }
    document->sourceBytes_.assign(input.begin(), input.end());

    auto sourceHash = Sha256(input);
    if (!sourceHash.Succeeded()) {
        fail("native-source-hash-failed", "无法计算 native 对象文件的 SHA-256 身份。");
        result.diagnostics = document->parseDiagnostics_;
        result.document = std::move(document);
        return result;
    }
    document->sourceSha256_ = std::move(*sourceHash.value);

    BinaryReader reader(input);
    if (!reader.ReadInt32(document->version_)) {
        fail("native-header-truncated", "native 对象文件缺少版本号。", 0, input.size());
        result.diagnostics = document->parseDiagnostics_;
        result.document = std::move(document);
        return result;
    }
    const bool classicV1 = document->version_ == kClassicVersion1
        && SupportsClassicV1ValueEdits(type_);
    const bool readOnlyV3 = document->version_ == kReadOnlyVersion3;
    if (document->version_ != kSupportedVersion && !classicV1 && !readOnlyV3) {
        fail(
            "native-version-unsupported",
            "不支持的 native 对象版本 " + std::to_string(document->version_)
                + "；文档保持只读且禁止序列化。",
            0,
            sizeof(std::int32_t));
        result.diagnostics = document->parseDiagnostics_;
        result.document = std::move(document);
        return result;
    }

    document->structurallySafe_ = true;
    document->editable_ = !readOnlyV3;
    if (classicV1) {
        document->parseDiagnostics_.push_back(MakeDiagnostic(DiagnosticSeverity::Warning,
            "native-v1-" + std::string(ToString(type_)) + "-read-only",
            "经典 native v1 保留来源结构；仅允许修改已存在字段的值并保源写出，新增、删除、重定父及模板整类替换仍被拒绝。",
            0, sizeof(std::int32_t)));
    }
    if (readOnlyV3) {
        makeReadOnly("native-v3-read-only",
            "native v3 对象的新增整数段只读显示并保留原成员；未知语义禁止编辑或序列化。",
            0, sizeof(std::int32_t));
    }
    // 战役原图中的 v1 Doodad 字段没有 v2 的 variation/dataIndex 两个整数。
    // v1 写出只替换已有值的源范围，不用 v2 结构重编码。
    const bool hasLevelDataInWire = HasLevelData(type_)
        && !(classicV1 && type_ == ObjectType::Doodad);
    std::unordered_set<std::string> objectIdentities;
    std::size_t totalModificationCount = 0;
    std::size_t totalVersion3IntegerCount = 0;

    const auto parseTable = [&](NativeObjectTable table) -> bool {
        const std::size_t countOffset = reader.Position();
        std::int32_t objectCount = 0;
        if (!reader.ReadInt32(objectCount)) {
            fail("native-object-count-truncated", "native 对象表缺少对象数量。", countOffset, reader.Remaining());
            return false;
        }
        if (objectCount < 0 || objectCount > kMaximumObjectCount) {
            fail("native-object-count-invalid", "native 对象数量超出安全范围。", countOffset, sizeof(std::int32_t));
            return false;
        }
        const std::size_t minimumObjectBytes = readOnlyV3 ? 16 : 12;
        if (static_cast<std::size_t>(objectCount) > reader.Remaining() / minimumObjectBytes) {
            fail(
                "native-object-count-exceeds-input",
                "native 对象数量超过剩余字节能够容纳的最小记录数。",
                countOffset,
                sizeof(std::int32_t));
            return false;
        }
        if (table == NativeObjectTable::Original) {
            document->originalObjectCount_ = static_cast<std::size_t>(objectCount);
        } else {
            document->customObjectCount_ = static_cast<std::size_t>(objectCount);
        }
        document->objects_.reserve(document->objects_.size() + static_cast<std::size_t>(objectCount));
        document->storage_.reserve(document->storage_.size() + static_cast<std::size_t>(objectCount));

        for (std::int32_t objectIndex = 0; objectIndex < objectCount; ++objectIndex) {
            const std::size_t objectOffset = reader.Position();
            NativeObjectDocument::ObjectStorage storedObject;
            storedObject.table = table;
            if (!reader.ReadBytes(storedObject.originalId)
                || !reader.ReadBytes(storedObject.newId)) {
                fail("native-object-id-truncated", "native 对象 ID 不完整。", objectOffset, reader.Remaining());
                return false;
            }
            if (!IsValidObjectId(storedObject.originalId)) {
                fail("native-original-id-invalid", "native 对象的原始 rawcode 包含 NUL。", objectOffset, 4);
                return false;
            }
            if (table == NativeObjectTable::Original && !IsZeroId(storedObject.newId)) {
                fail("native-original-table-new-id", "原始对象表出现非零 newId，无法证明表结构安全。", objectOffset + 4, 4);
                return false;
            }
            if (table == NativeObjectTable::Custom && !IsValidObjectId(storedObject.newId)) {
                fail("native-custom-table-new-id", "自定义对象表的 newId 无效。", objectOffset + 4, 4);
                return false;
            }

            if (readOnlyV3) {
                const std::size_t unknownCountOffset = reader.Position();
                std::int32_t unknownCount = 0;
                if (!reader.ReadInt32(unknownCount)) {
                    fail("native-v3-unknown-count-truncated", "native v3 对象缺少未知整数段数量。",
                        unknownCountOffset, reader.Remaining());
                    return false;
                }
                if (unknownCount < 0 || unknownCount > kMaximumVersion3IntegersPerObject
                    || totalVersion3IntegerCount + static_cast<std::size_t>(unknownCount)
                        > kMaximumTotalVersion3Integers) {
                    fail("native-v3-unknown-count-invalid", "native v3 未知整数段数量超出安全范围。",
                        unknownCountOffset, sizeof(std::int32_t));
                    return false;
                }
                if (static_cast<std::size_t>(unknownCount) > reader.Remaining() / sizeof(std::int32_t)) {
                    fail("native-v3-unknown-count-exceeds-input", "native v3 未知整数段超出成员边界。",
                        unknownCountOffset, sizeof(std::int32_t));
                    return false;
                }
                totalVersion3IntegerCount += static_cast<std::size_t>(unknownCount);
                storedObject.version3UnknownValues.reserve(static_cast<std::size_t>(unknownCount));
                for (std::int32_t unknownIndex = 0; unknownIndex < unknownCount; ++unknownIndex) {
                    std::int32_t value = 0;
                    if (!reader.ReadInt32(value)) {
                        fail("native-v3-unknown-value-truncated", "native v3 未知整数段不完整。",
                            reader.Position(), reader.Remaining());
                        return false;
                    }
                    storedObject.version3UnknownValues.push_back(value);
                }
            }

            std::int32_t modificationCount = 0;
            const std::size_t modificationCountOffset = reader.Position();
            if (!reader.ReadInt32(modificationCount)) {
                fail("native-modification-count-truncated", "native 对象缺少 modification 数量。", modificationCountOffset, reader.Remaining());
                return false;
            }
            if (modificationCount < 0 || modificationCount > kMaximumModificationsPerObject
                || totalModificationCount + static_cast<std::size_t>(modificationCount) > kMaximumTotalModifications) {
                fail("native-modification-count-invalid", "native modification 数量超出安全范围。", modificationCountOffset, sizeof(std::int32_t));
                return false;
            }
            const std::size_t minimumModificationBytes = hasLevelDataInWire ? 21 : 13;
            if (static_cast<std::size_t>(modificationCount)
                > reader.Remaining() / minimumModificationBytes) {
                fail(
                    "native-modification-count-exceeds-input",
                    "native modification 数量超过剩余字节能够容纳的最小记录数。",
                    modificationCountOffset,
                    sizeof(std::int32_t));
                return false;
            }
            totalModificationCount += static_cast<std::size_t>(modificationCount);

            const std::array<std::byte, 4>& effectiveId =
                table == NativeObjectTable::Original ? storedObject.originalId : storedObject.newId;
            ObjectRecord publicObject;
            publicObject.key = ObjectKey{type_, IdToString(effectiveId)};
            publicObject.stableId = MakeObjectStableId(publicObject.key);
            if (table == NativeObjectTable::Custom) {
                publicObject.parentRawcode = IdToString(storedObject.originalId);
            }
            publicObject.modifications.reserve(static_cast<std::size_t>(modificationCount));
            storedObject.modifications.reserve(static_cast<std::size_t>(modificationCount));
            storedObject.publicObjectIndex = document->objects_.size();

            const std::string objectIdentity = IdToString(effectiveId);
            if (!objectIdentities.insert(objectIdentity).second) {
                const auto prefix = "对象 rawcode " + IdForDiagnostic(effectiveId) + " 重复；";
                if (classicV1) fail("native-object-id-duplicate", prefix + "无法建立唯一查询身份。", objectOffset, 8);
                else makeReadOnly("native-object-id-duplicate", prefix + "文档可逐字节往返，但禁止歧义编辑。", objectOffset, 8);
            }
            std::unordered_set<std::string> modificationIdentities;

            for (std::int32_t modificationIndex = 0;
                 modificationIndex < modificationCount;
                 ++modificationIndex) {
                const std::size_t modificationOffset = reader.Position();
                NativeObjectDocument::ModificationStorage storedModification;
                if (!reader.ReadBytes(storedModification.fieldId)) {
                    fail("native-field-id-truncated", "native modification 的字段 ID 不完整。", modificationOffset, reader.Remaining());
                    return false;
                }
                std::int32_t rawValueType = 0;
                if (!reader.ReadInt32(rawValueType)) {
                    fail("native-value-type-truncated", "native modification 缺少值类型。", reader.Position(), reader.Remaining());
                    return false;
                }
                if (rawValueType < static_cast<std::int32_t>(NativeValueType::Int32)
                    || rawValueType > static_cast<std::int32_t>(NativeValueType::String)) {
                    fail(
                        "native-value-type-unsupported",
                        "native modification 使用未知值类型 " + std::to_string(rawValueType)
                            + "，无法可靠确定字段边界。",
                        modificationOffset + 4,
                        sizeof(std::int32_t));
                    return false;
                }
                storedModification.valueType = static_cast<NativeValueType>(rawValueType);

                if (hasLevelDataInWire) {
                    if (!reader.ReadInt32(storedModification.levelOrVariation)
                        || !reader.ReadInt32(storedModification.dataIndex)) {
                        fail("native-level-data-truncated", "native modification 的等级/数据索引不完整。", reader.Position(), reader.Remaining());
                        return false;
                    }
                    if (storedModification.levelOrVariation < 0 || storedModification.dataIndex < 0) {
                        fail("native-level-data-invalid", "native modification 的等级/变化或数据索引为负数。", modificationOffset + 8, 8);
                        return false;
                    }
                }

                storedModification.valueOffset = reader.Position();
                switch (storedModification.valueType) {
                case NativeValueType::Int32: {
                    std::int32_t value = 0;
                    if (!reader.ReadInt32(value)) {
                        fail("native-int-value-truncated", "native Int32 字段不完整。", storedModification.valueOffset, reader.Remaining());
                        return false;
                    }
                    storedModification.value = value;
                    storedModification.valueLength = sizeof(std::int32_t);
                    break;
                }
                case NativeValueType::Real:
                case NativeValueType::Unreal: {
                    float value = 0.0f;
                    if (!reader.ReadFloat(value)) {
                        fail("native-float-value-truncated", "native Float32 字段不完整。", storedModification.valueOffset, reader.Remaining());
                        return false;
                    }
                    storedModification.value = value;
                    storedModification.valueLength = sizeof(float);
                    break;
                }
                case NativeValueType::String: {
                    std::string value;
                    std::size_t encodedLength = 0;
                    if (!reader.ReadCString(value, encodedLength)) {
                        fail(
                            "native-string-invalid",
                            "native 字符串缺少 NUL 终止符或超过 64 MiB 安全上限。",
                            storedModification.valueOffset,
                            reader.Remaining());
                        return false;
                    }
                    storedModification.value = std::move(value);
                    storedModification.valueLength = encodedLength;
                    break;
                }
                }

                if (!reader.ReadBytes(storedModification.endMarker)) {
                    fail("native-end-marker-truncated", "native modification 缺少 4 字节结束标记。", reader.Position(), reader.Remaining());
                    return false;
                }
                if (!IsKnownEndMarker(
                        storedModification.endMarker,
                        storedObject.originalId,
                        storedObject.newId)) {
                    fail(
                        "native-end-marker-invalid",
                        "native modification 结束标记既不是零，也不匹配对象 rawcode。",
                        reader.Position() - 4,
                        4);
                    return false;
                }

                storedModification.sourceOffset = modificationOffset;
                storedModification.sourceLength = reader.Position() - modificationOffset;
                storedModification.publicModificationIndex = publicObject.modifications.size();
                ObjectModification publicModification;
                publicModification.key = MakeModificationKey(
                    type_,
                    storedModification.fieldId,
                    storedModification.levelOrVariation,
                    storedModification.dataIndex,
                    hasLevelDataInWire);
                publicModification.value = storedModification.value;
                publicModification.sourceKey = publicModification.key.fieldId;
                publicModification.origin = OriginSpan{
                    {},
                    static_cast<std::uint64_t>(modificationOffset),
                    static_cast<std::uint64_t>(storedModification.sourceLength),
                    document->sourceSha256_,
                };

                const std::string modificationIdentity = ModificationIdentity(publicModification.key);
                if (!modificationIdentities.insert(modificationIdentity).second) {
                    const auto prefix = "对象 " + IdForDiagnostic(effectiveId) + " 中存在重复字段键；";
                    if (classicV1) fail("native-modification-duplicate", prefix + "无法建立唯一查询身份。", modificationOffset,
                        storedModification.sourceLength);
                    else makeReadOnly("native-modification-duplicate", prefix + "文档可逐字节往返，但禁止歧义编辑。", modificationOffset,
                        storedModification.sourceLength);
                }
                publicObject.modifications.push_back(std::move(publicModification));
                storedObject.modifications.push_back(std::move(storedModification));
            }
            document->objects_.push_back(std::move(publicObject));
            document->storage_.push_back(std::move(storedObject));
        }
        return true;
    };

    if (parseTable(NativeObjectTable::Original)
        && parseTable(NativeObjectTable::Custom)
        && reader.Remaining() != 0) {
        fail(
            "native-trailing-bytes",
            "native 对象文件包含无法解释的尾随字节，已拒绝编辑与序列化。",
            reader.Position(),
            reader.Remaining());
    }

    result.diagnostics = document->parseDiagnostics_;
    result.document = std::move(document);
    return result;
}

Status NativeObjectCodec::Validate(const CodecDocument& document) const {
    const auto* native = dynamic_cast<const NativeObjectDocument*>(&document);
    if (native == nullptr || native->type_ != type_) {
        return Status::Failure(
            std::string(kSubsystem),
            "native-document-type-mismatch",
            "codec 与文档的 native 对象类型不匹配。");
    }
    Status result;
    result.diagnostics = native->parseDiagnostics_;
    if (!native->Supports(NativeObjectOperation::PreserveSource)) {
        if (result.Succeeded()) {
            result.diagnostics.push_back(MakeDiagnostic(
                DiagnosticSeverity::Error,
                "native-document-unsafe",
                "native 文档未通过结构或版本验证。"));
        }
        return result;
    }
    if (!native->Supports(NativeObjectOperation::EditExistingValue)) {
        result.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "native-document-ambiguous",
            "native 文档含有重复对象或字段键，无法建立稳定编辑身份。"));
    }
    return result;
}

Result<std::vector<std::byte>> NativeObjectCodec::Serialize(
    const CodecDocument& document) const {
    const auto* native = dynamic_cast<const NativeObjectDocument*>(&document);
    if (native == nullptr || native->type_ != type_) {
        return Result<std::vector<std::byte>>::Failure(
            std::string(kSubsystem),
            "native-document-type-mismatch",
            "codec 与文档的 native 对象类型不匹配。");
    }
    if (!native->Supports(NativeObjectOperation::PreserveSource)) {
        return Result<std::vector<std::byte>>::Failure(
            std::string(kSubsystem),
            "native-document-unsafe",
            "未知版本或结构不完整的 native 文档禁止序列化。");
    }
    if (!native->edited_) {
        return Result<std::vector<std::byte>>::Success(native->sourceBytes_);
    }
    if (!native->Supports(NativeObjectOperation::EditExistingValue)) {
        return Result<std::vector<std::byte>>::Failure(
            std::string(kSubsystem),
            "native-document-read-only",
            "存在稳定身份歧义的 native 文档禁止生成编辑结果。");
    }

    if (native->version_ == kClassicVersion1) {
        // 只替换值范围；对象头、字段类型、尾标记和未改字段全部复制来源字节。
        // 所有偏移始终属于最初来源，前面字符串变长不会移动后面修改的定位。
        const auto invalidSource = [] {
            return Result<std::vector<std::byte>>::Failure(std::string(kSubsystem),
                "native-serialize-internal-mismatch", "经典 native v1 来源范围或结构不一致，未输出部分修改。");
        };
        const auto outputTooLarge = [] {
            return Result<std::vector<std::byte>>::Failure(std::string(kSubsystem),
                "native-output-too-large", "经典 native v1 编辑结果超过 1 GiB 安全上限，未输出部分修改。");
        };
        const std::size_t headerBytes = HasLevelData(type_) && type_ != ObjectType::Doodad ? 16 : 8;
        std::size_t previousEnd = 0;
        std::size_t replacedBytes = 0;
        std::size_t replacementBytes = 0;
        std::size_t originalCount = 0;
        std::size_t customCount = 0;
        for (const auto& object : native->storage_) {
            if (object.deleted || !object.version3UnknownValues.empty()) return invalidSource();
            if (object.table == NativeObjectTable::Original) ++originalCount;
            else ++customCount;
            for (const auto& modification : object.modifications) {
                if (modification.isNew || modification.sourceOffset < previousEnd
                    || modification.sourceOffset > native->sourceBytes_.size()
                    || modification.sourceLength > native->sourceBytes_.size() - modification.sourceOffset
                    || modification.sourceLength < headerBytes + 5
                    || modification.valueOffset != modification.sourceOffset + headerBytes
                    || modification.valueLength != modification.sourceLength - headerBytes - 4) {
                    return invalidSource();
                }
                previousEnd = modification.sourceOffset + modification.sourceLength;
                if (!modification.changed) continue;
                replacedBytes += modification.valueLength;
                const std::size_t valueBytes = modification.valueType == NativeValueType::String
                    ? std::get<std::string>(modification.value).size() + 1 : sizeof(std::int32_t);
                if (valueBytes > kMaximumInputBytes - replacementBytes) return outputTooLarge();
                replacementBytes += valueBytes;
            }
        }
        if (originalCount != native->originalObjectCount_ || customCount != native->customObjectCount_
            || replacedBytes > native->sourceBytes_.size()) return invalidSource();
        const auto retainedBytes = native->sourceBytes_.size() - replacedBytes;
        if (retainedBytes > kMaximumInputBytes - replacementBytes) return outputTooLarge();
        std::vector<std::byte> output;
        output.reserve(retainedBytes + replacementBytes);
        std::size_t cursor = 0;
        for (const auto& object : native->storage_) {
            for (const auto& modification : object.modifications) {
                if (!modification.changed) continue;
                output.insert(output.end(), native->sourceBytes_.begin() + static_cast<std::ptrdiff_t>(cursor),
                    native->sourceBytes_.begin() + static_cast<std::ptrdiff_t>(modification.valueOffset));
                AppendValue(output, modification.valueType, modification.value);
                cursor = modification.valueOffset + modification.valueLength;
            }
        }
        output.insert(output.end(), native->sourceBytes_.begin() + static_cast<std::ptrdiff_t>(cursor),
            native->sourceBytes_.end());
        return Result<std::vector<std::byte>>::Success(std::move(output));
    }

    std::vector<std::byte> output;
    output.reserve(native->sourceBytes_.size());
    AppendInt32(output, native->version_);

    const auto serializeTable = [&](NativeObjectTable table, std::size_t expectedCount) -> bool {
        if (expectedCount > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            return false;
        }
        AppendInt32(output, static_cast<std::int32_t>(expectedCount));
        std::size_t actualCount = 0;
        for (const NativeObjectDocument::ObjectStorage& object : native->storage_) {
            if (object.table != table || object.deleted) {
                continue;
            }
            ++actualCount;
            AppendArray(output, object.originalId);
            AppendArray(output, object.newId);
            if (object.modifications.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                return false;
            }
            AppendInt32(output, static_cast<std::int32_t>(object.modifications.size()));
            for (const NativeObjectDocument::ModificationStorage& modification : object.modifications) {
                if (!modification.changed && !modification.isNew) {
                    if (modification.sourceOffset > native->sourceBytes_.size()
                        || modification.sourceLength > native->sourceBytes_.size() - modification.sourceOffset) {
                        return false;
                    }
                    output.insert(
                        output.end(),
                        native->sourceBytes_.begin() + static_cast<std::ptrdiff_t>(modification.sourceOffset),
                        native->sourceBytes_.begin() + static_cast<std::ptrdiff_t>(
                            modification.sourceOffset + modification.sourceLength));
                    continue;
                }
                AppendArray(output, modification.fieldId);
                AppendInt32(output, static_cast<std::int32_t>(modification.valueType));
                if (HasLevelData(type_)) {
                    AppendInt32(output, modification.levelOrVariation);
                    AppendInt32(output, modification.dataIndex);
                }
                AppendValue(output, modification.valueType, modification.value);
                AppendArray(output, modification.endMarker);
            }
        }
        return actualCount == expectedCount;
    };

    if (!serializeTable(NativeObjectTable::Original, native->originalObjectCount_)
        || !serializeTable(NativeObjectTable::Custom, native->customObjectCount_)) {
        return Result<std::vector<std::byte>>::Failure(
            std::string(kSubsystem),
            "native-serialize-internal-mismatch",
            "native 内部对象数量或来源范围不一致，已拒绝输出半成品。");
    }
    return Result<std::vector<std::byte>>::Success(std::move(output));
}

SemanticDiff NativeObjectCodec::Diff(
    const CodecDocument& before,
    const CodecDocument& after) const {
    SemanticDiff result;
    const auto* oldNative = dynamic_cast<const NativeObjectDocument*>(&before);
    const auto* newNative = dynamic_cast<const NativeObjectDocument*>(&after);
    if (oldNative == nullptr || newNative == nullptr
        || oldNative->type_ != type_ || newNative->type_ != type_) {
        return result;
    }
    const ObjectSemanticDiff changes = DiffObjects(oldNative->objects_, newNative->objects_);
    for (const ObjectChange& change : changes.changes) {
        result.changes.push_back(SemanticChange{
            "object-modification",
            std::filesystem::path(NativeFileName(type_)),
            ChangeSummary(change),
            {},
            {},
        });
    }
    return result;
}

CodecInspection NativeObjectCodec::Inspect(const CodecDocument& document) const {
    CodecInspection inspection;
    inspection.formatName = Name();
    const auto* native = dynamic_cast<const NativeObjectDocument*>(&document);
    if (native == nullptr || native->type_ != type_) {
        inspection.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "native-document-type-mismatch",
            "codec 与文档的 native 对象类型不匹配。"));
        return inspection;
    }

    std::size_t modificationCount = 0;
    std::size_t changedModificationCount = 0;
    for (const NativeObjectDocument::ObjectStorage& object : native->storage_) {
        modificationCount += object.modifications.size();
        changedModificationCount += static_cast<std::size_t>(std::count_if(
            object.modifications.begin(),
            object.modifications.end(),
            [](const NativeObjectDocument::ModificationStorage& modification) {
                return modification.changed;
            }));
    }
    inspection.properties.emplace_back("objectType", std::string(ToString(type_)));
    inspection.properties.emplace_back("fileName", std::string(NativeFileName(type_)));
    inspection.properties.emplace_back("version", std::to_string(native->version_));
    inspection.properties.emplace_back("originalObjectCount", std::to_string(native->originalObjectCount_));
    inspection.properties.emplace_back("customObjectCount", std::to_string(native->customObjectCount_));
    inspection.properties.emplace_back("modificationCount", std::to_string(modificationCount));
    inspection.properties.emplace_back("changedModificationCount", std::to_string(changedModificationCount));
    inspection.properties.emplace_back("structurallySafe", native->structurallySafe_ ? "true" : "false");
    inspection.properties.emplace_back("editable", native->IsEditable() ? "true" : "false");
    inspection.properties.emplace_back("preserveSource", native->Supports(NativeObjectOperation::PreserveSource) ? "true" : "false");
    inspection.properties.emplace_back("editExistingValue", native->Supports(NativeObjectOperation::EditExistingValue) ? "true" : "false");
    inspection.properties.emplace_back("replaceFromTemplate", native->Supports(NativeObjectOperation::ReplaceFromTemplate) ? "true" : "false");
    inspection.properties.emplace_back("sourceSha256", native->sourceSha256_);
    inspection.diagnostics = native->parseDiagnostics_;
    return inspection;
}

Result<War3Map::ObjectPatch> NativeObjectCodec::BuildPatch(
    const NativeObjectDocument& document,
    std::filesystem::path relativePath,
    std::string summary) const {
    auto encoded = Serialize(document);
    if (!encoded.Succeeded()) {
        Result<War3Map::ObjectPatch> result;
        result.diagnostics = std::move(encoded.diagnostics);
        return result;
    }
    if (document.sourceSha256_.size() != 64) {
        return Result<War3Map::ObjectPatch>::Failure(
            std::string(kSubsystem),
            "native-source-identity-missing",
            "Native 物编 patch 必须绑定解析时的源成员 SHA-256。");
    }

    std::vector<War3Map::BinaryFilePatch> files;
    files.push_back(War3Map::BinaryFilePatch{
        std::move(relativePath),
        std::move(*encoded.value),
        std::move(summary),
        document.sourceSha256_,
    });
    return Result<War3Map::ObjectPatch>::Success(
        War3Map::ObjectPatch(std::move(files), document.Type(), true));
}

} // namespace War3Map::ObjectData
