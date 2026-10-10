#pragma once

#include "WorldThumbnailCache.h"
#include "../Objects/ObjectViewEntries.h"

#include <QWidget>
#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;
class QTabBar;
class QGridLayout;
class QListWidgetItem;

namespace War3Map::Modules {

struct WorldAssetDrag {
    WorldAssetBinding binding;
    ObjectData::ObjectKey object;
};

struct WorldAssetFile {
    QString path, source;
    qint64 size = 0;
};

class WorldAssetBrowser final : public QWidget {
    Q_OBJECT
public:
    using ModelResolver = std::function<std::optional<Render::ModelInstanceRequest>(ObjectData::ObjectType, const std::string&)>;
    explicit WorldAssetBrowser(QtApp::IconProvider* icons, const War3RenderPluginApiV2* backend, QWidget* parent = nullptr,
        bool loadProductionBackend = false);
    void SetContext(QtProject::EditorProject& project, WorldAssetBinding binding,
        Result<WorldTerrainSources> sources, std::function<bool()> current, ModelResolver resolve);
    // 头像目录不读取地形来源；常规已 Observe 的非目录编辑只更新交互修订，不清列表。
    void SetContext(QtProject::EditorProject& project, WorldAssetBinding binding, std::function<bool()> current);
    void Clear();
    void SetCollapsed(bool value);
    [[nodiscard]] bool Collapsed() const;
    void SetVerticalDock(bool vertical);
    void SetCompact(bool compact);
    [[nodiscard]] bool Compact() const noexcept { return compact_; }
    [[nodiscard]] static Result<WorldAssetDrag> ParseDrag(const QByteArray& bytes);
    [[nodiscard]] static constexpr const char* DragMimeType() { return "application/x-forge-world-asset-v1"; }
signals:
    void PlacementRequested(const War3Map::QtApp::ObjectViewEntry& entry, const War3Map::Modules::WorldAssetBinding& binding);
    void DefinitionRequested(War3Map::ObjectData::ObjectType type, const QString& rawcode, const War3Map::Modules::WorldAssetBinding& binding);
    void FileRequested(const QString& path, const War3Map::Modules::WorldAssetBinding& binding);
    void CollapsedChanged(bool collapsed);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
private:
    struct ContentContext;
    [[nodiscard]] static std::shared_ptr<const ContentContext> CaptureContentContext(const QtProject::EditorProject& project);
    void Reload();
    void LoadBatch(std::uint64_t queryGeneration);
    void RefreshFilters();
    void ApplyLayout();
    void UpdateItemLayout(QListWidgetItem& item);
    bool Current() const;
    QPointer<QtProject::EditorProject> project_;
    QtApp::IconProvider* icons_ = nullptr;
    WorldAssetBinding binding_;
    std::function<bool()> current_;
    std::shared_ptr<const ContentContext> contentContext_;
    std::uint64_t observedEditRevision_ = 0;
    QWidget* content_ = nullptr;
    QTabBar* pages_ = nullptr;
    QToolButton* collapse_ = nullptr;
    QToolButton* compactToggle_ = nullptr;
    QGridLayout* filters_ = nullptr;
    QComboBox* kind_ = nullptr;
    QComboBox* source_ = nullptr;
    QComboBox* group_ = nullptr;
    QComboBox* catalogSet_ = nullptr;
    QComboBox* category_ = nullptr;
    QLineEdit* search_ = nullptr;
    QListWidget* rows_ = nullptr;
    QLabel* status_ = nullptr;
    QVector<QtApp::ObjectViewRow> objects_;
    std::vector<WorldAssetFile> files_;
    std::size_t cursor_ = 0;
    std::uint64_t queryGeneration_ = 0;
    QString queryIssue_;
    bool filesTruncated_ = false;
    bool verticalDock_ = false, compact_ = false, compactChosen_ = false, grouped_ = false, sectionStarted_ = false;
    int section_ = 0;
    QPoint press_;
};

} // namespace War3Map::Modules

Q_DECLARE_METATYPE(War3Map::Modules::WorldAssetBinding)
