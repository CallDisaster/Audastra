#pragma once

#include "GameUiModule.h"

#include <QFrame>
#include <QList>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTreeWidget;

namespace War3Map::Modules {

// 右键“创建界面”的类型选取：左侧按用途分组的类型，右侧预览、作用、用法与可换项；模板类型改为搜索模板。
// 放进哪个描述或框架可改。选定后发出 Chosen，由工作区创建；本身不改地图。
class GameUiKindPicker final : public QFrame {
    Q_OBJECT
public:
    struct Target {
        QString label;
        QString description;
        QString frame;  // 空为描述顶层
    };
    GameUiKindPicker(GameUiModule& module, QList<Target> targets, int current, QWidget* parent = nullptr);

    // 当前所选类型与模板名（测试与助手用）。
    [[nodiscard]] QString Kind() const;
    bool SelectKind(const QString& kind);
    void Accept();

signals:
    void Chosen(const QString& kind, const QString& templateName, const QString& description, const QString& frame);

private:
    void ShowKind();
    void ShowPreview(const GameUi::UiFrame& frame);
    void FilterTemplates(const QString& text);

    GameUiModule& module_;
    QList<Target> targets_;
    GameUiModule::FramePreview preview_;
    QTreeWidget* kinds_ = nullptr;
    GameUiCanvas* canvas_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* purpose_ = nullptr;
    QLabel* usage_ = nullptr;
    QLabel* slots_ = nullptr;
    QStackedWidget* detail_ = nullptr;
    QLineEdit* templateSearch_ = nullptr;
    QListWidget* templates_ = nullptr;
    QComboBox* target_ = nullptr;
    QPushButton* create_ = nullptr;
};

} // namespace War3Map::Modules
