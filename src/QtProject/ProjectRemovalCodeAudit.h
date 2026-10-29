#pragma once
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>

namespace War3Map::Services { struct CodeCheckProjectSnapshot; }
namespace War3Map::QtProject {
class EditorProject;

// 界面删除触发器/变量前，在代码里找到的引用。只用来提示作者，不阻止删除（与 WE 一致）。
struct RemovalCodeNotice {
    QStringList lines;          // 每处一行，最多 8 行；超出时末行写“还有 N 处”
    std::size_t references = 0; // 确定引用、可能的按名使用合计；变量还含触发器参数引用
    QString incomplete;         // 没能检查完的原因；空表示检查完整
};

// 与 Agent 删除门禁同一审计（直接引用、按名调用、预处理展开后的库文件）。
// Prepare 在界面线程冻结工程快照；Run 只用冻结快照，可在工作线程调用；应用结果前用 Current 核对工程未变。
class RemovalCodeAudit {
public:
    // removedTriggers：同一次删除里一起删掉的触发器，它们自己代码里的引用不算。
    [[nodiscard]] static RemovalCodeAudit Prepare(const EditorProject& project,
        std::vector<Trigger::GraphRemove> targets, std::set<StableId> removedTriggers);
    // stop 置位后在两个目标之间停下，返回空。
    [[nodiscard]] std::optional<RemovalCodeNotice> Run(
        const std::function<void(std::size_t done, std::size_t total)>& progress, const std::atomic_bool& stop) const;
    [[nodiscard]] bool Current(const EditorProject& project) const;
    [[nodiscard]] std::size_t TargetCount() const noexcept { return targets_.size(); }

private:
    std::shared_ptr<const Services::CodeCheckProjectSnapshot> snapshot_;
    std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjects_;
    std::vector<Trigger::GraphRemove> targets_;
    std::set<StableId> removedTriggers_;
    QString unavailable_;
    std::uint64_t generation_ = 0, revision_ = 0;
};
}
