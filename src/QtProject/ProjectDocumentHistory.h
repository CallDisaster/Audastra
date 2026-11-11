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

namespace War3Map::Trigger { struct EncodedTriggerDocument; }

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
    [[nodiscard]] bool NeedsCapture(std::size_t scope) const;
    static std::optional<std::string> TriggerFingerprint(const EditorProject& project,
        Trigger::EncodedTriggerDocument* bytes = nullptr);
    void RecordTrigger(const Trigger::EncodedTriggerDocument* bytes);
    void RecordObject(std::size_t type, const std::vector<std::byte>* bytes);
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

    struct Encoding {
        const Trigger::EncodedTriggerDocument* trigger = nullptr;
        const std::vector<std::byte>* object = nullptr;
    };
    [[nodiscard]] std::optional<State> Capture(std::size_t scope, Encoding bytes) const;
    [[nodiscard]] std::optional<QByteArray> Context(std::size_t scope) const;
    [[nodiscard]] bool MatchesStack(const Track& track) const;
    [[nodiscard]] bool ContextMatches(std::size_t scope, const State& state) const;
    void Attach(std::size_t scope, QUndoStack* stack);
    void Observe(std::size_t scope, Encoding bytes, bool refresh = false);
    void EnforceBudget(std::size_t keepScope, std::size_t keepIndex);
    void ReportLimit(std::size_t scope);

    EditorProject& project_;
    std::array<Track, 8> tracks_;
    QMetaObject::Connection mapConnection_;
    const QUndoCommand* firstMapCommand_ = nullptr;
    std::uint64_t revision_ = 0;
    bool installing_ = false;
};

} // namespace War3Map::QtProject
