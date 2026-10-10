#pragma once

#include "Modules/EditorModule.h"
#include "War3MapCore/MapData/MapSettings.h"

#include <QObject>
#include <QPointer>

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::Modules {

// 经典 W3I v25 的地图设置工作区。页面仅持有草稿；应用时经 MapDataEditSession
// 检查并写入成员覆盖，Shell 的撤销入口使用工程共用的地图数据撤销栈。
class MapSettingsModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT
public:
    explicit MapSettingsModule(QtProject::EditorProject& project);
    ~MapSettingsModule() override;

    Shell::ModuleInfo Info() const override;
    QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    bool Reveal(const Shell::NavigationTarget& target) override;
    QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& bookmark) override;
    bool PrepareNavigationLeave() override;
    QUndoStack* UndoStack() override;

    [[nodiscard]] bool ApplyDraft();
    void DiscardDraft();
    // 同一会话内有未应用草稿时只核对来源：设置被其他操作改动则标为过期，保留输入且拒绝应用；
    // 否则重新载入。切换会话、放弃和应用成功后总是重新载入。
    void Refresh();
    [[nodiscard]] bool HasPendingDraft() const noexcept { return pending_; }
    [[nodiscard]] bool DraftIsStale() const noexcept { return pending_ && stale_; }

signals:
    void SettingsEdited();
    void DraftChanged(bool pending);

private:
    enum class Encoding { Utf8, Local8Bit, Opaque };
    struct TextField {
        QWidget* editor = nullptr;
        std::string* target = nullptr;
        std::string original;
        Encoding encoding = Encoding::Utf8;
        QString initial;
    };
    struct FieldLabel {
        QLabel* widget = nullptr;
        QString display;
        QString raw;
    };
    struct FlagField {
        QCheckBox* widget = nullptr;
        std::uint32_t mask = 0;
        QString display;
        QString raw;
    };

    void BuildPage();
    void BuildForms();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void UpdateResponsiveLayout();
    void UpdateLabels();
    void MarkPending();
    void Reload();
    void EndDraft();
    void CheckDraftSource();
    void SetMessage(const QString& message, bool error = false);
    void AddTextField(QFormLayout* form, const QString& label, const QString& raw,
        std::string& value, bool multiline = false);
    void AddNumberField(QFormLayout* form, const QString& label, const QString& raw,
        int value, QSpinBox*& output);
    [[nodiscard]] bool ReadDraft(MapData::MapSettings& candidate, QString& error);

    QtProject::EditorProject& project_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QLabel* title_ = nullptr;
    QCheckBox* rawFields_ = nullptr;
    QListWidget* sections_ = nullptr;
    QComboBox* compactSections_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    std::vector<QFormLayout*> forms_;
    QGridLayout* flagsLayout_ = nullptr;
    int flagColumns_ = 0;
    QLabel* message_ = nullptr;
    QPushButton* apply_ = nullptr;
    QPushButton* discard_ = nullptr;
    QSpinBox* loadingBackground_ = nullptr;
    QComboBox* gameDataSet_ = nullptr;
    std::vector<FlagField> mapFlagFields_;
    QTableWidget* players_ = nullptr;
    QTableWidget* forces_ = nullptr;
    std::optional<MapData::MapSettings> draft_;
    std::vector<TextField> textFields_;
    std::vector<FieldLabel> labels_;
    std::uint64_t boundSession_ = 0;
    bool pending_ = false;
    bool stale_ = false;
    bool rebuilding_ = false;
    int pendingSection_ = 0;
    bool pendingRawFields_ = false;
};

} // namespace War3Map::Modules
