#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace War3Map {

// Scope 是稳定的文档身份，例如 objects/unit、triggers、imports。
// 指纹由格式文档按实际内容生成；缺席表示该文档不存在，空字符串表示未知，不能提交。
using EditScopeSnapshot = std::map<std::string, std::string>;

enum class EditSaveTarget { Project, Map };
enum class EditOperation { Edit, Undo, Redo, Restore };

struct EditScopeChange {
    std::string scope;
    std::optional<std::string> before;
    std::optional<std::string> after;
};

struct EditJournalEntry {
    std::uint64_t revision = 0;
    EditOperation operation = EditOperation::Edit;
    std::string description;
    std::vector<EditScopeChange> changes;
};

struct EditSavedBaseline {
    std::uint64_t revision = 0;
    EditScopeSnapshot scopes;
};

// 保存票据只能由 BeginSave 生成；其中捕获的文档状态由服务持有，调用者无法修改。
class EditSaveTicket {
public:
    [[nodiscard]] EditSaveTarget Target() const noexcept { return target_; }
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }

private:
    friend class EditRevision;
    EditSaveTicket(std::uint64_t owner, std::uint64_t generation,
        std::uint64_t sequence, std::uint64_t revision, EditSaveTarget target)
        : owner_(owner), generation_(generation), sequence_(sequence),
          revision_(revision), target_(target) {}

    std::uint64_t owner_ = 0;
    std::uint64_t generation_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t revision_ = 0;
    EditSaveTarget target_ = EditSaveTarget::Project;
};

// 只记录真实文档内容，不执行编辑命令，也不把编辑副本等同于已编译游戏地图。
// 各入口可并发调用；调用方仍须用同一份文档快照生成指纹和保存内容。
// 所有 scope 均参与两个保存目标的比较；工程专有布局应由独立 scope 服务管理。
class EditRevision {
public:
    EditRevision();
    EditRevision(const EditRevision&) = delete;
    EditRevision& operator=(const EditRevision&) = delete;

    // 切换项目或恢复：先验证全部快照，再一次替换状态。未提供的基线表示尚未保存。
    // 打开源地图时通常将 current 同时用作两份基线；恢复工程可提供不同地图基线。
    [[nodiscard]] Status Reset(EditScopeSnapshot current = {},
        std::optional<EditScopeSnapshot> projectBaseline = std::nullopt,
        std::optional<EditScopeSnapshot> mapBaseline = std::nullopt);

    // Undo/Redo 标记只用于日志；撤销的内容变化仍由现有命令栈执行。
    // 内容相同的通知不增加修订或日志，取消和失败操作不会制造脏状态。
    [[nodiscard]] Status Observe(EditScopeSnapshot current, std::string description,
        EditOperation operation = EditOperation::Edit);

    [[nodiscard]] EditScopeSnapshot Current() const;
    [[nodiscard]] std::uint64_t Generation() const;
    [[nodiscard]] std::uint64_t Revision() const;
    [[nodiscard]] std::vector<EditJournalEntry> Journal() const;
    [[nodiscard]] std::optional<EditSavedBaseline> Baseline(EditSaveTarget target) const;
    [[nodiscard]] bool IsDirty(EditSaveTarget target) const;
    [[nodiscard]] std::vector<std::string> DirtyScopes(EditSaveTarget target) const;

    // 同一目标只能有一项待完成保存，工程与地图可以分别捕获。
    [[nodiscard]] Result<EditSaveTicket> BeginSave(EditSaveTarget target);
    // 必须在输出已完整写入并验证后传入 true；恢复点不应提交为手动保存基线。
    // false 会结束票据，但不修改基线。成功只提交捕获状态，不会清除后续编辑。
    [[nodiscard]] Status CompleteSave(const EditSaveTicket& ticket, bool succeeded);

private:
    struct PendingSave {
        std::uint64_t sequence = 0;
        EditSavedBaseline snapshot;
    };
    [[nodiscard]] std::optional<EditSavedBaseline>& BaselineFor(EditSaveTarget target);
    [[nodiscard]] const std::optional<EditSavedBaseline>& BaselineFor(EditSaveTarget target) const;
    [[nodiscard]] std::optional<PendingSave>& PendingFor(EditSaveTarget target);

    mutable std::mutex mutex_;
    const std::uint64_t owner_;
    std::uint64_t generation_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t nextSaveSequence_ = 1;
    EditScopeSnapshot current_;
    std::optional<EditSavedBaseline> projectBaseline_;
    std::optional<EditSavedBaseline> mapBaseline_;
    std::optional<PendingSave> projectSave_;
    std::optional<PendingSave> mapSave_;
    std::vector<EditJournalEntry> journal_;
};

} // namespace War3Map
