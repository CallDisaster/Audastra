#pragma once

#include "War3MapCore/UiLibrary/UiLibrary.h"

#include <QDialog>

#include <string>
#include <vector>

class QPushButton;
class QTableWidget;
namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::Modules {

// 函数库管理：全局启用 / 顺序（本机设置，不改 config）、本图用到的包及其状态、
// “仅对此地图启用”，以及同名定义冲突与最终生效来源。
class UiLibraryDialog final : public QDialog {
    Q_OBJECT

public:
    UiLibraryDialog(const QtProject::EditorProject& project, QWidget* parent = nullptr);

    [[nodiscard]] UiLibrary::GlobalSettings Settings() const;
    [[nodiscard]] std::vector<std::string> MapOnly() const;

private:
    void Populate();
    void MoveSelected(int delta);
    void UpdateInstallButton();
    void InstallBundled();

    const QtProject::EditorProject& project_;
    UiLibrary::GlobalSettings settings_;
    std::vector<std::string> mapOnly_;
    QTableWidget* packages_ = nullptr;
    QPushButton* installBundled_ = nullptr;
};

} // namespace War3Map::Modules
