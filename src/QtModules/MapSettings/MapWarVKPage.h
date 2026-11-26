#pragma once
#include "War3MapCore/MapData/WarVKMapSettings.h"
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::Modules {
// 地图设置“WarVK”页：镜头限制（镜头远距上限、近裁剪）、指令执行时机、天空模型与远景装饰物类型。
// 每次改动经工程共用的 WarVK 设置命令提交，是地图设置撤销栈上的一条记录；物编装饰物的“远景”勾选改的是同一设置。
class MapWarVKPage final : public QWidget {
public:
    // blocked 返回当前不能提交的原因（如有未应用的地图设置草稿）；report 显示提交失败的原因。
    // 发布会重建地图设置页，提交推迟到事件循环下一轮并以 context 为生命周期。
    MapWarVKPage(QtProject::EditorProject& project, QObject* context, std::function<QString()> blocked,
        std::function<void(const QString&)> report, bool raw, QWidget* parent = nullptr);
    void SetRaw(bool raw);
    void Fill();

private:
    struct Caption { QLabel* widget = nullptr; QString display, raw; };
    void Commit(MapData::WarVKMapSettings next, const QString& label);
    [[nodiscard]] MapData::WarVKMapSettings Edited() const;

    QtProject::EditorProject& project_;
    QObject* context_ = nullptr;
    std::function<QString()> blocked_;
    std::function<void(const QString&)> report_;
    bool raw_ = false;
    MapData::WarVKMapSettings current_;
    std::vector<Caption> captions_;
    QCheckBox* limits_ = nullptr;
    QDoubleSpinBox* distance_ = nullptr;
    QDoubleSpinBox* near_ = nullptr;
    QComboBox* timing_ = nullptr;
    QComboBox* sky_ = nullptr;
    QListWidget* far_ = nullptr;
    QLabel* farCount_ = nullptr;
    QPushButton* removeFar_ = nullptr;
    QLabel* problem_ = nullptr;
};
} // namespace War3Map::Modules
