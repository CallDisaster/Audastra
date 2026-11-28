#pragma once

#include "ProjectSurfaceMaterials.h"
#include "War3MapCore/Render/SurfaceMaterialTable.h"
#include "Widgets/ObjectResourcePicker.h"

#include <QWidget>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QAbstractButton;
class QButtonGroup;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace War3Map::Modules {
// 世界页“材质”面板：地图 WarVK\materials.ini 的类型列表与单个类型的编辑。读写、校验都走
// ProjectSurfaceMaterials（与 Agent 的 world_surface_material_* 命令同一套），每次修改是资源撤销栈上的一步；
// 校验在后台线程做（界面线程只读冻结的表、类型与图片）。
class WorldSurfaceMaterialPanel final : public QWidget {
    Q_OBJECT
public:
    explicit WorldSurfaceMaterialPanel(QtProject::EditorProject& project, QWidget* parent = nullptr);
    ~WorldSurfaceMaterialPanel() override;
    void SetResourcePickerFactory(ObjectResourcePickerFactory factory) { pickerFactory_ = std::move(factory); }
    // 世界里选中装饰物或可破坏物时跟到它的类型（列表里没有这一类型时不动）。
    void FollowType(const QString& rawcode);
    [[nodiscard]] QString CurrentType() const { return current_; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct FieldRow {
        QLabel* label = nullptr;
        QToolButton* reset = nullptr;
        QLineEdit* path = nullptr;          // 图片
        QButtonGroup* choice = nullptr;     // 二选一
        QSlider* slider = nullptr;          // 数值
        QDoubleSpinBox* spin = nullptr;     // 数值
    };
    void Invalidate();
    void Reload();
    void StartValidation();
    void RebuildList();
    void ShowType();
    void ShowDiagnostics();
    [[nodiscard]] std::optional<Services::SurfaceMaterialSection> CurrentSection() const;
    // 在当前表上改当前类型并作为一步撤销提交；失败时在面板上说明原因，返回 false。
    // rawcode 是改动所属的类型（由调用方在打开对话框或选择器之前记下，不随面板当前类型变）。
    bool Commit(QString rawcode, const QString& label,
        const std::function<Status(Services::SurfaceMaterialTableDocument&)>& change,
        std::vector<QtProject::EditorProject::ImportPayload> images = {}, bool replaceImages = false);
    bool SetValue(QString rawcode, std::size_t key, const std::optional<QString>& value);
    void CommitNumber(std::size_t key);
    void ChooseImage(std::size_t key);
    void ImportImage(std::size_t key);
    void RemoveCurrent();
    [[nodiscard]] double SliderValue(std::size_t key, int position) const;
    [[nodiscard]] int SliderPosition(std::size_t key, double value) const;
    void ShowMessage(const QString& text, bool error);

    QtProject::EditorProject& project_;
    ObjectResourcePickerFactory pickerFactory_;
    std::optional<QtProject::SurfaceMaterialTableState> state_;
    std::shared_ptr<const QtProject::SurfaceMaterialSnapshot> snapshot_;
    std::shared_ptr<const Services::SurfaceMaterialValidation> validation_;
    std::shared_ptr<bool> alive_;
    std::uint64_t generation_ = 0;
    bool dirty_ = true;
    bool syncing_ = false;
    QString current_;
    QTimer* reloadTimer_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* filter_ = nullptr;
    QTreeWidget* types_ = nullptr;
    QWidget* editor_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* meta_ = nullptr;
    QLabel* message_ = nullptr;
    QListWidget* diagnostics_ = nullptr;
    QPushButton* remove_ = nullptr;
    std::array<FieldRow, Render::kSurfaceMaterialKeys.size()> fields_{};
};
} // namespace War3Map::Modules
