#pragma once
#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;
class QTextBrowser;

// 使用帮助：按“## 标题”把帮助文档分节，左侧目录 + 搜索，右侧显示本节。
// 编辑器界面不放说明文字，用法和限制都写在这份帮助里。
class HelpDialog final : public QDialog {
    Q_OBJECT
public:
    explicit HelpDialog(QWidget* parent = nullptr);
    // 显示标题包含 topic 的第一节；找不到时保持当前节。
    void ShowTopic(const QString& topic);
    [[nodiscard]] static QStringList SplitSections(const QString& markdown);

private:
    void Filter(const QString& text);
    void ShowSection(int row);

    QLineEdit* search_ = nullptr;
    QListWidget* topics_ = nullptr;
    QTextBrowser* page_ = nullptr;
    QStringList sections_;
};
