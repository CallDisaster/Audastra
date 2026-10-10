#pragma once

#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>

class QColor;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTextEdit;
class QTimer;

namespace War3Map::UiKit {

enum class WarcraftTextSourceState { Available, Unknown, Stale };
struct WarcraftTextSource {
    QString text;
    QString explanation;
    WarcraftTextSourceState state = WarcraftTextSourceState::Unknown;
};
// 只消费宿主已准备的 owned 来源；回调不得读取磁盘、建立快照或发布编辑。
using WarcraftTextSourceResolver = std::function<WarcraftTextSource(const QString&)>;

// 所有正文预览共用此组件；宿主仍拥有编码、WTS 来源和 Apply/Cancel 事务。
class WarcraftTextPreview final : public QWidget {
public:
    explicit WarcraftTextPreview(QWidget* parent = nullptr);
    void Bind(QPlainTextEdit* editor, const QString& sourceExplanation = {}, bool sourceAvailable = true,
        WarcraftTextSourceResolver resolver = {});
    void Bind(QLineEdit* editor, const QString& sourceExplanation = {}, bool sourceAvailable = true,
        WarcraftTextSourceResolver resolver = {});
    void Refresh();
    // 接收已确认的用户取色；只插入当前草稿，一次原编辑框 undo，不提交字段。
    [[nodiscard]] bool InsertColour(const QColor& colour);
    [[nodiscard]] bool InsertReset();
    [[nodiscard]] bool InsertNewline();
    [[nodiscard]] QTextEdit* Preview() const { return preview_; }
    [[nodiscard]] QString StatusText() const;

private:
    void ResetBinding(const QString& sourceExplanation, bool sourceAvailable);
    void Schedule();
    void ChooseColour();
    [[nodiscard]] bool CanInsert() const;
    [[nodiscard]] bool Insert(const QString& text);

    QPointer<QPlainTextEdit> multiline_;
    QPointer<QLineEdit> singleline_;
    QTimer* timer_ = nullptr;
    QTextEdit* preview_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* source_ = nullptr;
    QPushButton* colour_ = nullptr;
    QPushButton* reset_ = nullptr;
    QPushButton* newline_ = nullptr;
    bool sourceAvailable_ = true;
    bool rendered_ = false;
    std::uint64_t generation_ = 0;
    WarcraftTextSourceResolver sourceResolver_;
};

} // namespace War3Map::UiKit
