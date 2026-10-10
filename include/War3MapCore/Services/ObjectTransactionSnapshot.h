#pragma once

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::ObjectData {
class ObjectFieldSchema;
class StandardObjectCatalog;
}

namespace War3Map::Services {

// 原生物编写出输入的不可变快照。调用方只提交已编辑的领域文档，不能提交任意
// replacement 字节。Freeze 负责回灌、校验、编码与回读；保存服务仍使用原共用任务。
// 本对象不授予活动事务租约，宿主提交前还须复核当前 expected_revision。
class ObjectTransactionSnapshot final {
public:
    inline static constexpr std::size_t kMaximumMemberBytes = 64U * 1024U * 1024U;

    [[nodiscard]] static Result<std::shared_ptr<const ObjectTransactionSnapshot>> Freeze(
        const ObjectData::LniObjectDocument& edited,
        std::optional<std::span<const std::byte>> originalNative,
        std::string sourceMapSha256,
        const ObjectData::ObjectFieldSchema* schema = nullptr,
        const ObjectData::StandardObjectCatalog* standard = nullptr);

    [[nodiscard]] ObjectData::ObjectType Type() const noexcept { return edited_.Type(); }
    [[nodiscard]] const ObjectData::LniObjectDocument& EditedDocument() const noexcept { return edited_; }
    // 与 EditedDocument 同一冻结步骤的最终 LNI 产物；保存/脚本入口无需再借用调用方文档。
    [[nodiscard]] std::span<const std::byte> SerializedLni() const noexcept { return serializedLni_; }
    [[nodiscard]] const std::string& LniSha256() const noexcept { return lniSha256_; }
    // nullopt 是成员不存在；存在但零字节的成员不能通过 Freeze，不能被当作缺失。
    [[nodiscard]] std::optional<std::span<const std::byte>> OriginalNative() const noexcept;
    [[nodiscard]] const ObjectData::NativeObjectDocument& CandidateNative() const noexcept { return candidate_; }
    [[nodiscard]] std::span<const std::byte> EncodedNative() const noexcept { return encoded_; }
    [[nodiscard]] const std::string& SourceSha256() const noexcept { return sourceSha256_; }
    [[nodiscard]] const std::string& OriginalSha256() const noexcept { return originalSha256_; }
    [[nodiscard]] const std::string& CandidateSha256() const noexcept { return candidateSha256_; }

    // 保存入口以刚从同一源归档读取的事实核对；成员存在性也是身份的一部分。
    [[nodiscard]] Status VerifyAgainst(std::string_view sourceMapSha256,
        std::optional<std::span<const std::byte>> originalNative) const;

    ObjectTransactionSnapshot(const ObjectTransactionSnapshot&) = delete;
    ObjectTransactionSnapshot& operator=(const ObjectTransactionSnapshot&) = delete;

private:
    ObjectTransactionSnapshot(ObjectData::LniObjectDocument edited,
        std::vector<std::byte> serializedLni, std::string lniSha256,
        std::optional<std::vector<std::byte>> originalNative,
        ObjectData::NativeObjectDocument candidate, std::vector<std::byte> encoded,
        std::string sourceSha256, std::string originalSha256, std::string candidateSha256);

    ObjectData::LniObjectDocument edited_;
    std::vector<std::byte> serializedLni_;
    std::string lniSha256_;
    std::optional<std::vector<std::byte>> original_;
    ObjectData::NativeObjectDocument candidate_;
    std::vector<std::byte> encoded_;
    std::string sourceSha256_, originalSha256_, candidateSha256_;
};

} // namespace War3Map::Services
