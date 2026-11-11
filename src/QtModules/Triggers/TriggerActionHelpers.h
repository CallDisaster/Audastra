#pragma once

#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Types.h"

#include <QAbstractItemView>
#include <QItemSelectionModel>
#include <QObject>
#include <QString>
#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// 触发器模块各操作文件共用的小工具（只在模块内部使用）：诊断文字、目录查找、批量撤销宏、右键选区。
namespace War3Map::Modules::TriggerActions {

inline QString FirstMessage(const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
    return diagnostics.empty() ? fallback : QString::fromUtf8(diagnostics.front().message);
}

inline const Trigger::TriggerRecord* FindTrigger(const Trigger::TriggerDocument& document, StableId id) {
    for (const auto& trigger : document.triggers) {
        if (trigger.id == id) return &trigger;
    }
    return nullptr;
}

// 同类别中紧随其后的触发器；没有时返回 0（表示放到类别末尾）。
inline StableId NextInCategory(const Trigger::TriggerDocument& document, StableId triggerId) {
    bool seen = false;
    std::int32_t categoryId = 0;
    for (const auto& trigger : document.triggers) {
        if (seen && trigger.categoryId == categoryId) return trigger.id;
        if (trigger.id == triggerId) {
            seen = true;
            categoryId = trigger.categoryId;
        }
    }
    return 0;
}

// 两份目录布局的顺序与归属相同（放回原处的移动不入撤销栈）。
inline bool SameDirectoryLayout(const Trigger::TriggerDirectoryLayout& left, const Trigger::TriggerDirectoryLayout& right) {
    return left.triggers == right.triggers
        && std::equal(left.categories.begin(), left.categories.end(), right.categories.begin(), right.categories.end(),
            [](const auto& a, const auto& b) { return a.categoryId == b.categoryId; });
}

// 代码身份服务复制的是本图里现有的触发器：剪贴板里的必须就是它、且正文没改过，不能拿别处的正文冒充。
inline bool CustomSourceIsCurrent(const Trigger::TriggerDocument& document, const Trigger::TriggerSnapshot& snapshot) {
    const auto* source = FindTrigger(document, snapshot.trigger.id);
    return source != nullptr && source->customTextFlag != 0 && source->name.text == snapshot.trigger.name.text
        && source->customText.text == snapshot.trigger.customText.text
        && source->customText.encoding == snapshot.trigger.customText.encoding;
}

// 子树快照根语句的类型（0 事件，1 条件，2 动作）。
inline std::int32_t SnapshotRootType(const Trigger::TriggerDocumentEditor::EcaSubtreeSnapshot& snapshot) {
    for (std::size_t index = 0; index < snapshot.ecas.size() && index < snapshot.ecaOldIndices.size(); ++index) {
        if (snapshot.ecaOldIndices[index] == snapshot.rootOldIndex) return snapshot.ecas[index].type;
    }
    return snapshot.ecas.empty() ? 2 : snapshot.ecas.front().type;
}

// 由程序设定当前项并只选中这一行。扩展多选下 QAbstractItemView::setCurrentIndex 按此刻按住的 Ctrl / Shift
// 决定改选区的方式（按着 Ctrl+V / Ctrl+D 时结果会被追加或反选），所以先照常设当前项（同时更新 Shift 锚点），
// 再明确只选这一行。
inline void SelectOnly(QAbstractItemView& view, const QModelIndex& index) {
    view.setCurrentIndex(index);
    if (index.isValid() && view.selectionModel() != nullptr) {
        view.selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
}

// 右键：点在已选中的行上保留整个选区，只移动当前项；否则改为只选该行。
inline void FocusForContextMenu(QAbstractItemView& view, const QModelIndex& index) {
    if (!index.isValid()) return;
    if (view.selectionModel()->isSelected(index)) {
        view.selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
    } else {
        SelectOnly(view, index);
    }
}

// 批量修改的一步撤销：第一次压入命令时才开宏（只有一条命令时也是一步）；结束时宏里没有命令则整体移除。
class UndoBatch {
public:
    UndoBatch(QUndoStack* stack, QString label) : stack_(stack), label_(std::move(label)) {}
    UndoBatch(const UndoBatch&) = delete;
    UndoBatch& operator=(const UndoBatch&) = delete;
    ~UndoBatch() { Close(); }

    // 由其他服务压入命令前先调用（例如代码身份服务的整图事务）。
    void Open() {
        if (!open_ && stack_ != nullptr) {
            stack_->beginMacro(label_);
            open_ = true;
        }
    }
    void Push(QUndoCommand* command) {
        Open();
        stack_->push(command);
    }
    void Close() {
        if (!open_) return;
        open_ = false;
        stack_->endMacro();
        auto* macro = const_cast<QUndoCommand*>(stack_->command(stack_->index() - 1));
        if (macro != nullptr && macro->childCount() == 0) {
            macro->setObsolete(true);
            stack_->undo(); // 空宏：标记作废后撤销即从栈中移除，文档不动
        }
    }

private:
    QUndoStack* stack_ = nullptr;
    QString label_;
    bool open_ = false;
};

} // namespace War3Map::Modules::TriggerActions
