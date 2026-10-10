qt_add_library(War3MapQtModuleMapSettings STATIC
    MapSettingsModule.cpp MapSettingsLayout.cpp MapPlayerEnumDelegate.cpp MapSettingsModule.h MapPlayerEnumDelegate.h)
add_library(War3MapEditor::QtModuleMapSettings ALIAS War3MapQtModuleMapSettings)
target_include_directories(War3MapQtModuleMapSettings PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleMapSettings PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Widgets)
target_compile_definitions(War3MapQtModuleMapSettings PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleMapSettings PRIVATE /W4 /permissive- /utf-8)
