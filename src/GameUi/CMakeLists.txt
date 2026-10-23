# 游戏界面（fdf/toc、皮肤、实例树与布局，地图脚本加载的界面目录、脚本创建的界面与预览模拟，
# 以及自有界面描述 audastra.ui）。不依赖 Qt；
# 编辑器与命令行共用。分析地图脚本时复用 JASS / Lua 的词法与语法分析。
add_library(War3MapGameUi STATIC
    FrameDefinitionParser.cpp
    FrameDraw.cpp
    FrameLibrary.cpp
    FrameTree.cpp
    GameCodeLayout.cpp
    MapUiFiles.cpp
    ScriptFrames.cpp
    ScriptFrameTree.cpp
    ScriptObjectCalls.cpp
    ScriptSimulation.cpp
    ScriptUiApi.cpp
    ScriptValue.cpp
    UiDescription.cpp
    UiDescriptionCheck.cpp
    UiDescriptionEdit.cpp
    UiDescriptionText.cpp
    UiDescriptionTree.cpp
    UiSkin.cpp)
add_library(War3MapEditor::GameUi ALIAS War3MapGameUi)
target_link_libraries(War3MapGameUi PUBLIC War3MapEditor::Core PRIVATE War3MapEditor::Jass War3MapEditor::Lua audastra-json)
target_compile_definitions(War3MapGameUi PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(War3MapGameUi PRIVATE /W4 /permissive- /utf-8)
endif()
