#pragma once
#include <QCoreApplication>
#include <QStyle>
#include <QToolButton>

namespace War3Map::QtCommon {
inline void ApplyLuaReloadButtonState(QToolButton& button,const QString& actionText,bool pending,bool rebuild) {
    const auto text=rebuild?QCoreApplication::translate("MainWindow","需重新构建"):actionText;
    const auto style=rebuild?Qt::ToolButtonTextBesideIcon:Qt::ToolButtonIconOnly;
    if(button.property("luaReloadPending").isValid() && button.property("luaReloadPending").toBool()==pending
        && button.property("hluaRebuildRequired").isValid() && button.property("hluaRebuildRequired").toBool()==rebuild
        && button.text()==text && button.toolButtonStyle()==style)return;
    button.setProperty("luaReloadPending",pending);
    button.setProperty("hluaRebuildRequired",rebuild);
    button.setToolButtonStyle(style);button.setText(text);
    button.style()->unpolish(&button);button.style()->polish(&button);
    button.updateGeometry();button.update();
}
} // namespace War3Map::QtCommon
