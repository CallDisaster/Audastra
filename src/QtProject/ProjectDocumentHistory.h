#pragma once

#include "War3MapCore/Types.h"

#include <QByteArray>
#include <QMetaObject>
#include <QPointer>
#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

class QUndoCommand;
class QUndoStack;

namespace War3Map::QtProject {
class EditorProject;

// 将现有撤销栈的文档状态持久化。不执行命令，也不创建另一条会话内的编辑路径。
class ProjectDocumentHistory {
public:
    static constexpr std::size_t MaximumBytes = 64u * 1024u * 1024u;
    static constexpr auto Member = "overlay/editor/document-history.bin";
    explicit ProjectDocumentHistory(EditorProject& project);
    ~ProjectDocumentHistory();

    void Reset();
    void ResetTriggers();
    void RefreshSavedObjects();
    [[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }
    [[nodiscard]] Result<std::vector<std::byte>> Freeze() const;
    [[nodiscard]] Status Install(std::span<const std::byte> bytes);

private:
    struct State {
        QByteArray first, second, extra;
        QByteArray assets;
        [[nodiscard]] std::size_t Bytes() const noexcept;
        bool operator==(const State&) const = default;
    };
    struct Track {
        QPointer<QUndoStack> stack;
        QMetaObject::Connection connection;
        std::vector<const QUndoCommand*> commands;
        std::vector<std::optional<State>> states;
        std::size_t index = 0;
        bool prefixLost = false;
        bool limitReported = false;
    };

    [[nodiscard]] std::optional<State> Capture(std::size_t scope) const;
    void Attach(std::size_t scope, QUndoStack* stack);
    void Observe(std::size_t scope);
    void EnforceBudget(std::size_t keepScope, std::size_t keepIndex);
    void ReportLimit(std::size_t scope);

    EditorProject& project_;
    std::array<Track, 8> tracks_;
    std::uint64_t revision_ = 0;
    bool installing_ = false;
};

} // namespace War3Map::QtProject
