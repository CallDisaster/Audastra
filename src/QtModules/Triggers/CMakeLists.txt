# 触发器模块：触发器目录、ECA 逻辑编辑、参数与函数选择对话框、树 / 逻辑模型。
qt_add_library(War3MapQtModuleTriggers STATIC
    TriggerModule.cpp TriggerModule.h
    TriggerModulePage.cpp
    TriggerDirectoryActions.cpp
    TriggerCodeActions.cpp
    TriggerCodeChecking.cpp TriggerCodeChecking.h
    TriggerLuaScripts.cpp TriggerLuaScripts.h
    LuaCodeAssist.cpp LuaCodeAssist.h
    TriggerEcaActions.cpp
    TriggerEcaStructure.cpp
    TriggerValueActions.cpp
    TriggerBlueprint.cpp TriggerBlueprint.h
    TriggerBlueprintInput.cpp
    TriggerBlueprintItems.cpp TriggerBlueprintItems.h
    TriggerBlueprintActions.cpp
    TriggerValuePanel.cpp TriggerValuePanel.h
    TriggerFunctionList.cpp TriggerFunctionList.h
    TriggerSentenceDelegate.cpp TriggerSentenceDelegate.h
    TriggerUiText.h
    TriggerMapScript.cpp
    TriggerNavigation.cpp
    TriggerReplaceDialog.cpp TriggerReplaceDialog.h
    TriggerReplaceActions.cpp
    TriggerTreeActions.cpp
    UiLibraryDialog.cpp UiLibraryDialog.h
    TriggerDialogs.cpp TriggerDialogs.h
    TriggerModels.cpp TriggerModels.h TriggerModelsDetail.h TriggerEcaModel.cpp
    TriggerEcaModelSlots.cpp)
add_library(War3MapEditor::QtModuleTriggers ALIAS War3MapQtModuleTriggers)
target_include_directories(War3MapQtModuleTriggers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# TriggerDialogs.h 仍转发包含变量对话框（旧头入口），因此公开依赖变量模块。
target_link_libraries(War3MapQtModuleTriggers PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell War3MapEditor::QtUiKit
    War3MapEditor::QtModuleVariables Qt6::Widgets)
target_link_libraries(War3MapQtModuleTriggers PRIVATE War3MapEditor::Services)
target_compile_definitions(War3MapQtModuleTriggers PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleTriggers PRIVATE /W4 /permissive- /utf-8)
