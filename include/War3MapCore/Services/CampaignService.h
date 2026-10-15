#pragma once

#include "War3MapCore/Campaign/CampaignInfo.h"
#include "War3MapCore/EditRevision.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <map>
#include <memory>
#include <set>

namespace War3Map::Services {

struct CampaignRevision {
    std::uint64_t document = 0;
    std::uint64_t revision = 0;
    bool operator==(const CampaignRevision&) const = default;
};

enum class CampaignOperation {
    SetInfo, AddMap, ReplaceMap, RemoveMap, MoveMap,
    AddButton, UpdateButton, RemoveButton, MoveButton,
    ReplaceSharedData, ImportResource, RemoveResource
};

struct CampaignCommand {
    CampaignOperation operation = CampaignOperation::SetInfo;
    StableId id = 0;
    std::size_t position = 0;
    Campaign::CampaignInfo info; // SetInfo 只替换设置；地图和按钮通过独立命令。
    Campaign::MenuButton button;
    std::string path;
    // 新增/替换章节时只读此文件；事务拥有完整字节，不借用磁盘文件作为长期来源。
    std::filesystem::path mapSource;
    std::vector<MemberReplacement> replacements; // 共享物编/资源批次；不允许任意内部成员。
    std::vector<std::string> removals;
};

struct CampaignMemberInfo {
    std::string path;
    std::uint64_t size = 0;
    bool changed = false;
};
struct CampaignPage {
    CampaignRevision revision;
    std::size_t total = 0;
    std::size_t nextOffset = 0;
    std::vector<CampaignMemberInfo> members;
};
struct CampaignSaveReceipt {
    ArchiveSaveResult written;
    std::filesystem::path backup;
};

// UI 与 MCP 的共同领域会话。唯一发布点为 Apply/Undo/Redo；所有写入沿用归档保存、备份和回读。
// 保存不会扁平重建原归档，匿名成员、未知成员和原压缩块仍由 MapArchiveSession 保护。
class CampaignSession {
public:
    [[nodiscard]] static Result<std::shared_ptr<CampaignSession>> Open(const std::filesystem::path& path);
    [[nodiscard]] static std::shared_ptr<CampaignSession> Create();
    [[nodiscard]] CampaignRevision Revision() const;
    [[nodiscard]] const Campaign::CampaignInfo& Info() const noexcept { return state_.info; }
    [[nodiscard]] const std::filesystem::path& SourcePath() const noexcept;
    [[nodiscard]] const std::vector<Diagnostic>& Diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] bool Dirty() const { return edits_.IsDirty(EditSaveTarget::Map); }
    [[nodiscard]] bool CanUndo() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool CanRedo() const noexcept { return cursor_ < history_.size(); }
    [[nodiscard]] std::size_t HistorySize() const noexcept { return history_.size(); }
    [[nodiscard]] std::size_t HistoryPosition() const noexcept { return cursor_; }
    [[nodiscard]] Result<StableId> Apply(CampaignRevision expected, const CampaignCommand& command);
    [[nodiscard]] Status Undo(CampaignRevision expected);
    [[nodiscard]] Status Redo(CampaignRevision expected);
    [[nodiscard]] Result<CampaignPage> Members(CampaignRevision expected, std::size_t offset, std::size_t limit) const;
    [[nodiscard]] Result<std::vector<std::byte>> ReadMember(CampaignRevision expected, std::string_view path) const;
    // 固定名字的只读捕获，含会话覆盖/删除；缺失与空成员分别记录。
    [[nodiscard]] Result<std::vector<ArchiveExactMember>> CaptureExactMembers(CampaignRevision expected,
        std::span<const std::string> names, const ArchiveExactMemberLimits& limits = {}) const;
    [[nodiscard]] Status ExportMember(CampaignRevision expected, std::string_view path, const std::filesystem::path& target) const;
    [[nodiscard]] Result<CampaignSaveReceipt> Save(CampaignRevision expected,
        const std::filesystem::path& target, bool inPlace, const InPlaceSaveOptions& backup = {});
    // 读取 WTS；无法解析时保留 TRIGSTR 原值并返回诊断。
    [[nodiscard]] Result<std::string> ResolveText(std::string_view text) const;
    [[nodiscard]] static std::string ObjectMember(ObjectData::ObjectType type);

private:
    struct OwnedMember { std::string name, sha256; std::shared_ptr<const std::vector<std::byte>> bytes; };
    struct State {
        Campaign::CampaignInfo info;
        std::map<std::string, OwnedMember> replacements;
        std::set<std::string> removals;
    };
    struct Change { State before, after; std::string title; };
    CampaignSession();
    [[nodiscard]] Status Check(CampaignRevision expected) const;
    [[nodiscard]] Status Publish(State next, std::string title, EditOperation operation);
    [[nodiscard]] Result<EditScopeSnapshot> Scopes(const State& state) const;
    [[nodiscard]] Status Validate(const State& state) const;
    [[nodiscard]] bool Exists(const State& state, std::string_view name) const;
    [[nodiscard]] Result<std::vector<std::byte>> Read(const State& state, std::string_view name) const;
    [[nodiscard]] Status Put(State& state, std::string name, std::span<const std::byte> bytes) const;
    [[nodiscard]] Status UpdateImports(State& state, std::span<const std::string> add,
        std::span<const std::string> remove) const;
    std::shared_ptr<MapArchiveSession> archive_;
    State state_;
    EditRevision edits_;
    std::uint64_t identity_ = 0;
    StableId nextId_ = 1;
    std::vector<Change> history_;
    std::size_t cursor_ = 0;
    std::vector<Diagnostic> diagnostics_;
};
} // namespace War3Map::Services
