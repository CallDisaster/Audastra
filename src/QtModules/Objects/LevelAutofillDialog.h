#pragma once
#include "War3MapCore/ObjectData/LevelAutofill.h"
#include <QDialog>
#include <QString>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QTableWidget;

// 原版物编“自动填充等级”：开始/结束等级、基础值与三个因素（文本字段为基础文本与等级记号），下方实时预览各级新旧值。
// 计算走 Core 的 ComputeLevelAutofill；确定后由调用方经字段模型一次撤销写入，失败时保留表单并显示原因。
class LevelAutofillDialog final : public QDialog {
public:
    using Values = std::vector<std::pair<std::uint32_t, QString>>;
    // 返回空表示写入成功；否则为显示给作者的原因。
    using Commit = std::function<QString(const Values&)>;

    // current 是该字段当前对象上各等级的现值（按等级排序，不能为空）。
    LevelAutofillDialog(const QString& fieldName, War3Map::ObjectData::LevelAutofillKind kind, bool multiline,
        Values current, Commit commit, QWidget* parent = nullptr);

    [[nodiscard]] const Values& Preview() const { return preview_; }

private:
    void SyncBaseFromStart();
    void Recompute();
    void Submit();
    [[nodiscard]] QString CurrentValue(std::uint32_t level) const;
    [[nodiscard]] QString BaseText() const;

    War3Map::ObjectData::LevelAutofillKind kind_;
    Values current_;
    Values preview_;
    Commit commit_;
    bool baseEdited_ = false;
    QSpinBox* start_ = nullptr;
    QSpinBox* end_ = nullptr;
    QLineEdit* base_ = nullptr;
    QLineEdit* previousFactor_ = nullptr;
    QLineEdit* levelFactor_ = nullptr;
    QLineEdit* constant_ = nullptr;
    QLineEdit* baseLine_ = nullptr;
    QPlainTextEdit* baseBlock_ = nullptr;
    QLineEdit* token_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* problem_ = nullptr;
    QWidget* okButton_ = nullptr;
};
