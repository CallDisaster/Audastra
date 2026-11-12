#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QWidget>

#include <functional>

class QLabel;

namespace War3Map::UiKit::Tokens {
struct ThemeTokens;
}

namespace War3Map::Agent {

// 上下文分段条与图例共用的配色，全部取自主题令牌；图例色块与分段条逐一相同。
struct ContextSegmentColors {
    QColor input;     // 未命中缓存的输入
    QColor cache;     // 缓存命中
    QColor output;    // 模型输出
    QColor remaining; // 剩余容量（底槽）
};
[[nodiscard]] ContextSegmentColors ResolveContextSegmentColors(const UiKit::Tokens::ThemeTokens& theme);

// 调色板或样式变更后重新着色：样式表逐个控件重新抛光，推迟到本轮事件之后才能读到全部生效的主题；
// 同一轮多次变更只着色一次。queued 必须是 context 自己的成员，context 销毁时排队的着色随之取消。
void ScheduleThemeRetint(const QEvent* event, QObject* context, bool& queued, std::function<void()> retint);

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

// 右侧检查栏的彩色分段上下文用量条：按输入、缓存命中、模型输出与剩余容量绘制圆角比例条。
// 已用量中提供商没有分类的部分画成单独的未分类段，不归入任何一项。
class ContextSegmentBar final : public QWidget {
    Q_OBJECT

public:
    explicit ContextSegmentBar(QWidget* parent = nullptr);

    void SetBreakdown(qint64 limit, qint64 used, qint64 uncachedInput, qint64 cachedRead, qint64 output);
    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qint64 limit_ = 0;
    qint64 used_ = -1;
    qint64 uncachedInput_ = 0;
    qint64 cachedRead_ = 0;
    qint64 output_ = 0;
};

// 权限菜单中的一项：图标、标题，右侧勾选；说明只在悬停提示里。仿照 Codex 的批准方式菜单。
class PermissionOption final : public QWidget {
    Q_OBJECT

public:
    PermissionOption(const QIcon& icon, const QString& title, const QString& description, QWidget* parent = nullptr);
    void SetChecked(bool checked);
    void SetWarning(bool warning);

signals:
    void Activated();

protected:
    void changeEvent(QEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void UpdateCheckIcon();

    QLabel* icon_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* check_ = nullptr;
    bool hovered_ = false;
    bool checked_ = false;
    bool retintQueued_ = false;
};

[[nodiscard]] QString FormatTokens(qint64 tokens);

} // namespace War3Map::Agent
