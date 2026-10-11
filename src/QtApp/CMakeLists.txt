qt_add_executable(war3-map-editor WIN32
    Main.cpp
    AcceptanceObjectProbe.cpp
    AcceptanceProbe.cpp
    MainWindow.cpp
    MainWindowStartup.cpp
    AiWorkspaceActions.cpp
    ProductServicesActions.cpp
    LiveEditorTools.cpp
    LiveEditorTools.h
    LiveObjectTools.cpp
    LiveObjectTools.h
    MapDropActions.cpp
    MainWindow.h
    MainWindowChrome.cpp
    MainWindowCommands.cpp
    ProjectSearchActions.cpp GlobalSearchDialog.cpp GlobalSearchDialog.h
    ProjectSearchCapture.cpp ProjectSearchCapture.h
    MapSizeActions.cpp MapSizeDialog.cpp MapSizeDialog.h
    MainWindowChanges.cpp
    ResourceSettingsActions.cpp
    MapSaveActions.cpp
    MapTestActions.cpp
    MapTestSettingsPage.cpp
    MapTestSettingsPage.h
    RecoverySettingsPage.cpp
    RecoverySettingsPage.h
    WorldEffectsSettingsPage.cpp
    WorldEffectsSettingsPage.h
    LuaMapSaveActions.cpp
    MapCreateActions.cpp
    WorkspaceActions.cpp
    AppHelpers.h
    EditorParts.cpp
    EditorParts.h
    NativeWorkbench.cpp
    ProjectActions.cpp
    RecoveryExitDialog.cpp RecoveryExitDialog.h
    ProjectRevisionActions.cpp
    ProjectWatchActions.cpp
    MapDataActions.cpp
    UiCaptureActions.cpp
    WorldProbeActions.cpp
    WorldMeasurementActions.cpp
)

# Windows 文件与快捷方式图标独立于 Qt 的窗口图标，直接嵌入 EXE 资源。
if(WIN32)
    enable_language(RC)
    configure_file(ApplicationIcon.rc.in "${CMAKE_CURRENT_BINARY_DIR}/ApplicationIcon.rc" @ONLY)
    target_sources(war3-map-editor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ApplicationIcon.rc")
endif()

# 每次构建重新生成版本戳（git hash + 时间戳），标题栏可见，便于区分新旧构建。
set(WAR3MAP_BUILD_INFO_CPP "${CMAKE_CURRENT_BINARY_DIR}/war3_map_build_info.cpp")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/BuildInfoGen.cmake"
"string(TIMESTAMP NOW \"%Y-%m-%d %H:%M:%S\")
execute_process(COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY \"${CMAKE_SOURCE_DIR}\"
    OUTPUT_VARIABLE HASH OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT HASH)
    set(HASH \"unknown\")
endif()
file(WRITE \"${WAR3MAP_BUILD_INFO_CPP}\"
\"const char* kWar3MapBuildStamp = \\\"\${HASH} \${NOW}\\\";\\n\")
")
add_custom_target(war3-map-build-info ALL
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/BuildInfoGen.cmake"
    BYPRODUCTS "${WAR3MAP_BUILD_INFO_CPP}")
set_source_files_properties("${WAR3MAP_BUILD_INFO_CPP}" PROPERTIES GENERATED TRUE)
add_dependencies(war3-map-editor war3-map-build-info)
target_sources(war3-map-editor PRIVATE "${WAR3MAP_BUILD_INFO_CPP}")

target_link_libraries(war3-map-editor
    PRIVATE
        War3MapEditor::Core
        War3MapEditor::ObjectData
        War3MapEditor::MapSizeAnalysis
        War3MapEditor::Trigger
        War3MapEditor::QtModuleVariables
        War3MapEditor::QtModuleAssets
        War3MapEditor::QtModuleTriggers
        War3MapEditor::QtModuleObjects
        War3MapEditor::QtModuleManager
        War3MapEditor::QtShell
        War3MapEditor::QtProject
        War3MapEditor::QtUiKit
        War3MapEditor::QtAgent
        War3MapEditor::QtFeedback
        War3MapEditor::QtUpdate
        War3MapEditor::McpProtocol
        War3MapEditor::QtModuleMapEntities
        War3MapEditor::QtModuleMapSettings
        War3MapEditor::QtModuleSounds
        War3MapEditor::QtModuleWorld
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
war3map_deploy_fonts(war3-map-editor)
war3map_deploy_qt_runtime(war3-map-editor)
add_dependencies(war3-map-editor war3-render-d3d11)
add_dependencies(war3-map-editor audastra-runtime32)
add_custom_command(TARGET war3-map-editor POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:war3-render-d3d11>" "$<TARGET_FILE_DIR:war3-map-editor>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-launcher.exe" "$<TARGET_FILE_DIR:war3-map-editor>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-bootstrap.dll" "$<TARGET_FILE_DIR:war3-map-editor>"
    VERBATIM)

target_compile_definitions(war3-map-editor PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(AUDASTRA_DEV_SOURCE_FALLBACKS)
    target_compile_definitions(war3-map-editor PRIVATE WAR3MAP_AGENT_SKILL_SOURCE_DIR="${PROJECT_SOURCE_DIR}/.agents/skills")
endif()
target_compile_options(war3-map-editor PRIVATE /W4 /permissive- /utf-8)
target_include_directories(war3-map-editor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
