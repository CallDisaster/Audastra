#pragma once
#include "Widgets/ObjectResourcePicker.h"

#include <functional>

class QObject;
namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::QtApp {
// 同一工程只建一个工厂，所有面板共用它按内容身份缓存的资源索引。
[[nodiscard]] Modules::ObjectResourcePickerFactory MakeObjectResourcePickerFactory(QtProject::EditorProject& project);
// 返回的函数在 UI 线程调用：冻结当前工程并在后台低优先级建好游戏文件与物体资源索引。
// context 销毁后后台工作尽快取消。
[[nodiscard]] std::function<void()> MakeObjectResourcePrewarm(Modules::ObjectResourcePickerFactory factory, QObject* context);
}
