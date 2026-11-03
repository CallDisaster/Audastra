#pragma once
#include "War3MapCore/Services/MinimapPreview.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
class QThread;
namespace War3Map::QtProject {
class EditorProject;
struct MinimapPreviewOptions {
    bool current = true;
    std::uint32_t layers = 0x8b;
    bool units = false, starts = false, icons = false;
    bool operator==(const MinimapPreviewOptions&) const = default;
};
// GUI只冻结文档；资源捕获、散列、栅格化和解码均在一个有界worker中执行。
class ProjectMinimapPreview final : public QObject {
    Q_OBJECT
public:
    explicit ProjectMinimapPreview(EditorProject& project, QObject* parent = nullptr);
    ~ProjectMinimapPreview() override;
    void Request(MinimapPreviewOptions options, bool force = false);
    [[nodiscard]] const std::shared_ptr<const Result<Services::MinimapPreview>>& ResultValue() const { return result_; }
    [[nodiscard]] bool Pending() const { return pending_; }
    [[nodiscard]] MinimapPreviewOptions Options() const { return options_; }
signals:
    void Changed();
private:
    void Invalidate();
    void Start();
    EditorProject& project_;
    MinimapPreviewOptions options_;
    bool requested_ = false, pending_ = false;
    std::uint64_t serial_ = 0;
    QTimer debounce_;
    QPointer<QThread> worker_;
    std::shared_ptr<const Result<Services::MinimapPreview>> result_;
};
ProjectMinimapPreview& AgentMinimapPreview(EditorProject& project);
} // namespace War3Map::QtProject
