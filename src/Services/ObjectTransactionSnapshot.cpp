#include "War3MapCore/Services/ObjectTransactionSnapshot.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/ObjectData/ObjectImport.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace War3Map::Services {
namespace {
using namespace ObjectData;
using SnapshotResult = Result<std::shared_ptr<const ObjectTransactionSnapshot>>;
constexpr auto kSubsystem = "Services.ObjectTransaction";

Status Fail(std::string code, std::string message) {
    return Status::Failure(kSubsystem, std::move(code), std::move(message));
}

SnapshotResult Reject(std::string code, std::string message, const std::vector<Diagnostic>& details = {}) {
    auto result = SnapshotResult::Failure(kSubsystem, std::move(code), std::move(message));
    result.diagnostics.insert(result.diagnostics.end(), details.begin(), details.end());
    return result;
}

std::optional<std::string> CanonicalSha(std::string_view value) {
    if (value.size() != 64) return std::nullopt;
    std::string canonical(value);
    for (char& ch : canonical) {
        if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
        else if (!(ch >= '0' && ch <= '9') && !(ch >= 'a' && ch <= 'f')) return std::nullopt;
    }
    return canonical;
}

bool SameValueBits(const Value& left, const Value& right) {
    if (left.index() != right.index()) return false;
    if (const auto* value = std::get_if<float>(&left)) {
        return std::bit_cast<std::uint32_t>(*value) == std::bit_cast<std::uint32_t>(std::get<float>(right));
    }
    return left == right;
}

bool SameNativeFields(const NativeObjectDocument& staged, const NativeObjectDocument& reopened) {
    if (staged.Type() != reopened.Type() || staged.Version() != reopened.Version()
        || staged.OriginalObjectCount() != reopened.OriginalObjectCount()
        || staged.CustomObjectCount() != reopened.CustomObjectCount()
        || staged.Objects().size() != reopened.Objects().size()) return false;
    // 新建 Original 记录可以在内存中位于 Custom 之后；编码时两张表各自分组，按稳定身份对照。
    std::unordered_map<std::string_view, const ObjectRecord*> byId;
    for (const auto& object : reopened.Objects()) {
        if (!byId.emplace(object.key.rawcode, &object).second) return false;
    }
    for (const auto& before : staged.Objects()) {
        const auto found = byId.find(before.key.rawcode);
        if (found == byId.end()) return false;
        const auto& after = *found->second;
        if (before.key != after.key || before.parentRawcode != after.parentRawcode
            || before.modifications.size() != after.modifications.size()) return false;
        for (std::size_t field = 0; field < before.modifications.size(); ++field) {
            const auto& left = before.modifications[field];
            const auto& right = after.modifications[field];
            if (left.key != right.key || !SameValueBits(left.value, right.value)) return false;
        }
    }
    return true;
}

using FieldAddress = std::tuple<std::string_view, std::uint32_t, std::uint32_t, std::uint32_t>;
FieldAddress Address(const ModificationKey& key) {
    // 只读身份对照：LNI 的零占位与 native 的无索引属于同一地址，实际可写性由共用回灌验证。
    return {key.fieldId, key.level.value_or(0), key.dataIndex.value_or(0), key.variation.value_or(0)};
}

bool IsOpaque(const ObjectModification& field) {
    return field.valueWasOpaque || std::holds_alternative<UnknownBytesValue>(field.value);
}

std::optional<std::span<const std::byte>> OriginBytes(const ObjectModification& field,
    std::span<const std::byte> input) {
    if (!field.origin || field.origin->offset > input.size()
        || field.origin->length > input.size() - field.origin->offset) return std::nullopt;
    return input.subspan(static_cast<std::size_t>(field.origin->offset),
        static_cast<std::size_t>(field.origin->length));
}

Status PreserveOpaque(const LniObjectDocument& edited, const NativeObjectDocument& original,
    const NativeObjectDocument& reopened, std::span<const std::byte> source,
    std::span<const std::byte> encoded) {
    if (std::none_of(edited.Objects().begin(), edited.Objects().end(), [](const ObjectRecord& object) {
            return std::any_of(object.modifications.begin(), object.modifications.end(), IsOpaque);
        })) return Status::Ok();
    std::unordered_map<std::string_view, const ObjectRecord*> originals, candidates;
    for (const auto& object : original.Objects()) originals.emplace(object.key.rawcode, &object);
    for (const auto& object : reopened.Objects()) candidates.emplace(object.key.rawcode, &object);
    for (const auto& object : edited.Objects()) {
        if (std::none_of(object.modifications.begin(), object.modifications.end(), IsOpaque)) continue;
        const auto before = originals.find(object.key.rawcode), after = candidates.find(object.key.rawcode);
        if (before == originals.end() || after == candidates.end()) {
            return Fail("OBJECT_TRANSACTION_OPAQUE_LOSS", "不透明物编字段没有可逐字节保留的原生对象来源：" + object.key.rawcode);
        }
        std::map<FieldAddress, const ObjectModification*> sourceFields, candidateFields;
        for (const auto& field : before->second->modifications) sourceFields.emplace(Address(field.key), &field);
        for (const auto& field : after->second->modifications) candidateFields.emplace(Address(field.key), &field);
        for (const auto& field : object.modifications) {
            if (!IsOpaque(field)) continue;
            const auto old = sourceFields.find(Address(field.key)), saved = candidateFields.find(Address(field.key));
            if (old == sourceFields.end() || saved == candidateFields.end()
                || !SameValueBits(old->second->value, saved->second->value)) {
                return Fail("OBJECT_TRANSACTION_OPAQUE_LOSS", "不透明物编字段在候选中丢失或改变：" + object.key.rawcode + "/" + field.key.fieldId);
            }
            const auto oldBytes = OriginBytes(*old->second, source), newBytes = OriginBytes(*saved->second, encoded);
            if (!oldBytes || !newBytes || !std::ranges::equal(*oldBytes, *newBytes)) {
                return Fail("OBJECT_TRANSACTION_OPAQUE_LOSS", "不透明字段的原生字节范围没有完整保留：" + object.key.rawcode + "/" + field.key.fieldId);
            }
        }
    }
    return Status::Ok();
}

} // namespace

