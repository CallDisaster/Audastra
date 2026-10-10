#pragma once

#include "War3MapCore/Services/ProjectSearch.h"

#include <QDialog>

#include <functional>
#include <cstdint>
#include <optional>
#include <vector>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTimer;

namespace War3Map::QtApp {
class GlobalSearchDialog final : public QDialog {
public:
    using Query = std::function<Result<Services::ProjectSearchPage>(std::string_view, std::string_view,
        const Services::ProjectSearchOptions&, std::size_t, const Services::ProjectSearchCursor*)>;
    GlobalSearchDialog(std::function<std::string()> revision, Query query,
        std::function<void(const Services::ProjectSearchRow&)> activate, QWidget* parent = nullptr);
    void FocusQuery(const QString& text = {});
    void ContextChanged();
private:
    void Refresh(bool reset);
    void Populate(const Services::ProjectSearchPage& page);
    void ActivateCurrent();
    std::function<std::string()> revision_;
    Query query_;
    std::function<void(const Services::ProjectSearchRow&)> activate_;
    QLineEdit* edit_ = nullptr;
    QTreeWidget* results_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QTimer* timer_ = nullptr;
    std::string boundRevision_, boundQuery_;
    std::vector<std::optional<Services::ProjectSearchCursor>> starts_;
    std::size_t pageIndex_ = 0;
    std::optional<Services::ProjectSearchCursor> nextCursor_;
    std::vector<Services::ProjectSearchRow> rows_;
    bool dirty_ = true;
    std::uint64_t requestGeneration_ = 0;
};
} // namespace War3Map::QtApp
