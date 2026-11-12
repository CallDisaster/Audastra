#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QScrollArea>

#include <memory>

class QTextDocument;

namespace War3Map::Agent {

// 工具调用状态的唯一分类：会话工具行与检查栏的圆点、计数共用。“已停止”“未收到结果”不算成功。
enum class ToolStateKind { Running, Ok, Error, Stopped, UnknownResult, Done };
inline constexpr int kToolStateKindCount = 6;
[[nodiscard]] ToolStateKind ClassifyToolState(const QString& state);
// 样式表 stateKind / statusKind 属性值：running / ok / error / stopped / unknown / done。
[[nodiscard]] QString ToolStateKindName(ToolStateKind kind);
// 工具调用的简短操作名：已知工具映射为中文操作名，过长的工具说明退回工具名；会话行与检查栏共用。
[[nodiscard]] QString ShortToolLabel(const QJsonObject& event);

// DSH 的单列会话/行内 disclosure 在 Qt 的原生承接；只呈现事件，不执行工具。
class AgentConversationView final : public QScrollArea {
    Q_OBJECT
public:
    explicit AgentConversationView(QWidget* parent = nullptr);
    ~AgentConversationView() override;
    void BeginMessage(const QString& speaker, bool user = false);
    void AppendText(const QString& text);
    void ReplaceText(const QString& text);
    void EndMessage();
    void UpdateTool(const QJsonObject& event, const QString& state, const QString& duration);
    void AppendChanges(const QJsonArray& items);
    void SetReducedMotion(bool reduced);
    void SetContentWidth(int width);
    void Clear();
    [[nodiscard]] QString toPlainText() const;
    [[nodiscard]] QTextDocument* ActiveDocument() const;
    [[nodiscard]] int ToolCount() const;
    [[nodiscard]] int ChangeCount() const;
    // 诊断默认写当前用户本地目录；测试/宿主可在采样前注入独立文件。
    void SetLayoutDiagnosticPath(const QString& path);
    [[nodiscard]] QString LayoutDiagnosticError() const;

signals:
    void ContentsChanged();
    void NavigateRequested(QString kind, QString objectId);
    // 空白页的常用任务；只填入输入框，由作者确认后再发送。
    void SuggestionChosen(QString text);
    void LayoutDiagnosticFailed(QString code);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Private;
    std::unique_ptr<Private> d_;
    void ScheduleLayoutDiagnostics();
    void InspectLayoutDiagnostics();
};

} // namespace War3Map::Agent
