#pragma once
#include "Widgets/ObjectResourcePicker.h"

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::QtApp {
[[nodiscard]] Modules::ObjectResourcePickerFactory MakeObjectResourcePickerFactory(QtProject::EditorProject& project);
}
