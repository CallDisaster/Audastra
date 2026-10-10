#pragma once

#include "Modules/EditorModule.h"

#include <QLabel>
#include <QPointer>

namespace War3Map::Shell {

// 尚未接通的模块：在切换器中占位并显示原因，不能被激活或停靠。
class PlaceholderModule final : public EditorModule {
public:
    PlaceholderModule(ModuleId id, QString label, QString icon, QKeySequence shortcut, QString reason)
        : info_{std::move(id), std::move(label), std::move(icon), std::move(shortcut), false, std::move(reason)} {}
    ~PlaceholderModule() override { delete widget_.data(); }

    ModuleInfo Info() const override { return info_; }
    QWidget* Widget() override {
        if (!widget_) {
            widget_ = new QLabel(info_.unavailableReason);
            widget_->setAlignment(Qt::AlignCenter);
        }
        return widget_;
    }

private:
    ModuleInfo info_;
    QPointer<QLabel> widget_;
};

} // namespace War3Map::Shell
