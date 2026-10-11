#pragma once

#include "War3MapCore/Trigger/TriggerCodeEditing.h"

#include <map>
#include <utility>

namespace War3Map::Trigger {

// 工程拥有的 GUI 转码快照。只保存到 .forge，绝不写进 WTG/WCT 或游戏地图。
// 查找绑定当前文档来源、目录、稳定身份、记录元数据和完整正文；持久化另绑定整份
// WTG/WCT 与工程来源。重新解析会产生新稳定身份，只有完整容器绑定通过才按槽位重绑。
class GuiConversionSnapshots {
    struct Entry {
        GuiTriggerFragment fragment;
        std::string sourceWtg, sourceWct, catalogSha, codeSha, recordSha;
        std::uint64_t version = 0;
    };
    using Entries = std::map<StableId, Entry>;
public:
    static constexpr std::size_t kMaxBytes = 64 * 1024 * 1024;
    class PreparedSnapshot {
    public:
        PreparedSnapshot() = default;
        PreparedSnapshot(PreparedSnapshot&& other) noexcept
            : entry_(std::move(other.entry_)), id_(std::exchange(other.id_, 0)) {}
        PreparedSnapshot& operator=(PreparedSnapshot&& other) noexcept {
            if (this != &other) { entry_ = std::move(other.entry_); id_ = std::exchange(other.id_, 0); }
            return *this;
        }
    private:
        friend class GuiConversionSnapshots;
        Entries::node_type entry_;
        StableId id_ = 0;
    };
    // 在确认或文档发布前完成分配；取消/拒绝时直接丢弃，不更改已有快照或修订。
    [[nodiscard]] static Result<PreparedSnapshot> Prepare(const TriggerDocument& document,
        const UiCatalog& catalog, StableId id, std::string_view generatedCode);
    void Publish(PreparedSnapshot&& snapshot) noexcept;
    // 和同一个代码转换收据一起交换；暂存上一份，撤销/重做均不复制或重新分配子树。
    void Swap(PreparedSnapshot& snapshot) noexcept;
    [[nodiscard]] Status Remember(const TriggerDocument& document, const UiCatalog& catalog,
        StableId id, std::string_view generatedCode);
    [[nodiscard]] const GuiTriggerFragment* Find(const TriggerDocument& document,
        const UiCatalog& catalog, StableId id) const;
    void Clear() noexcept { if (!snapshots_.empty()) { snapshots_.clear(); ++revision_; } }
    [[nodiscard]] bool Empty() const noexcept { return snapshots_.empty(); }
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    // 只计当前代码态实际能命中的快照；已撤销到 GUI 的缓存不能制造工程脏标记。
    [[nodiscard]] std::string ActiveFingerprint(const TriggerDocument& document, const UiCatalog& catalog) const;

    [[nodiscard]] Result<std::vector<std::byte>> Serialize(const TriggerDocument& document,
        const UiCatalog& catalog, std::string_view projectSourceIdentity) const;
    [[nodiscard]] static Result<GuiConversionSnapshots> Parse(std::span<const std::byte> bytes,
        const TriggerDocument& document, const UiCatalog& catalog, std::string_view projectSourceIdentity);

private:
    Entries snapshots_;
    std::uint64_t revision_ = 0;
    std::uint64_t nextVersion_ = 0;
};

} // namespace War3Map::Trigger
