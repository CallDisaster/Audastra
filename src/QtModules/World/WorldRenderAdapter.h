#pragma once

#include <QString>

class QLibrary;

namespace War3Map::Modules {

// 编辑器“选择显卡”设置：world/renderAdapter 保存跨重启稳定的显卡键（空 = 跟随系统），
// world/renderAdapterName 保存选择时的显卡名，卡不在时用它说明。
inline constexpr char kRenderAdapterSettingsKey[] = "world/renderAdapter";
inline constexpr char kRenderAdapterNameSettingsKey[] = "world/renderAdapterName";

struct RenderAdapterPreference {
    QString text; // 显卡键或名称片段；空表示跟随系统
    QString name; // 用于说明的显示名
};

// 命令行 --render-adapter：只对本进程生效并覆盖设置。值为显卡键、名称片段（如 Quadro）或 default。
void SetRenderAdapterOverride(const QString& text);
[[nodiscard]] RenderAdapterPreference ReadRenderAdapterPreference();
// 把当前偏好交给已加载的渲染插件，之后新建的渲染器按它建设备。
// 返回给界面的说明：所选显卡不可用或插件不支持选择时非空，此时使用系统默认显卡。
[[nodiscard]] QString ApplyRenderAdapterPreference(QLibrary& library);

} // namespace War3Map::Modules
