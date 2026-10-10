#pragma once

#include "Theme/DesignTokens.generated.h"

#include <QFont>
#include <QString>
#include <QStringList>

namespace War3Map::UiKit {

struct FontSetup {
    // 令牌候选中本机实际可用的族名，按令牌顺序；全部不可用时为空（使用系统默认）。
    QStringList uiFamilies;
    QStringList codeFamilies;
    QStringList loadedFiles;  // 本次成功注册的自带字体文件
    QStringList failedFiles;  // 存在但注册失败的字体文件（进入诊断）
    // 第一候选是否可用（即自带的界面 / 代码字体已生效）。
    bool bundledUi = false;
    bool bundledCode = false;
};

// 发布与开发构建都把字体复制到程序目录下的 fonts。
[[nodiscard]] QString DefaultFontDirectory();

// 注册目录中的 .ttf / .otf（不递归），再按令牌顺序解析可用族名。
// 目录不存在不是错误：自带字体缺失时回退到系统字体。
[[nodiscard]] FontSetup LoadFonts(const QString& directory);

// 字重只用令牌里的 400 / 500 / 700。思源黑体可变字体（回退字体）的命名实例没有 600（DemiBold），
// Qt 匹配 600 时会退到第一个实例 ExtraLight，标题笔画只剩 1 像素；半粗一律用 500。
[[nodiscard]] QFont UiFont(const FontSetup& setup,
    int pixelSize = Tokens::Size::kFontBody,
    int weight = Tokens::Weight::kWeightRegular);
[[nodiscard]] QFont CodeFont(const FontSetup& setup, int pixelSize = Tokens::Size::kFontBody);
// 数值与坐标用等宽字体：按令牌候选族名顺序回退，不需要 FontSetup（样式表不写族名，控件用它 setFont）。
[[nodiscard]] QFont MonoFont(int pixelSize = Tokens::Size::kFontSmall);

// 设为应用默认字体；样式表只写字号，族名来自这里。preferredFamily 是用户在设置里选的字体，
// 空或本机没有时使用令牌默认（微软雅黑）。
void ApplyApplicationFont(const FontSetup& setup, const QString& preferredFamily = {});
// 设置里改字体后即时生效：沿用上次 ApplyApplicationFont 的字体候选，不重复登记字体文件。
void ApplyPreferredUiFamily(const QString& preferredFamily);
// 设置里可选的界面字体：本机支持简体中文的字体族，不含竖排变体和私有字体。
[[nodiscard]] QStringList SelectableUiFamilies();
// 应用设置里保存用户字体选择的键。
inline constexpr char kUiFontFamilySettingsKey[] = "ui/fontFamily";

} // namespace War3Map::UiKit
