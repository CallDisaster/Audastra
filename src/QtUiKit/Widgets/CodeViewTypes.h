#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>

namespace War3Map::UiKit {

enum class CodeLanguage : std::uint8_t { Jass, VJass, Lua };
enum class CodeSeverity : std::uint8_t { Information, Warning, Error };

struct CodeDiagnostic {
    CodeSeverity severity = CodeSeverity::Error;
    QString source, code, message;
    int line = 0; // 1 基；0 表示没有可靠位置。
    int column = 1; // 当前源的 UTF-16 列；外部源只供列表导航。
    int length = 0;
    bool originMapped = false;
    bool byteRangeExact = false;
};

struct CodeCompletionItem {
    QString name, kind, signature, source, documentation;
    int line = 0, column = 1;
};
struct CodeExternalSource { QString name, text, reason; QByteArray bytes; };
struct CodeSemanticDefinition {
    std::uint64_t identity = 0;
    QString name, kind, signature, source;
    int line = 0, column = 1, length = 0;
    bool exact = false;
};
struct CodeSemanticReference {
    std::uint64_t target = 0;
    QString source, use;
    int line = 0, column = 1, length = 0;
    bool targetKnown = false, exact = false;
};

struct CodeCheckResult {
    std::uint64_t generation = 0;
    bool completed = false, syntaxChecked = false, semanticChecked = false;
    bool contextComplete = false, cancelled = false;
    bool externalSourcesComplete = true;
    QString reason, contextBinding;
    QVector<CodeDiagnostic> diagnostics;
    QVector<CodeCompletionItem> declarations;
    QVector<CodeExternalSource> externalSources;
    bool semanticIndexComplete = false;
    QString semanticIndexReason;
    QVector<CodeSemanticDefinition> semanticDefinitions;
    QVector<CodeSemanticReference> semanticReferences;
    // 后台宿主附加拥有的项目索引；UiKit只保活，不依赖或解释Core类型。
    std::shared_ptr<const void> ownedProjectIndex;
};

struct CodeDecodedSource {
    QString text;
    bool complete = true;
    QString reason;
};

struct CodeWorkerStats {
    int running = 0, pending = 0, peakRunning = 0, peakPending = 0;
    std::uint64_t submitted = 0, adopted = 0, discarded = 0;
};

// PrepareCheck 在 UI 线程捕获完整、拥有的快照，返回不依赖控件/实时工程的后台函数。
using CodeCheckTask = std::function<CodeCheckResult(std::stop_token)>;
using PrepareCodeCheck = std::function<CodeCheckTask(const QString&, std::uint64_t)>;
using DecodeCodeBytes = std::function<CodeDecodedSource(const QByteArray&, std::stop_token)>;

} // namespace War3Map::UiKit
