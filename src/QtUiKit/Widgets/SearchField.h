#pragma once

class QLineEdit;

namespace War3Map::UiKit {

// 搜索框左侧放大镜。QLineEdit::addAction 的图标在非整数缩放下会偏离垂直中心，
// 这里改用子控件按输入框实际高度居中，并用文本边距让出位置；全编辑器搜索框共用这一处实现。
void InstallSearchIcon(QLineEdit* edit);

} // namespace War3Map::UiKit
