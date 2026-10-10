#pragma once

#include <QIcon>
#include <QString>
#include <QWidget>

class QLabel;

namespace War3Map::Agent {

// 输入框右下角的上下文用量圆环：灰色底环是总上限，亮色弧是已用部分。
// 用量未知时只画底环；上限是估算值时提示里注明。
class ContextRing final : public QWidget {
    Q_OBJECT

public:
    explicit ContextRing(QWidget* parent = nullptr);

    void SetLimit(qint64 tokens, bool estimated, const QString& source);
    void SetUsed(qint64 tokens);
    void ClearUsed();
    [[nodiscard]] double Fraction() const noexcept;
    [[nodiscard]] qint64 Limit() const noexcept { return limit_; }
    [[nodiscard]] qint64 Used() const noexcept { return used_; }
    [[nodiscard]] bool IsEstimated() const noexcept { return estimated_; }
    [[nodiscard]] QString Source() const { return source_; }
    [[nodiscard]] QSize sizeHint() const override;

signals:
    void Activated();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void UpdateToolTip();

    qint64 limit_ = 0;
    qint64 used_ = -1;
    bool estimated_ = true;
    QString source_;
    bool pressed_ = false;
};

// 权限菜单中的一项：图标、标题、说明，右侧勾选。仿照 Codex 的批准方式菜单。
class PermissionOption final : public QWidget {
    Q_OBJECT

public:
    PermissionOption(const QIcon& icon, const QString& title, const QString& description, QWidget* parent = nullptr);
    void SetChecked(bool checked);
    void SetWarning(bool warning);

signals:
    void Activated();

protected:
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* icon_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* check_ = nullptr;
    bool hovered_ = false;
};

[[nodiscard]] QString FormatTokens(qint64 tokens);

} // namespace War3Map::Agent
