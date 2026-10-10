# UI 函数库两层启用：发现、指纹、全局 / 地图比对与有效层序。只依赖 Core 与 Trigger，不依赖 Qt。
add_library(War3MapUiLibrary STATIC
    UiLibrary.cpp)
add_library(War3MapEditor::UiLibrary ALIAS War3MapUiLibrary)
target_link_libraries(War3MapUiLibrary PUBLIC War3MapTrigger War3MapEditor::Core)
target_compile_definitions(War3MapUiLibrary PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(War3MapUiLibrary PRIVATE /W4 /permissive- /utf-8)
endif()
