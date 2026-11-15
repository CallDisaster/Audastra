#pragma once

#include "War3MapCore/Feedback/ErrorReport.h"

#include <QDialog>

#include <functional>

class QLabel;
class QPlainTextEdit;

namespace War3Map::QtFeedback {

// 导出错误报告：填写经过、预览报告内容、选择位置导出 ZIP。内容由宿主按当时状态收集。
class ErrorReportDialog final : public QDialog {
    Q_OBJECT
public:
    using Collect = std::function<Feedback::ErrorReportInput()>;
    ErrorReportDialog(Collect collect, QString logDirectory, QWidget* parent = nullptr);
    // 不弹文件对话框，直接导出到 target（测试与自动化用）。
    [[nodiscard]] bool ExportTo(const QString& target, QString* error = nullptr);
private:
    void Refresh();
    void Export();
    Collect collect_;
    QString logDirectory_;
    Feedback::ErrorReportInput input_;
    QPlainTextEdit* description_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace War3Map::QtFeedback
