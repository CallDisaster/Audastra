#pragma once

#include "Widgets/CodeViewTypes.h"

#include <functional>
#include <optional>

namespace War3Map::UiKit {

// JASS / vJass 调用的参数提示。line 为光标所在行，column 为光标在行内的位置（QString 下标）；
// JASS 语句不跨行，只看本行。signatureOf 按函数名返回声明签名（“名 takes 类型 名, … returns 类型”），没有时返回空。
[[nodiscard]] std::optional<CodeSignatureHelp> JassSignatureHelp(const QString& line, int column, CodeLanguage language,
    const std::function<QString(const QString& name)>& signatureOf);

// 把声明签名整理成提示行，参数位置填进 help.parameters；不是函数签名时返回 false。
bool JassSignatureLabel(const QString& name, const QString& signature, CodeSignatureHelp& help);

} // namespace War3Map::UiKit
