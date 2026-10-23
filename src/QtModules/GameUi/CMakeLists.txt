# 游戏界面工作区：显示游戏自带界面（贴图、文字、字体、层与节点）、地图脚本界面与地图自己的界面描述；
# 资源读取复用世界模块的统一解析器。
qt_add_library(War3MapQtModuleGameUi STATIC
    GameUiModule.cpp GameUiModule.h GameUiPanels.cpp
    GameUiCanvas.cpp GameUiCanvas.h
    GameUiAuthoring.cpp GameUiDescriptionPreview.cpp GameUiInputs.h
    GameUiSimulation.cpp GameUiSources.cpp GameUiSources.h
    GameUiToolService.cpp GameUiToolService.h)
add_library(War3MapEditor::QtModuleGameUi ALIAS War3MapQtModuleGameUi)
target_include_directories(War3MapQtModuleGameUi PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleGameUi PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell War3MapEditor::QtModuleWorld War3MapEditor::GameUi
    War3MapEditor::WarcraftText Qt6::Widgets)
target_compile_definitions(War3MapQtModuleGameUi PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleGameUi PRIVATE /W4 /permissive- /utf-8)
