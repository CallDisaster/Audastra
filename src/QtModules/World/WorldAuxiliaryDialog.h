#pragma once

#include "War3MapCore/MapDocument.h"
#include <QDialog>
#include <array>
#include <variant>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace War3Map::Modules {
using WorldAuxiliaryRecord = std::variant<MapData::RegionRecord, MapData::CameraRecord>;

// 只编辑一份草稿；来源身份、引用、发布和撤销由 ProjectAuxiliaryEdits 负责。
class WorldAuxiliaryDialog final : public QDialog {
public:
    WorldAuxiliaryDialog(WorldAuxiliaryRecord record, const MapDocument& source, bool creating, QWidget* parent);
    [[nodiscard]] Result<WorldAuxiliaryRecord> Record() const;
    void accept() override;
private:
    WorldAuxiliaryRecord original_;
    QLineEdit* name_ = nullptr;
    std::array<QLineEdit*, 9> numbers_{};
    QLineEdit* weather_ = nullptr;
    QComboBox* sound_ = nullptr;
    std::array<QSpinBox*, 3> color_{};
    QLabel* error_ = nullptr;
};
} // namespace War3Map::Modules
