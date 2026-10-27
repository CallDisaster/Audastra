#pragma once
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Services/MapCustomSettingsEditing.h"
#include <QWidget>
#include <functional>
class QLabel;
class QPushButton;
class QTableWidget;
namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::Modules {
// 地图设置“科技与随机表”页：升级可用性与科技限制按行列出（对象头像+名称，玩家，等级，状态），
// 添加/修改/删除各是一条带 CAS 的共用服务命令。随机表在初始化接通前只显示摘要。
class MapTechPage final : public QWidget {
public:
    // 返回空表示命令已准备好并将提交；否则为原因，调用方保留表单。
    using Commit = std::function<QString(War3Map::Services::MapCustomSettingsCommand, const QString& label)>;
    // readOnly 非空时只列出，不提供添加/修改/删除。
    MapTechPage(QtProject::EditorProject& project, const MapData::MapSettings& settings, bool raw,
        const QString& readOnly, Commit commit, QWidget* parent = nullptr);
    void SetRaw(bool raw);

    // 宿主的提交：blocked 给出当前不能编辑的原因（如有未应用草稿）；准备与校验同步完成并把原因返回表单，
    // 实际发布推迟到事件循环下一轮（发布会重建本页），发布失败经 report 显示。
    [[nodiscard]] static Commit ProjectCommit(QtProject::EditorProject& project, QObject* context,
        std::function<QString()> blocked, std::function<void(const QString&)> report);

private:
    void Fill();
    void EditRow(War3Map::Services::MapCustomSettingsSection section, int row);
    void DeleteRow(War3Map::Services::MapCustomSettingsSection section);
    [[nodiscard]] QString PlayersText(std::uint32_t mask) const;
    [[nodiscard]] QString ObjectText(const MapData::SettingsRawcode& id, QIcon* icon, QString* tooltip) const;

    QtProject::EditorProject& project_;
    MapData::MapSettings settings_;
    bool raw_ = false;
    bool editable_ = true;
    Commit commit_;
    QTableWidget* upgrades_ = nullptr;
    QTableWidget* restrictions_ = nullptr;
    QPushButton* deleteUpgrade_ = nullptr;
    QPushButton* deleteRestriction_ = nullptr;
    QLabel* problem_ = nullptr;
};
} // namespace War3Map::Modules
