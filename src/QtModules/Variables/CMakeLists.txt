qt_add_library(War3MapQtModuleVariables STATIC
    VariablesModule.cpp VariablesModule.h
    VariablesNavigation.cpp
    VariablesDialog.cpp VariablesDialog.h)
add_library(War3MapEditor::QtModuleVariables ALIAS War3MapQtModuleVariables)
target_include_directories(War3MapQtModuleVariables PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleVariables PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Widgets)
target_compile_definitions(War3MapQtModuleVariables PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleVariables PRIVATE /W4 /permissive- /utf-8)
