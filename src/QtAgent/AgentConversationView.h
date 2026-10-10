#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QScrollArea>

#include <memory>

class QTextDocument;

namespace War3Map::Agent {

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
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Private;
    std::unique_ptr<Private> d_;
    void ScheduleLayoutDiagnostics();
    void InspectLayoutDiagnostics();
};

} // namespace War3Map::Agent
