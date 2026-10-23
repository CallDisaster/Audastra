# 游戏界面（fdf/toc、皮肤、实例树与布局）。只依赖 Core，不依赖 Qt；编辑器与命令行共用。
add_library(War3MapGameUi STATIC
    FrameDefinitionParser.cpp
    FrameDraw.cpp
    FrameLibrary.cpp
    FrameTree.cpp
    UiSkin.cpp)
add_library(War3MapEditor::GameUi ALIAS War3MapGameUi)
target_link_libraries(War3MapGameUi PUBLIC War3MapEditor::Core)
target_compile_definitions(War3MapGameUi PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(War3MapGameUi PRIVATE /W4 /permissive- /utf-8)
endif()
