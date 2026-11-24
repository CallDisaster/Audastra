#pragma once

#include <QWidget>
#include <QString>

class QComboBox;

namespace War3Map::QtApp {

// 只编辑草稿；持久化仍走应用设置提交事务。
class WorldEffectsSettingsPage final : public QWidget {
public:
    explicit WorldEffectsSettingsPage(QWidget* parent = nullptr);
    [[nodiscard]] QString QualityName() const;
    // 选定显卡的跨重启键（空 = 跟随系统）与显示名。
    [[nodiscard]] QString RenderAdapterKey() const;
    [[nodiscard]] QString RenderAdapterName() const;
private:
    QComboBox* quality_ = nullptr;
    QComboBox* adapter_ = nullptr;
};

} // namespace War3Map::QtApp