ObjectTransactionSnapshot::ObjectTransactionSnapshot(LniObjectDocument edited,
    std::vector<std::byte> serializedLni, std::string lniSha256,
    std::optional<std::vector<std::byte>> originalNative, NativeObjectDocument candidate,
    std::vector<std::byte> encoded, std::string sourceSha256, std::string originalSha256,
    std::string candidateSha256)
    : edited_(std::move(edited)), serializedLni_(std::move(serializedLni)), lniSha256_(std::move(lniSha256)),
      original_(std::move(originalNative)), candidate_(std::move(candidate)),
      encoded_(std::move(encoded)), sourceSha256_(std::move(sourceSha256)),
      originalSha256_(std::move(originalSha256)), candidateSha256_(std::move(candidateSha256)) {
}

std::optional<std::span<const std::byte>> ObjectTransactionSnapshot::OriginalNative() const noexcept {
    if (!original_) return std::nullopt;
    return std::span<const std::byte>(*original_);
}

SnapshotResult ObjectTransactionSnapshot::Freeze(const LniObjectDocument& edited,
    std::optional<std::span<const std::byte>> originalNative, std::string sourceMapSha256,
    const ObjectFieldSchema* schema, const StandardObjectCatalog* standard) {
    const auto sourceSha = CanonicalSha(sourceMapSha256);
    if (!sourceSha) return Reject("OBJECT_TRANSACTION_SOURCE_IDENTITY_INVALID", "源地图身份必须为完整的 64 位十六进制 SHA-256。");
    if (originalNative && originalNative->size() > kMaximumMemberBytes)
        return Reject("OBJECT_TRANSACTION_BYTE_BUDGET", "原生物编成员超过 64 MiB 字节预算。");
    if (originalNative && originalNative->empty())
        return Reject("OBJECT_TRANSACTION_NATIVE_EMPTY", "原生物编成员存在但为空；不能将损坏成员当作缺失并创建空表。");
    if (NativeFileName(edited.Type()).empty())
        return Reject("OBJECT_TRANSACTION_TYPE_INVALID", "物编事务没有七类格式中可识别的原生成员类别。");
    try {
        LniObjectDocument ownedEdited = edited;
        std::optional<std::vector<std::byte>> original;
        if (originalNative) original.emplace(originalNative->begin(), originalNative->end());
        const LniObjectCodec lniCodec(ownedEdited.Type());
        const auto textValid = lniCodec.Validate(ownedEdited);
        if (!textValid.Succeeded() || !ownedEdited.IsStructurallySafe())
            return Reject("OBJECT_TRANSACTION_LNI_INVALID", "编辑后的物编文档未通过结构与值验证。", textValid.diagnostics);
        auto text = lniCodec.Serialize(ownedEdited);
        if (!text.Succeeded() || !text.value)
            return Reject("OBJECT_TRANSACTION_LNI_INVALID", "编辑后的物编不能无损序列化。", text.diagnostics);
        if (text.value->size() > kMaximumMemberBytes)
            return Reject("OBJECT_TRANSACTION_BYTE_BUDGET", "物编文本超过 64 MiB 字节预算。");
        const auto reopenedText = lniCodec.Parse(*text.value);
        const auto* textDocument = reopenedText.Succeeded() && reopenedText.document
            ? dynamic_cast<const LniObjectDocument*>(reopenedText.document.get()) : nullptr;
        if (!textDocument || !textDocument->IsStructurallySafe() || !lniCodec.Validate(*textDocument).Succeeded()
            || !DiffObjects(ownedEdited.Objects(), textDocument->Objects()).Empty())
            return Reject("OBJECT_TRANSACTION_LNI_ROUNDTRIP", "最终 LNI 回读的对象、字段地址或值不同，未冻结候选。", reopenedText.diagnostics);
        const auto textHash = Sha256(*text.value);
        if (!textHash.Succeeded() || !textHash.value)
            return Reject("OBJECT_TRANSACTION_HASH_FAILED", "最终 LNI 身份计算失败。", textHash.diagnostics);

        const NativeObjectCodec nativeCodec(ownedEdited.Type());
        NativeObjectDocument baseline;
        std::vector<Diagnostic> diagnostics = textValid.diagnostics;
        std::string originalSha;
        if (original) {
            const auto parsed = nativeCodec.Parse(*original);
            const auto* document = parsed.Succeeded() && parsed.document
                ? dynamic_cast<const NativeObjectDocument*>(parsed.document.get()) : nullptr;
            if (!document)
                return Reject("OBJECT_TRANSACTION_NATIVE_INVALID", "原生物编成员无法可靠解析。", parsed.diagnostics);
            const auto valid = nativeCodec.Validate(*document);
            if (!valid.Succeeded() || !document->Supports(NativeObjectOperation::EditExistingValue))
                return Reject("OBJECT_TRANSACTION_NATIVE_UNSUPPORTED", "原生物编版本、结构或唯一身份不支持本事务。", valid.diagnostics);
            const auto sourceRoundtrip = nativeCodec.Serialize(*document);
            if (!sourceRoundtrip.Succeeded() || !sourceRoundtrip.value || *sourceRoundtrip.value != *original)
                return Reject("OBJECT_TRANSACTION_SOURCE_ROUNDTRIP", "原生成员未通过原字节往返，不能冻结编辑输入。", sourceRoundtrip.diagnostics);
            baseline = *document;
            diagnostics.insert(diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
            const auto hash = Sha256(*original);
            if (!hash.Succeeded() || !hash.value) return Reject("OBJECT_TRANSACTION_HASH_FAILED", "原生成员身份计算失败。", hash.diagnostics);
            originalSha = *hash.value;
        } else {
            const auto empty = NativeObjectDocument::CreateEmpty(ownedEdited.Type(), 2);
            if (!empty.Succeeded() || !empty.value)
                return Reject("OBJECT_TRANSACTION_NATIVE_INVALID", "缺失成员的经典 v2 空表创建失败。", empty.diagnostics);
            baseline = *empty.value;
        }

        NativeObjectDocument candidate = baseline;
        const auto applied = ApplyLniChangesToNative(candidate, ownedEdited, schema, standard);
        if (!applied.Succeeded())
            return Reject("OBJECT_TRANSACTION_APPLY_REJECTED", "编辑物编不能完整回灌到原生成员；没有产生部分候选。", applied.diagnostics);
        const auto valid = nativeCodec.Validate(candidate);
        if (!valid.Succeeded())
            return Reject("OBJECT_TRANSACTION_CANDIDATE_INVALID", "回灌后的原生物编未通过验证。", valid.diagnostics);
        auto encoded = nativeCodec.Serialize(candidate);
        if (!encoded.Succeeded() || !encoded.value)
            return Reject("OBJECT_TRANSACTION_CANDIDATE_INVALID", "回灌后的原生物编无法编码。", encoded.diagnostics);
        if (encoded.value->size() > kMaximumMemberBytes)
            return Reject("OBJECT_TRANSACTION_BYTE_BUDGET", "候选物编成员超过 64 MiB 字节预算。");
        const auto reparsed = nativeCodec.Parse(*encoded.value);
        const auto* reopened = reparsed.Succeeded() && reparsed.document
            ? dynamic_cast<const NativeObjectDocument*>(reparsed.document.get()) : nullptr;
        if (!reopened || !nativeCodec.Validate(*reopened).Succeeded() || !SameNativeFields(candidate, *reopened))
            return Reject("OBJECT_TRANSACTION_CANDIDATE_ROUNDTRIP", "候选回读的对象、字段地址或值位模式不一致。", reparsed.diagnostics);
        const auto roundtrip = nativeCodec.Serialize(*reopened);
        if (!roundtrip.Succeeded() || !roundtrip.value || *roundtrip.value != *encoded.value)
            return Reject("OBJECT_TRANSACTION_CANDIDATE_ROUNDTRIP", "候选成员没有通过完整字节回读往返。", roundtrip.diagnostics);
        const auto opaque = PreserveOpaque(ownedEdited, baseline, *reopened,
            original ? std::span<const std::byte>(*original) : std::span<const std::byte>{}, *encoded.value);
        if (!opaque.Succeeded()) return Reject("OBJECT_TRANSACTION_OPAQUE_LOSS", "候选未保留全部有来源的不透明字段。", opaque.diagnostics);
        const auto hash = Sha256(*encoded.value);
        if (!hash.Succeeded() || !hash.value) return Reject("OBJECT_TRANSACTION_HASH_FAILED", "候选成员身份计算失败。", hash.diagnostics);
        auto snapshot = std::shared_ptr<const ObjectTransactionSnapshot>(new ObjectTransactionSnapshot(
            std::move(ownedEdited), std::move(*text.value), *textHash.value,
            std::move(original), *reopened, std::move(*encoded.value),
            *sourceSha, std::move(originalSha), *hash.value));
        auto result = SnapshotResult::Success(std::move(snapshot));
        result.diagnostics = std::move(diagnostics);
        return result;
    } catch (const std::exception& error) {
        return Reject("OBJECT_TRANSACTION_EXCEPTION", "物编冻结在隔离状态中失败：" + std::string(error.what()));
    }
}

Status ObjectTransactionSnapshot::VerifyAgainst(std::string_view sourceMapSha256,
    std::optional<std::span<const std::byte>> originalNative) const {
    const auto sourceSha = CanonicalSha(sourceMapSha256);
    if (!sourceSha || *sourceSha != sourceSha256_)
        return Fail("OBJECT_TRANSACTION_SOURCE_CONFLICT", "源地图 SHA-256 与冻结事务不同，必须重新取得当前物编修订。");
    if (originalNative.has_value() != original_.has_value())
        return Fail("OBJECT_TRANSACTION_MEMBER_PRESENCE_CONFLICT", "原生成员的存在性发生变化；缺失成员与存在的空成员不是同一来源。");
    if (!originalNative) return Status::Ok();
    if (originalNative->size() > kMaximumMemberBytes)
        return Fail("OBJECT_TRANSACTION_BYTE_BUDGET", "复核的原生物编超过 64 MiB 字节预算。");
    const auto hash = Sha256(*originalNative);
    if (!hash.Succeeded() || !hash.value) return {hash.diagnostics};
    if (*hash.value != originalSha256_ || !std::ranges::equal(*originalNative, *original_))
        return Fail("OBJECT_TRANSACTION_MEMBER_CONFLICT", "源地图中的原生物编字节已改变，不能使用旧的冻结候选。");
    return Status::Ok();
}

} // namespace War3Map::Services
