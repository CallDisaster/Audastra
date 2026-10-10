#pragma once

#include "War3MapCore/Services/MapSizeAnalysis.h"

#include <QDialog>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class QLabel;
class QTableWidget;

namespace War3Map::QtApp {
class MapSizeDialog final : public QDialog {
public:
    using Revision = std::function<std::string()>;
    using Query = std::function<Result<Services::MapSizeReport>(std::string_view)>;
    using Activate = std::function<void(const Services::MapSizeRow&, std::string_view)>;
    MapSizeDialog(Revision revision, Query query, Activate activate, QWidget* parent = nullptr);
    void Focus();
    void ContextChanged();
private:
    void Refresh();
    void Populate(const Services::MapSizeReport& report);
    void ActivateCurrent();
    Revision revision_;
    Query query_;
    Activate activate_;
    QLabel* scope_ = nullptr;
    QLabel* status_ = nullptr;
    QTableWidget* totals_ = nullptr;
    QTableWidget* top_ = nullptr;
    std::optional<Services::MapSizeReport> report_;
    std::uint64_t requestGeneration_ = 0;
};
} // namespace War3Map::QtApp
