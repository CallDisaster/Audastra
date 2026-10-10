#pragma once

#include "Modules/EditorModule.h"
#include <QObject>
#include <QPointer>

class VariablesDialog;
class QLabel;
class QVBoxLayout;
namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::Modules {

// 独立变量工作区：与触发器共享工程文档/撤销栈，不依赖 MainWindow。
class VariablesModule final : public QObject, public Shell::EditorModule {
public:
    explicit VariablesModule(QtProject::EditorProject& project);
    ~VariablesModule() override;
    Shell::ModuleInfo Info() const override;
    QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    bool Reveal(const Shell::NavigationTarget& target) override;
    QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) override;
    QUndoStack* UndoStack() override;
    void Refresh();

private:
    void ReleaseEditor();
    QtProject::EditorProject& project_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QPointer<VariablesDialog> editor_;
    QPointer<QLabel> empty_;
    QVBoxLayout* layout_ = nullptr;
    const void* boundDocument_ = nullptr;
    QPointer<QUndoStack> boundUndoStack_;
    QJsonObject pendingViewState_;
};

} // namespace War3Map::Modules
