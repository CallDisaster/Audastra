#pragma once

#include "War3MapCore/Codec.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/ObjectData/ReferenceAudit.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::ObjectData {

class NativeObjectDocument;
class ObjectFieldSchema;
class StandardObjectCatalog;

class LniObjectDocument final : public CodecDocument {
public:
    struct CloneSource {
        std::string newRawcode;
        std::string sourceRawcode;
    };
    static constexpr std::size_t kMaximumCloneSources = 65'536;
    // 克隆物化阶段的 native wire 文档峰值；不是进程内所有副本的总堆预算。
    static constexpr std::size_t kMaximumNativeCloneProjectionBytes = 64ull * 1024ull * 1024ull;
    [[nodiscard]] std::string FormatName() const override;
    [[nodiscard]] ObjectType Type() const noexcept;
    [[nodiscard]] std::span<const ObjectRecord> Objects() const noexcept;
    [[nodiscard]] const ObjectRecord* FindObject(const ObjectKey& key) const noexcept;
    [[nodiscard]] bool IsStructurallySafe() const noexcept;
    [[nodiscard]] std::string DocumentFingerprint() const;

    [[nodiscard]] Status SetModification(
        const ObjectKey& object,
        const ModificationKey& modification,
        Value value);
    // 只移除指定覆盖行，保留其余字段和未知原文。
    [[nodiscard]] Status RemoveModification(const ObjectKey& object, const ModificationKey& modification);
    [[nodiscard]] Status AddObject(
        const std::string& rawcode,
        const std::string& parentRawcode);
    // 原版物体的地图覆盖记录，没有 _parent；不会改写游戏资源。
    [[nodiscard]] Status AddOriginalObject(const std::string& rawcode);
    // 复制一个已有自定义对象的完整节，包括未知但可安全框定的原文。
    // 保留 _parent；新 rawcode 必须未占用。原版覆盖不能作为复制来源。
    [[nodiscard]] Status CloneCustomObject(
        const ObjectKey& source,
        const std::string& newRawcode);
    // 来源只由克隆操作记录；普通 LNI Parse 不从相似值猜测 native wire 类型。
    [[nodiscard]] std::span<const CloneSource> CloneSources() const noexcept;
    [[nodiscard]] Status PrepareNativeClones(NativeObjectDocument& nativeDocument) const;
    // 工程恢复以实际 native 基线验证来源、父级与拓扑；同事务新增的普通根
    // 先走严格的新增字段编码，不能从 LNI 相似值推测已有 native 来源类型。
    [[nodiscard]] Status RestoreCloneSources(std::span<const CloneSource> sources,
        const NativeObjectDocument& nativeBaseline,
        const ObjectFieldSchema* schema = nullptr,
        const StandardObjectCatalog* standardCatalog = nullptr);
    // 仅在成功切换到已包含新对象的地图基线后丢弃临时来源。
    void ClearCloneSources() noexcept;
    [[nodiscard]] Status DeleteObject(
        const ObjectKey& object,
        const DeletionAuditToken& token);

private:
    friend class LniObjectCodec;

    enum class LineKind : std::uint8_t {
        Blank,
        Comment,
        Section,
        Assignment,
        OpaqueContinuation,
    };

    struct SourceLine {
        LineKind kind = LineKind::Blank;
        std::string text;
        std::string ending;
        std::string prefix;
        std::string renderedValue;
        std::string suffix;
        std::uint64_t offset = 0;
    };

    [[nodiscard]] std::string Render() const;
    [[nodiscard]] Status RequireEditable(std::string_view operation) const;
    [[nodiscard]] Status AppendObject(const std::string& rawcode,
        const std::optional<std::string>& parentRawcode);

    ObjectType type_ = ObjectType::Unit;
    bool hasUtf8Bom_ = false;
    bool structurallySafe_ = true;
    std::string sourceSha256_;
    std::string preferredLineEnding_ = "\n";
    std::vector<SourceLine> lines_;
    std::vector<ObjectRecord> objects_;
    std::vector<CloneSource> cloneSources_;
    std::vector<Diagnostic> parseDiagnostics_;
};

class LniObjectCodec final : public IFormatCodec {
public:
    explicit LniObjectCodec(ObjectType type) noexcept;

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
        const LniObjectDocument& document,
        std::filesystem::path relativePath,
        std::string summary) const;

private:
    ObjectType type_;
};

// 将一组 ObjectRecord 直接渲染为 LNI 文本字节（用于从 native w3u 等导入）。
// 原始对象（无 parentRawcode）不写 _parent；自定义对象写 _parent。
// 无法安全编码为 LNI 的值（如 UnknownBytes）会被跳过并计入诊断。
[[nodiscard]] Result<std::vector<std::byte>> RenderObjectRecordsToLni(
    ObjectType type, std::span<const ObjectRecord> records);

} // namespace War3Map::ObjectData
