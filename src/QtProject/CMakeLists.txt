# 工程层（Qt 适配）：当前打开的地图工程、文档与会话、修订与恢复、本机游戏资源图标。
# 不含界面控件；界面模块通过它读写数据。
qt_add_library(War3MapQtProject STATIC
    EditorProject.cpp
    ProjectObjectCatalog.cpp ProjectObjectCatalog.h
    RecentMapsStore.cpp RecentMapsStore.h
    Commands/MapDataCommands.cpp
    Commands/MapDataCommands.h
    Commands/AssetCommands.cpp
    Commands/AssetCommands.h
    Commands/TriggerArgumentCommands.cpp
    Commands/TriggerArgumentCommands.h
    Commands/TriggerStringCommands.cpp
    Commands/TriggerStringCommands.h
    Commands/TriggerCodeCommands.cpp
    Commands/TriggerCodeCommands.h
    Commands/TriggerCommands.cpp
    Commands/TriggerCommands.h
    Commands/TriggerDirectoryCommands.cpp
    Commands/TriggerDirectoryCommands.h
    ProjectSnapshots.cpp
    ProjectRevisions.cpp
    ProjectSourceWatch.cpp
    ProjectSourceChecks.cpp
    ProjectSave.cpp
    ProjectSaveJob.cpp ProjectSaveJob.h
    RecoveryBackupCleanup.cpp RecoveryBackupCleanup.h
    LegacyMapBackupCheck.cpp LegacyMapBackupCheck.h
    ProjectOpen.cpp
    ProjectOpen.h
    ProjectFileIO.cpp
    ProjectFileIO.h
    ImportQueue.cpp
    AssetEdits.cpp
    AssetOwnedPayload.cpp AssetOwnedPayload.h
    PendingImportPayload.cpp PendingImportPayload.h
    ProjectDiagnostics.cpp
    TriggerWorkspace.cpp
    TriggerMapObjectReferences.cpp
    ProjectStrings.cpp
    ProjectLuaCatalog.cpp ProjectLuaModules.h
    ObjectNames.cpp
    ObjectContextProvider.cpp ObjectContextProvider.h
    MapCopySave.cpp
    MapSaveRun.cpp
    MapSavePreferences.cpp
    MapScriptRebuild.cpp
    ProjectCodeChecking.cpp
    LuaMapTargetProject.cpp
    ProjectMapData.cpp
    ProjectAuxiliaryEdits.cpp ProjectAuxiliaryEdits.h
    ProjectChanges.cpp
    ProjectQuerySources.cpp
    ProjectSummary.cpp
    UiLibraryState.cpp
    UiLibraryWatch.cpp
    ProjectReferences.cpp
    ProjectDeletionAssessment.cpp
    ProjectValidation.cpp
    ObjectNames.h
    EditorProject.h
    Icons/IconProvider.cpp
    Icons/IconProvider.h)
add_library(War3MapEditor::QtProject ALIAS War3MapQtProject)

target_include_directories(War3MapQtProject PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/Commands")
target_link_libraries(War3MapQtProject
    PUBLIC
        War3MapEditor::Core
        War3MapEditor::ObjectData
        War3MapEditor::Trigger
        War3MapEditor::ScriptBuild
        War3MapEditor::UiLibrary
        War3MapEditor::Services
        Qt6::Core
        Qt6::Gui)
target_compile_definitions(War3MapQtProject PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtProject PRIVATE /W4 /permissive- /utf-8)
