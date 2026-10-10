#include "War3MapCore/EditRevision.h"

#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace War3Map {
namespace {

std::uint64_t NewOwner() {
    static std::atomic<std::uint64_t> sequence{1};
    auto value = sequence.load(std::memory_order_relaxed);
    do {
        if (value == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("编辑修订服务身份已耗尽");
        }
    } while (!sequence.compare_exchange_weak(value, value + 1, std::memory_order_relaxed));
    return value;
}

Status Validate(const EditScopeSnapshot& snapshot) {
    for (const auto& [scope, fingerprint] : snapshot) {
        if (scope.empty() || scope.find('\0') != std::string::npos) {
            return Status::Failure("EditRevision", "edit.scope-invalid", "文档范围必须具有非空的稳定身份");
        }
        if (fingerprint.empty()) {
            return Status::Failure("EditRevision", "edit.fingerprint-unknown",
                "无法确认文档内容指纹：" + scope);
        }
    }
    return Status::Ok();
}

bool ValidTarget(EditSaveTarget target) {
    return target == EditSaveTarget::Project || target == EditSaveTarget::Map;
}

std::vector<EditScopeChange> Changes(const EditScopeSnapshot& before,
    const EditScopeSnapshot& after) {
    std::vector<EditScopeChange> changes;
    auto left = before.begin();
    auto right = after.begin();
    while (left != before.end() || right != after.end()) {
        if (right == after.end() || (left != before.end() && left->first < right->first)) {
            changes.push_back({left->first, left->second, std::nullopt});
            ++left;
        } else if (left == before.end() || right->first < left->first) {
            changes.push_back({right->first, std::nullopt, right->second});
            ++right;
        } else {
            if (left->second != right->second) {
                changes.push_back({left->first, left->second, right->second});
            }
            ++left;
            ++right;
        }
    }
    return changes;
}

std::string DefaultDescription(EditOperation operation) {
    switch (operation) {
    case EditOperation::Edit: return "修改文档";
    case EditOperation::Undo: return "撤销修改";
    case EditOperation::Redo: return "重做修改";
    case EditOperation::Restore: return "恢复文档";
    }
    return {};
}

} // namespace

EditRevision::EditRevision() : owner_(NewOwner()) {}

Status EditRevision::Reset(EditScopeSnapshot current,
    std::optional<EditScopeSnapshot> projectBaseline,
    std::optional<EditScopeSnapshot> mapBaseline) {
    for (const auto* snapshot : {&current, projectBaseline ? &*projectBaseline : nullptr,
             mapBaseline ? &*mapBaseline : nullptr}) {
        if (!snapshot) continue;
        auto status = Validate(*snapshot);
        if (!status.Succeeded()) return status;
    }
    // 所有可能分配内存的准备先完成，失败时现有会话不被部分替换。
    std::optional<EditSavedBaseline> project;
    std::optional<EditSavedBaseline> map;
    if (projectBaseline) project.emplace(EditSavedBaseline{0, std::move(*projectBaseline)});
    if (mapBaseline) map.emplace(EditSavedBaseline{0, std::move(*mapBaseline)});
    std::scoped_lock lock(mutex_);
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::Failure("EditRevision", "edit.generation-exhausted", "编辑会话编号已耗尽");
    }
    current_ = std::move(current);
    projectBaseline_ = std::move(project);
    mapBaseline_ = std::move(map);
    revision_ = 0;
    ++generation_;
    projectSave_.reset();
    mapSave_.reset();
    journal_.clear();
    return Status::Ok();
}

Status EditRevision::Observe(EditScopeSnapshot current, std::string description,
    EditOperation operation) {
    auto status = Validate(current);
    if (!status.Succeeded()) return status;
    const auto defaultDescription = DefaultDescription(operation);
    if (defaultDescription.empty()) {
        return Status::Failure("EditRevision", "edit.operation-invalid", "未知的编辑操作类型");
    }
    std::scoped_lock lock(mutex_);
    if (current == current_) return Status::Ok();
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status::Failure("EditRevision", "edit.revision-exhausted", "文档修订编号已耗尽");
    }
    EditJournalEntry entry{revision_ + 1, operation,
        description.empty() ? defaultDescription : std::move(description), Changes(current_, current)};
    journal_.push_back(std::move(entry));
    current_ = std::move(current);
    ++revision_;
    return Status::Ok();
}

