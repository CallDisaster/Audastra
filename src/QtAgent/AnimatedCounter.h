#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QTimer>

namespace War3Map::Agent {

// 数值动效只影响显示；Value() 在完成后精确等于 TargetValue()。
class AnimatedCounter final : public QLabel {
    Q_OBJECT

public:
    explicit AnimatedCounter(QWidget* parent = nullptr);
    [[nodiscard]] qint64 Value() const noexcept;
    [[nodiscard]] qint64 TargetValue() const noexcept;
    [[nodiscard]] bool IsAnimating() const noexcept;
    void SetTarget(qint64 target);
    void SetValue(qint64 value);
    void SetReducedMotion(bool reduced);
    void Finish();
    void SetMinimumDigits(int digits);

signals:
    void ValueChanged(qint64 value);

private:
    void Advance();
    void Display(qint64 value);
    void ReserveWidth(qint64 value);

    QTimer timer_;
    QElapsedTimer elapsed_;
    QElapsedTimer frameElapsed_;
    qint64 value_ = 0;
    qint64 target_ = 0;
    bool reducedMotion_ = false;
    int reservedDigits_ = 6;
};

} // namespace War3Map::Agent
