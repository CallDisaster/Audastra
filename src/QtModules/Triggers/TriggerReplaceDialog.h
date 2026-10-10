#pragma once

#include "War3MapCore/Services/TriggerReplace.h"

#include <QDialog>
#include <QFuture>
#include <QFutureWatcher>

#include <functional>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QHideEvent;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace War3Map::Modules {

// 只保存自有查询页、稳定勾选身份与共享服务密封的预览；不读文件或直接编辑 IR。
class TriggerReplaceDialog final : public QDialog {
public:
    using Query = std::function<Result<Services::TriggerReplacePage>(std::string_view,
        const Services::TriggerReplaceQuery&, const Services::TriggerReplaceLimits&,
        const Services::TriggerReplaceCursor*)>;
    using Options = std::function<Result<Services::TriggerReplacementOptionsPage>(std::string_view,
        Services::TriggerReplacementKind, std::string_view, const Services::TriggerReplaceLimits&,
        const Services::TriggerReplaceCursor*)>;
    using Preview = std::function<Result<Services::TriggerReplacePreviewResult>(std::string_view,
        const Services::TriggerReplaceQuery&, const Services::TriggerReplaceQueryProof&,
        std::span<const std::string>, const Services::TriggerReplacement&, const Services::TriggerReplaceLimits&)>;
    using Apply = std::function<Status(std::string_view, const Services::TriggerReplacePreview&,
        const Services::TriggerReplaceLimits&)>;
    // 仅诊断实际调度/后台时间；不参与来源、类型或写入授权，-1 为未完成/未知。
    struct PreviewTiming {
        std::int64_t planNanoseconds = 0, cloneNanoseconds = 0;
        std::int64_t queuedAtNanoseconds = 0;
        std::atomic<std::int64_t> workerNanoseconds{-1};
        std::atomic<std::int64_t> queueNanoseconds{-1}, deliveryNanoseconds{-1}, finishedAtNanoseconds{0};
    };
    struct PreviewTask {
        QFuture<Result<Services::TriggerReplacePreviewResult>> future;
        std::shared_ptr<std::stop_source> stop;
        std::shared_ptr<PreviewTiming> timing{};
    };
    using AsyncPreview = std::function<Result<PreviewTask>(std::string_view,
        const Services::TriggerReplaceQuery&, const Services::TriggerReplaceQueryProof&,
        std::span<const std::string>, const Services::TriggerReplacement&, const Services::TriggerReplaceLimits&)>;
    TriggerReplaceDialog(std::function<std::string()> revision, std::function<Status()> prepareContext, Query query, Options options,
        Preview preview, Apply apply, std::function<void(const Services::TriggerReplaceHit&)> activate,
        QWidget* parent = nullptr);
    ~TriggerReplaceDialog() override;
    void SetCurrentScope(std::optional<StableId> id, const QString& name);
    void ContextChanged();
    void FocusQuery();
    void SetAsyncPreview(AsyncPreview preview);
    void CancelPreview();
    [[nodiscard]] bool PreviewPending() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreviewTiming> LastPreviewTiming() const noexcept;

protected:
    void hideEvent(QHideEvent* event) override;

private:
    void ResetQuery();
    void ResetTargets();
    void InvalidatePreview();
    void Search(bool reset);
    void SearchTargets(bool reset);
    void Populate(const Services::TriggerReplacePage& page);
    void PopulateTargets(const Services::TriggerReplacementOptionsPage& page);
    void CheckCurrentPage(bool checked);
    void ActivateCurrent();
    void PreparePreview();
    void PresentPreview(const Result<Services::TriggerReplacePreviewResult>& result,
        const Services::TriggerReplaceQueryProof& proof);
    void ApplyPreview();
    [[nodiscard]] Services::TriggerReplaceQuery ReadQuery() const;
    [[nodiscard]] std::optional<Services::TriggerReplacement> Replacement() const;
    void UpdateSelectionText();

    std::function<std::string()> revision_;
    std::function<Status()> prepareContext_;
    Query query_;
    Options options_;
    Preview previewCallback_;
    AsyncPreview asyncPreview_;
    QFutureWatcher<Result<Services::TriggerReplacePreviewResult>>* previewWatcher_ = nullptr;
    std::shared_ptr<std::stop_source> previewStop_;
    std::shared_ptr<PreviewTiming> previewTiming_;
    bool previewStarting_ = false;
    Apply apply_;
    std::function<void(const Services::TriggerReplaceHit&)> activate_;
    Services::TriggerReplaceLimits limits_;
    QComboBox* scope_ = nullptr;
    QComboBox* mode_ = nullptr;
    QComboBox* match_ = nullptr;
    QCheckBox* caseSensitive_ = nullptr;
    QLineEdit* queryEdit_ = nullptr;
    QComboBox* replacementKind_ = nullptr;
    QLineEdit* targetFilter_ = nullptr;
    QComboBox* targets_ = nullptr;
    QTreeWidget* results_ = nullptr;
    QTreeWidget* previewRows_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* targetStatus_ = nullptr;
    QLabel* selectionStatus_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* targetPrevious_ = nullptr;
    QPushButton* targetNext_ = nullptr;
    QPushButton* previewButton_ = nullptr;
    QPushButton* cancelPreviewButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    std::vector<Services::TriggerReplaceHit> rows_;
    std::vector<Services::TriggerReplacementOption> targetRows_;
    std::set<std::string> selected_;
    std::optional<Services::TriggerReplaceQueryProof> proof_;
    Services::TriggerReplaceQuery boundQuery_;
    std::optional<Services::TriggerReplacePreview> preview_;
    std::vector<std::optional<Services::TriggerReplaceCursor>> starts_, targetStarts_;
    std::optional<Services::TriggerReplaceCursor> nextCursor_, targetNextCursor_;
    std::size_t pageIndex_ = 0, targetPageIndex_ = 0;
    std::string boundRevision_, targetRevision_, boundTargetFilter_;
    Services::TriggerReplacementKind boundTargetKind_ = Services::TriggerReplacementKind::Variable;
    std::uint64_t requestGeneration_ = 0;
    bool updatingRows_ = false;
};

} // namespace War3Map::Modules
