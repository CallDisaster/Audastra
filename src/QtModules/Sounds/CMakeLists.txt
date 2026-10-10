qt_add_library(War3MapQtModuleSounds STATIC
    SoundsModule.cpp SoundsModule.h)
add_library(War3MapEditor::QtModuleSounds ALIAS War3MapQtModuleSounds)
target_include_directories(War3MapQtModuleSounds PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleSounds PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Widgets Qt6::Multimedia)
target_compile_definitions(War3MapQtModuleSounds PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleSounds PRIVATE /W4 /permissive- /utf-8)