EditScopeSnapshot EditRevision::Current() const {
    std::scoped_lock lock(mutex_);
    return current_;
}

std::uint64_t EditRevision::Generation() const {
    std::scoped_lock lock(mutex_);
    return generation_;
}

std::uint64_t EditRevision::Revision() const {
    std::scoped_lock lock(mutex_);
    return revision_;
}

std::vector<EditJournalEntry> EditRevision::Journal() const {
    std::scoped_lock lock(mutex_);
    return journal_;
}

std::optional<EditSavedBaseline>& EditRevision::BaselineFor(EditSaveTarget target) {
    return target == EditSaveTarget::Map ? mapBaseline_ : projectBaseline_;
}

const std::optional<EditSavedBaseline>& EditRevision::BaselineFor(EditSaveTarget target) const {
    return target == EditSaveTarget::Map ? mapBaseline_ : projectBaseline_;
}

std::optional<EditRevision::PendingSave>& EditRevision::PendingFor(EditSaveTarget target) {
    return target == EditSaveTarget::Map ? mapSave_ : projectSave_;
}

std::optional<EditSavedBaseline> EditRevision::Baseline(EditSaveTarget target) const {
    if (!ValidTarget(target)) return std::nullopt;
    std::scoped_lock lock(mutex_);
    return BaselineFor(target);
}

bool EditRevision::IsDirty(EditSaveTarget target) const {
    if (!ValidTarget(target)) return true;
    std::scoped_lock lock(mutex_);
    const auto& baseline = BaselineFor(target);
    return baseline ? current_ != baseline->scopes : !current_.empty();
}

std::vector<std::string> EditRevision::DirtyScopes(EditSaveTarget target) const {
    std::scoped_lock lock(mutex_);
    static const EditScopeSnapshot empty;
    // 未知保存类型绝不回退到某份已保存基线。
    const auto baseline = ValidTarget(target) ? &BaselineFor(target) : nullptr;
    const auto differences = Changes(baseline && *baseline ? (**baseline).scopes : empty, current_);
    std::vector<std::string> scopes;
    scopes.reserve(differences.size());
    for (const auto& change : differences) scopes.push_back(change.scope);
    return scopes;
}

Result<EditSaveTicket> EditRevision::BeginSave(EditSaveTarget target) {
    if (!ValidTarget(target)) {
        return Result<EditSaveTicket>::Failure("EditRevision", "edit.save-target-invalid", "未知的保存目标");
    }
    std::scoped_lock lock(mutex_);
    auto& pending = PendingFor(target);
    if (pending) {
        return Result<EditSaveTicket>::Failure("EditRevision", "edit.save-active", "此目标已有保存正在进行");
    }
    if (nextSaveSequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<EditSaveTicket>::Failure("EditRevision", "edit.save-sequence-exhausted", "保存票据编号已耗尽");
    }
    pending.emplace(PendingSave{nextSaveSequence_, {revision_, current_}});
    return Result<EditSaveTicket>::Success(
        EditSaveTicket(owner_, generation_, nextSaveSequence_++, revision_, target));
}

Status EditRevision::CompleteSave(const EditSaveTicket& ticket, bool succeeded) {
    std::scoped_lock lock(mutex_);
    if (ticket.owner_ != owner_ || ticket.generation_ != generation_) {
        return Status::Failure("EditRevision", "edit.save-session-mismatch", "保存结果不属于当前编辑会话");
    }
    auto& pending = PendingFor(ticket.target_);
    if (!pending || pending->sequence != ticket.sequence_) {
        return Status::Failure("EditRevision", "edit.save-ticket-inactive", "保存票据已结束或失效");
    }
    if (succeeded) BaselineFor(ticket.target_) = std::move(pending->snapshot);
    pending.reset();
    return Status::Ok();
}

} // namespace War3Map
