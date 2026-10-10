# 物体管理模块：地图概况、物体 / 触发器 / 函数 / 变量的引用总览（只读）。
qt_add_library(War3MapQtModuleManager STATIC
    ManagerModule.cpp ManagerModule.h
    ManagerRows.cpp ManagerRows.h
    ManagerUsages.cpp)
add_library(War3MapEditor::QtModuleManager ALIAS War3MapQtModuleManager)
target_include_directories(War3MapQtModuleManager PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleManager PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Widgets)
target_compile_definitions(War3MapQtModuleManager PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleManager PRIVATE /W4 /permissive- /utf-8)
