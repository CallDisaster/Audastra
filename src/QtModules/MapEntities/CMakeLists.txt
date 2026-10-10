qt_add_library(War3MapQtModuleMapEntities STATIC
    MapEntitiesModule.cpp MapEntitiesModule.h)
add_library(War3MapEditor::QtModuleMapEntities ALIAS War3MapQtModuleMapEntities)
target_include_directories(War3MapQtModuleMapEntities PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleMapEntities PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Widgets)
target_compile_definitions(War3MapQtModuleMapEntities PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleMapEntities PRIVATE /W4 /permissive- /utf-8)
