#pragma once

#include "Modules/EditorModule.h"
#include "War3MapCore/MapData/MapAuxiliary.h"

#include <QObject>
#include <QPointer>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QAudioOutput;
class QBuffer;
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMediaPlayer;
class QPushButton;
class QTableWidget;
class QToolButton;
class QWidget;

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::Modules {
class SoundPreviewPlayer;

// war3map.w3s 的真实定义编辑器。资源文件、声音定义和播放实例分别建模；
// 文档修改只经过工程持有的 MapDataEditSession，候选地图由统一保存服务产生。
class SoundsModule final : public QObject, public Shell::EditorModule {
    Q_OBJECT
public:
    explicit SoundsModule(QtProject::EditorProject& project);
    ~SoundsModule() override;

    [[nodiscard]] Shell::ModuleInfo Info() const override;
    [[nodiscard]] QWidget* Widget() override;
    void AttachHost(Shell::ModuleHost* host) override;
    [[nodiscard]] bool Reveal(const Shell::NavigationTarget& target) override;
    [[nodiscard]] QJsonObject SaveViewState() const override;
    void RestoreViewState(const QJsonObject& state) override;
    std::optional<QJsonObject> CaptureNavigationBookmark() const override;
    Shell::NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& bookmark) override;
    bool PrepareNavigationLeave() override;
    [[nodiscard]] QUndoStack* UndoStack() override;

    // 供非模态属性区与回归测试使用。失败时文档和撤销栈均不变化。
    [[nodiscard]] Status EditField(std::size_t row, std::string_view field, std::string_view value);
    [[nodiscard]] Status AddDefinition(std::string name, std::string filePath);
    [[nodiscard]] Status DuplicateDefinition(std::size_t row);
    [[nodiscard]] bool HasPendingDraft() const noexcept { return draftChanged_; }
    [[nodiscard]] bool ApplyPendingDraft();
    void DiscardPendingDraft();
    void Refresh(bool discardDraft = false);

signals:
    void DraftChanged(bool pending);

private:
    struct ResourceRow {
        QString path;
        QString source;
        QString diskPath;
        QString archivePath;
        bool game = false;
    };

    void BuildPage();
    void StartGameIndex();
    void RebuildResources();
    [[nodiscard]] std::vector<ResourceRow> ResourceCatalogRows() const;
    void RebuildDefinitions();
    void LoadInspector();
    void SetDraftChanged();
    [[nodiscard]] Status ApplyInspector();
    [[nodiscard]] Status CommitFields(std::size_t row,
        const std::vector<std::pair<std::string, std::string>>& values,
        const MapData::SoundsDocument* additional = nullptr);
    [[nodiscard]] Status Commit(const MapData::SoundsDocument& document,
        const std::vector<std::byte>& before, const QString& description);
    [[nodiscard]] Status ApplySerialized(const std::vector<std::byte>& expected,
        const std::vector<std::byte>& replacement);
    void ShowStatus(const QString& message, bool error = false) const;
    void StartPreview();
    void PlayBytes(std::vector<std::byte> bytes, const QString& path);
    void StopPreview();

    QtProject::EditorProject& project_;
    Shell::ModuleHost* host_ = nullptr;
    QPointer<QWidget> page_;
    QTableWidget* resourceTable_ = nullptr;
    QTableWidget* definitionsTable_ = nullptr;
    QCheckBox* showRawValues_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* apply_ = nullptr;
    QGroupBox* spatial_ = nullptr;
    std::vector<ResourceRow> resourceRows_;
    std::vector<ResourceRow> gameDirectoryRows_;
    std::optional<MapData::SoundsDocument> visibleSounds_;
    std::vector<QLineEdit*> fields_;
    std::vector<QToolButton*> defaultButtons_;
    std::vector<QCheckBox*> flags_;
    SoundPreviewPlayer* previewPlayer_ = nullptr;
    std::filesystem::path gameIndexRoot_;
    std::uint64_t gameIndexGeneration_ = 0;
    std::uint64_t previewGeneration_ = 0;
    bool gameIndexPending_ = false;
    bool gameIndexLoaded_ = false;
    bool previewPending_ = false;
    std::vector<std::byte> selectedBytes_;
    std::uint64_t selectedSessionIdentity_ = 0;
    std::uint64_t selectedRevision_ = 0;
    std::array<std::string, 3> originalTexts_{};
    std::array<bool, 3> localText_{};
    std::array<bool, 3> opaqueText_{};
    bool loading_ = false;
    bool draftChanged_ = false;
    int selectedDefinition_ = -1;
};

} // namespace War3Map::Modules
