foreach(product_unit IN ITEMS UpdateClient UpdateDialog FeedbackClient FeedbackDialog ProgramIssueReporter)
    string(TOLOWER "${product_unit}" product_unit_lower)
    qt_add_executable(audastra-${product_unit_lower}-tests ${product_unit}Tests.cpp)
    if(product_unit MATCHES "^Update")
        target_link_libraries(audastra-${product_unit_lower}-tests PRIVATE War3MapEditor::QtUpdate War3MapEditor::QtUiKit audastra-miniz audastra-sodium Qt6::Test)
    else()
        target_include_directories(audastra-${product_unit_lower}-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_link_libraries(audastra-${product_unit_lower}-tests PRIVATE War3MapEditor::QtFeedback War3MapEditor::QtUiKit audastra-miniz Qt6::Test)
    endif()
    target_compile_definitions(audastra-${product_unit_lower}-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-${product_unit_lower}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME qt-${product_unit_lower} COMMAND audastra-${product_unit_lower}-tests)
    set_tests_properties(qt-${product_unit_lower} PROPERTIES TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()
war3map_deploy_fonts(audastra-updatedialog-tests)

qt_add_executable(audastra-recovery-exit-dialog-tests RecoveryExitDialogTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/RecoveryExitDialog.cpp")
target_include_directories(audastra-recovery-exit-dialog-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(audastra-recovery-exit-dialog-tests PRIVATE Qt6::Widgets Qt6::Test)
target_compile_options(audastra-recovery-exit-dialog-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-recovery-exit-dialog COMMAND audastra-recovery-exit-dialog-tests)
set_tests_properties(qt-recovery-exit-dialog PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(audastra-project-save-cancellation-qt-tests ProjectSaveCancellationTests.cpp)
target_link_libraries(audastra-project-save-cancellation-qt-tests PRIVATE War3MapEditor::QtProject Qt6::Test)
target_compile_definitions(audastra-project-save-cancellation-qt-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-project-save-cancellation-qt-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-project-save-cancellation COMMAND audastra-project-save-cancellation-qt-tests)
set_tests_properties(qt-project-save-cancellation PROPERTIES TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(audastra-update-end-to-end-tests UpdateEndToEndTests.cpp)
target_link_libraries(audastra-update-end-to-end-tests PRIVATE
    War3MapEditor::QtUpdate audastra-miniz audastra-sodium Qt6::Test)
target_compile_definitions(audastra-update-end-to-end-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-update-end-to-end-tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(audastra-update-end-to-end-tests audastra-test-updater
    audastra-update-health-fixture-v1 audastra-update-health-fixture-v2 audastra-update-health-fixture-v3)
add_test(NAME qt-update-end-to-end COMMAND audastra-update-end-to-end-tests
    --fixture-v1 $<TARGET_FILE:audastra-update-health-fixture-v1>
    --fixture-v2 $<TARGET_FILE:audastra-update-health-fixture-v2>
    --fixture-v3 $<TARGET_FILE:audastra-update-health-fixture-v3>
    --test-updater $<TARGET_FILE:audastra-test-updater>)
set_tests_properties(qt-update-end-to-end PROPERTIES TIMEOUT 180
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 健康协议验收复用生产 Main、Normal 主窗及真实事件循环。
# main 重命名仅作用于测试 OBJECT，不改生产源文件或增加产品退出入口。
get_target_property(product_health_sources war3-map-editor SOURCES)
get_target_property(product_health_libraries war3-map-editor LINK_LIBRARIES)
get_target_property(product_health_definitions war3-map-editor COMPILE_DEFINITIONS)
set(product_health_launcher_sources ProductMainHealthLauncher.cpp)
foreach(product_health_source IN LISTS product_health_sources)
    if(product_health_source STREQUAL "Main.cpp")
        continue()
    elseif(IS_ABSOLUTE "${product_health_source}")
        list(APPEND product_health_launcher_sources "${product_health_source}")
    else()
        list(APPEND product_health_launcher_sources "${PROJECT_SOURCE_DIR}/src/QtApp/${product_health_source}")
    endif()
endforeach()
add_library(audastra-product-main-health-object OBJECT "${PROJECT_SOURCE_DIR}/src/QtApp/Main.cpp")
target_link_libraries(audastra-product-main-health-object PRIVATE ${product_health_libraries})
target_include_directories(audastra-product-main-health-object PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_compile_definitions(audastra-product-main-health-object PRIVATE
    ${product_health_definitions} main=AudastraProductMainForHealthTest)
target_compile_options(audastra-product-main-health-object PRIVATE /W4 /permissive- /utf-8)
add_dependencies(audastra-product-main-health-object audastra-version-info)

qt_add_executable(audastra-product-main-health-launcher ${product_health_launcher_sources}
    $<TARGET_OBJECTS:audastra-product-main-health-object>)
target_link_libraries(audastra-product-main-health-launcher PRIVATE ${product_health_libraries} audastra-json shell32 ole32 uuid)
target_include_directories(audastra-product-main-health-launcher PRIVATE
    "${PROJECT_SOURCE_DIR}/src/QtApp" "${PROJECT_SOURCE_DIR}/src/Core")
target_compile_definitions(audastra-product-main-health-launcher PRIVATE ${product_health_definitions})
target_compile_options(audastra-product-main-health-launcher PRIVATE /W4 /permissive- /utf-8)
add_dependencies(audastra-product-main-health-launcher war3-map-build-info audastra-version-info)
war3map_deploy_fonts(audastra-product-main-health-launcher)
war3map_deploy_qt_runtime(audastra-product-main-health-launcher)

add_executable(audastra-product-main-health-tests ProductMainHealthTests.cpp)
target_link_libraries(audastra-product-main-health-tests PRIVATE War3MapEditor::Core audastra-json Qt6::Core)
target_include_directories(audastra-product-main-health-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Core")
target_compile_definitions(audastra-product-main-health-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-product-main-health-tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(audastra-product-main-health-tests audastra-product-main-health-launcher audastra-version-info)
add_test(NAME qt-product-main-health COMMAND audastra-product-main-health-tests
    --launcher $<TARGET_FILE:audastra-product-main-health-launcher>
    --runtime-dir $<TARGET_FILE_DIR:audastra-product-main-health-launcher>
    --skills-dir "${PROJECT_SOURCE_DIR}/.agents/skills")
# 系统 TEMP 可能带额外 NTFS 数据流；只为该测试提供独占临时父目录，
# 仍由原安全租约检查普通路径、身份与 ADS，不更改机器或产品数据目录。
set(product_health_temp "${CMAKE_CURRENT_BINARY_DIR}/product-main-health-temp")
file(MAKE_DIRECTORY "${product_health_temp}")
set_tests_properties(qt-product-main-health PROPERTIES TIMEOUT 180
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;TEMP=set:${product_health_temp};TMP=set:${product_health_temp}")

foreach(agent_unit IN ITEMS Settings Window Controller)
    string(TOLOWER "${agent_unit}" agent_unit_lower)
    qt_add_executable(war3-map-agent-${agent_unit_lower}-tests Agent${agent_unit}Tests.cpp)
    target_link_libraries(war3-map-agent-${agent_unit_lower}-tests PRIVATE War3MapEditor::QtAgent Qt6::Test)
    target_compile_definitions(war3-map-agent-${agent_unit_lower}-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-agent-${agent_unit_lower}-tests PRIVATE /W4 /permissive- /utf-8)
    war3map_deploy_qt_runtime(war3-map-agent-${agent_unit_lower}-tests)
    add_test(NAME qt-agent-${agent_unit_lower} COMMAND war3-map-agent-${agent_unit_lower}-tests)
    set_tests_properties(qt-agent-${agent_unit_lower} PROPERTIES TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()
war3map_deploy_fonts(war3-map-agent-window-tests)
qt_add_executable(war3-map-agent-ui-capture AgentUiCapture.cpp)
target_link_libraries(war3-map-agent-ui-capture PRIVATE War3MapEditor::QtAgent)
target_compile_definitions(war3-map-agent-ui-capture PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-agent-ui-capture PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-agent-ui-capture)
war3map_deploy_fonts(war3-map-agent-ui-capture)
qt_add_executable(war3-map-mcp-agent-transport-tests McpAgentTransportTests.cpp)
target_link_libraries(war3-map-mcp-agent-transport-tests PRIVATE War3MapEditor::McpProtocol Qt6::Network Qt6::Test)
target_compile_definitions(war3-map-mcp-agent-transport-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-mcp-agent-transport-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-mcp-agent-transport-tests NO_GUI)
add_test(NAME qt-mcp-agent-transport COMMAND war3-map-mcp-agent-transport-tests)
set_tests_properties(qt-mcp-agent-transport PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-live-editor-tools-tests LiveEditorToolsTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveObjectTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveObjectTools.h"
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveEditorTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveEditorTools.h")
target_include_directories(war3-map-live-editor-tools-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-live-editor-tools-tests PRIVATE War3MapEditor::QtProject War3MapEditor::McpProtocol War3MapEditor::ObjectResourceAdapter Qt6::Test)
target_compile_definitions(war3-map-live-editor-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-live-editor-tools-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-live-editor-tools-tests)
add_test(NAME qt-live-editor-tools COMMAND war3-map-live-editor-tools-tests)
set_tests_properties(qt-live-editor-tools PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(war3-map-slk-dataset-oracle-tests SlkDatasetOracleTests.cpp)
target_link_libraries(war3-map-slk-dataset-oracle-tests PRIVATE War3MapEditor::ScriptBuild Qt6::Core)
target_compile_definitions(war3-map-slk-dataset-oracle-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    SLK_LNI_ORACLE_EXPECTED_PATH="${PROJECT_SOURCE_DIR}/tests/fixtures/slk_lni_oracle_expected.json")
target_compile_options(war3-map-slk-dataset-oracle-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME slk-dataset-lni-oracle COMMAND war3-map-slk-dataset-oracle-tests)
set_tests_properties(slk-dataset-lni-oracle PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-qt-model-tests
    QtModelTests.cpp)

target_include_directories(
    war3-map-qt-model-tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/QtApp")

target_link_libraries(
    war3-map-qt-model-tests
    PRIVATE
        War3MapEditor::Core
        War3MapEditor::ObjectData
        War3MapEditor::Trigger
        War3MapEditor::QtUiKit
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Test)

target_compile_definitions(
    war3-map-qt-model-tests
    PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN)

target_compile_options(war3-map-qt-model-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-editor-models COMMAND war3-map-qt-model-tests)
set_tests_properties(
    qt-editor-models
    PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_executable(
    war3-map-exit-after-open-fixture-writer
    ExitAfterOpenFixtureWriter.cpp)
target_compile_options(
    war3-map-exit-after-open-fixture-writer
    PRIVATE /W4 /permissive- /utf-8)

foreach(exit_after_open_case IN ITEMS success custom-text models-missing invalid-workspace)
    set(exit_after_open_custom_text OFF)
    # 每个配置与用例使用独立工作区，避免缺失模型用例删除成功用例的 fixture。
    set(exit_after_open_workspace
        "${CMAKE_CURRENT_BINARY_DIR}/exit-after-open-workspace-$<CONFIG>-${exit_after_open_case}")
    if(exit_after_open_case STREQUAL "success" OR exit_after_open_case STREQUAL "custom-text")
        if(exit_after_open_case STREQUAL "custom-text")
            set(exit_after_open_custom_text ON)
        endif()
        set(exit_after_open_path "${exit_after_open_workspace}")
        set(exit_after_open_expected 0)
        set(exit_after_open_generate_fixture ON)
        set(exit_after_open_remove_unit OFF)
        set(exit_after_open_require_loaded ON)
    elseif(exit_after_open_case STREQUAL "models-missing")
        set(exit_after_open_path "${exit_after_open_workspace}")
        set(exit_after_open_expected 2)
        set(exit_after_open_generate_fixture ON)
        set(exit_after_open_remove_unit ON)
        set(exit_after_open_require_loaded ON)
    else()
        # 以确定存在的普通文件作为非法工作区，避免测试依赖偶然不存在的路径。
        set(exit_after_open_path "${PROJECT_SOURCE_DIR}/CMakeLists.txt")
        set(exit_after_open_expected 1)
        set(exit_after_open_generate_fixture OFF)
        set(exit_after_open_remove_unit OFF)
        set(exit_after_open_require_loaded ON)
    endif()
    add_test(
        NAME "qt-exit-after-open-${exit_after_open_case}"
        COMMAND "${CMAKE_COMMAND}"
            "-DEDITOR=$<TARGET_FILE:war3-map-editor>"
            "-DFIXTURE_WRITER=$<TARGET_FILE:war3-map-exit-after-open-fixture-writer>"
            "-DQT_BIN_DIR=$<TARGET_FILE_DIR:Qt6::Core>"
            "-DWORKSPACE=${exit_after_open_path}"
            "-DEXPECTED_EXIT_CODE=${exit_after_open_expected}"
            "-DGENERATE_FIXTURE=${exit_after_open_generate_fixture}"
            "-DREMOVE_UNIT_FIXTURE=${exit_after_open_remove_unit}"
            "-DREQUIRE_LOADED_WORKSPACE=${exit_after_open_require_loaded}"
            "-DCUSTOM_TEXT_FIXTURE=${exit_after_open_custom_text}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/ExitAfterOpenProcessTest.cmake")
    set_tests_properties(
        "qt-exit-after-open-${exit_after_open_case}"
        PROPERTIES TIMEOUT 45)
endforeach()

# 嵌入式变量页：GUI 引用重命名与撤销一致性。
qt_add_executable(war3-map-variable-dialog-tests
    VariablesDialogTests.cpp)
target_include_directories(war3-map-variable-dialog-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-variable-dialog-tests PRIVATE
    War3MapEditor::Core War3MapEditor::Trigger Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-variable-dialog-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-variable-dialog-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-variable-dialog COMMAND war3-map-variable-dialog-tests)
set_tests_properties(qt-variable-dialog PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 通用对象目录与非模态选择组件：合成条目、offscreen 交互，不依赖游戏资源。
qt_add_executable(war3-map-object-catalog-widget-tests
    ObjectCatalogWidgetTests.cpp
)
target_include_directories(war3-map-object-catalog-widget-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-object-catalog-widget-tests PRIVATE
    War3MapEditor::Core
    War3MapEditor::Trigger
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-object-catalog-widget-tests PRIVATE
    NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-object-catalog-widget-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-object-catalog-widgets COMMAND war3-map-object-catalog-widget-tests)
set_tests_properties(qt-object-catalog-widgets PROPERTIES
    TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 主窗口会话回归复用实际 Qt 实现；独立入口不链接 Main.cpp 或生成版本戳。
qt_add_executable(war3-map-application-settings-commit-tests ApplicationSettingsCommitTests.cpp)
target_link_libraries(war3-map-application-settings-commit-tests PRIVATE War3MapEditor::QtAgent Qt6::Test)
target_compile_options(war3-map-application-settings-commit-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-application-settings-commit-tests)
add_test(NAME qt-application-settings-commit COMMAND war3-map-application-settings-commit-tests)
set_tests_properties(qt-application-settings-commit PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-startup-page-tests StartupPageTests.cpp)
target_link_libraries(war3-map-startup-page-tests PRIVATE War3MapEditor::QtShell Qt6::Test)
target_compile_options(war3-map-startup-page-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-startup-page-tests)
war3map_deploy_fonts(war3-map-startup-page-tests)
add_test(NAME qt-startup-page COMMAND war3-map-startup-page-tests)
add_test(NAME qt-startup-page-dpr2 COMMAND war3-map-startup-page-tests)
set_tests_properties(qt-startup-page qt-startup-page-dpr2 PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
set_tests_properties(qt-startup-page-dpr2 PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

# R08 MainWindowCommands.cpp 随应用 SOURCES 收入本目标与 project-watch，不能只登记产品漏测。
get_target_property(native_workbench_app_sources war3-map-editor SOURCES)
set(native_workbench_test_sources NativeWorkbenchTests.cpp)
foreach(native_workbench_source IN LISTS native_workbench_app_sources)
    get_filename_component(native_workbench_source_name "${native_workbench_source}" NAME)
    if(native_workbench_source_name STREQUAL "Main.cpp" OR
       native_workbench_source_name STREQUAL "war3_map_build_info.cpp")
        continue()
    endif()
    if(IS_ABSOLUTE "${native_workbench_source}")
        list(APPEND native_workbench_test_sources "${native_workbench_source}")
    else()
        list(APPEND native_workbench_test_sources "${PROJECT_SOURCE_DIR}/src/QtApp/${native_workbench_source}")
    endif()
endforeach()
qt_add_executable(war3-map-native-workbench-tests ${native_workbench_test_sources})
target_include_directories(war3-map-native-workbench-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-native-workbench-tests PRIVATE
    War3MapEditor::Core War3MapEditor::ObjectData War3MapEditor::Trigger War3MapEditor::QtUiKit
    War3MapEditor::MapSizeAnalysis War3MapEditor::ObjectResourceAdapter
    War3MapEditor::QtAgent War3MapEditor::McpProtocol War3MapEditor::QtFeedback War3MapEditor::QtUpdate
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-native-workbench-tests PRIVATE
    NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-native-workbench-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-native-workbench COMMAND war3-map-native-workbench-tests -o -,txt)
# Root-owned append-only suggestion after the existing native-workbench target declaration.
# The actual target already deploys fonts and compiles all app sources; no new executable.
if(WIN32)
    target_link_libraries(war3-map-native-workbench-tests PRIVATE Psapi)
endif()
add_test(NAME qt-trigger-replace-main-host-real
    COMMAND war3-map-native-workbench-tests TriggerReplaceOwnedLargeMapMainHost256 -o -,txt)
set_tests_properties(qt-trigger-replace-main-host-real PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 180
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
# F1：大图主窗口转换为自定义代码及撤销/重做，每步（含界面刷新）不超过 200 ms。
add_test(NAME qt-trigger-conversion-main-host-real
    COMMAND war3-map-native-workbench-tests TriggerConversionOwnedLargeMapMainHost -o -,txt)
set_tests_properties(qt-trigger-conversion-main-host-real PROPERTIES
    SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL TRUE
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
# Header-only test helper is in tests/qt and consumed by both existing test TUs.
# Explicit readonly map/catalog/optional source SHA/needle/target inputs remain runner ENV;
# no source path, user settings, runtime installation or game environment defaults here.

war3map_deploy_fonts(war3-map-native-workbench-tests)
set_tests_properties(qt-native-workbench PROPERTIES
    # 全量原版保存/宿主回归采用本阶段要求的900秒上限，保留全部断言。
    TIMEOUT 900
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

set(project_watch_test_sources ${native_workbench_test_sources})
list(REMOVE_ITEM project_watch_test_sources NativeWorkbenchTests.cpp)
list(APPEND project_watch_test_sources ProjectWatchTests.cpp)
qt_add_executable(war3-map-project-watch-tests ${project_watch_test_sources})
target_include_directories(war3-map-project-watch-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-project-watch-tests PRIVATE
    War3MapEditor::Core War3MapEditor::ObjectData War3MapEditor::Trigger War3MapEditor::QtUiKit
    War3MapEditor::MapSizeAnalysis War3MapEditor::ObjectResourceAdapter
    War3MapEditor::QtAgent War3MapEditor::McpProtocol War3MapEditor::QtFeedback War3MapEditor::QtUpdate
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-project-watch-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-project-watch-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-project-watch COMMAND war3-map-project-watch-tests)
set_tests_properties(qt-project-watch PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 地图/工作区/游戏/附加目录的图标层：仅原创 PNG 与合成 MPQ。
qt_add_executable(war3-map-icon-resource-layer-tests
    IconResourceLayerTests.cpp
)
target_include_directories(war3-map-icon-resource-layer-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-icon-resource-layer-tests PRIVATE
    War3MapEditor::Core War3MapEditor::Trigger
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-icon-resource-layer-tests PRIVATE
    NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-icon-resource-layer-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-icon-resource-layers COMMAND war3-map-icon-resource-layer-tests)
set_tests_properties(qt-icon-resource-layers PROPERTIES
    TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 应用目录的本机部署仅带 qwindows；测试显式从本次构建使用的 Qt 安装找 offscreen。
qt_add_executable(war3-map-object-field-tests
    ObjectFieldModelTests.cpp
)
target_include_directories(war3-map-object-field-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-object-field-tests PRIVATE
    War3MapEditor::Core War3MapEditor::ObjectData War3MapEditor::Trigger
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-object-field-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-object-field-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-object-fields COMMAND war3-map-object-field-tests)
set_tests_properties(qt-object-fields PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 界面组件库：令牌主题样式表、SVG 图标按状态着色、字体加载回退。
qt_add_executable(war3-map-uikit-tests UiKitTests.cpp)
target_link_libraries(war3-map-uikit-tests PRIVATE War3MapEditor::QtUiKit Qt6::Test)
target_compile_definitions(war3-map-uikit-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN
    "WAR3MAP_TEST_FONT_DIR=\"${WAR3MAPEDITOR_FONT_DIR}\"")
target_compile_options(war3-map-uikit-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-uikit COMMAND war3-map-uikit-tests)
set_tests_properties(qt-uikit PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# 外壳：停靠布局规则、布局持久化、模块移动时控件与状态保留、撤销路由。
qt_add_executable(war3-map-responsive-toolbar-tests ResponsiveToolbarTests.cpp)
target_link_libraries(war3-map-responsive-toolbar-tests PRIVATE War3MapEditor::QtShell Qt6::Test)
target_compile_options(war3-map-responsive-toolbar-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-responsive-toolbar COMMAND war3-map-responsive-toolbar-tests -o -,txt)
add_test(NAME qt-responsive-toolbar-dpr150 COMMAND war3-map-responsive-toolbar-tests -o -,txt)
set_tests_properties(qt-responsive-toolbar qt-responsive-toolbar-dpr150 PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
set_tests_properties(qt-responsive-toolbar-dpr150 PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=1.5")

qt_add_executable(war3-map-shell-tests ShellTests.cpp)
target_link_libraries(war3-map-shell-tests PRIVATE War3MapEditor::QtShell Qt6::Test)
target_compile_definitions(war3-map-shell-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-shell-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-shell COMMAND war3-map-shell-tests)
set_tests_properties(qt-shell PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

foreach(native_qt_test IN ITEMS qt-uikit qt-shell qt-variable-dialog qt-object-catalog-widgets qt-native-workbench qt-project-watch qt-icon-resource-layers qt-object-fields)
    set_property(TEST ${native_qt_test} APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endforeach()

target_link_libraries(war3-map-qt-model-tests PRIVATE War3MapEditor::QtProject)

target_link_libraries(war3-map-object-catalog-widget-tests PRIVATE War3MapEditor::QtProject)

target_link_libraries(war3-map-native-workbench-tests PRIVATE War3MapEditor::QtProject)
target_link_libraries(war3-map-native-workbench-tests PRIVATE War3MapEditor::QtModuleMapEntities)
target_link_libraries(war3-map-native-workbench-tests PRIVATE War3MapEditor::QtModuleMapSettings)
target_link_libraries(war3-map-native-workbench-tests PRIVATE War3MapEditor::QtModuleSounds)
target_link_libraries(war3-map-native-workbench-tests PRIVATE War3MapEditor::QtModuleWorld)
target_link_libraries(war3-map-native-workbench-tests PRIVATE War3MapEditor::QtModuleCampaign)

target_link_libraries(war3-map-project-watch-tests PRIVATE War3MapEditor::QtProject)
target_link_libraries(war3-map-project-watch-tests PRIVATE War3MapEditor::QtModuleMapEntities)
target_link_libraries(war3-map-project-watch-tests PRIVATE War3MapEditor::QtModuleMapSettings)
target_link_libraries(war3-map-project-watch-tests PRIVATE War3MapEditor::QtModuleSounds)
target_link_libraries(war3-map-project-watch-tests PRIVATE War3MapEditor::QtModuleWorld)
target_link_libraries(war3-map-project-watch-tests PRIVATE War3MapEditor::QtModuleCampaign)

qt_add_executable(audastra-object-context-provider-tests ObjectContextProviderTests.cpp)
target_link_libraries(audastra-object-context-provider-tests PRIVATE War3MapEditor::QtProject)
target_compile_definitions(audastra-object-context-provider-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-object-context-provider-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-object-context-provider COMMAND audastra-object-context-provider-tests)
set_tests_properties(qt-object-context-provider PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-world-module-tests WorldModuleTests.cpp)
target_link_libraries(war3-map-world-module-tests PRIVATE War3MapEditor::QtModuleWorld
    War3MapEditor::QtModuleMapEntities War3MapEditor::QtModuleMapSettings Qt6::Test)
target_compile_definitions(war3-map-world-module-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-world-module COMMAND war3-map-world-module-tests)
set_tests_properties(qt-world-module PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-world-viewport-tests WorldViewportTests.cpp)
target_link_libraries(war3-map-world-viewport-tests PRIVATE War3MapEditor::QtModuleWorld Qt6::Test)
target_compile_definitions(war3-map-world-viewport-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-viewport-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-world-viewport COMMAND war3-map-world-viewport-tests)
set_tests_properties(qt-world-viewport PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-world-placement-pending-tests WorldPlacementPendingTests.cpp)
qt_add_executable(war3-map-world-animation-diagnostics-tests WorldAnimationDiagnosticsTests.cpp)
qt_add_executable(war3-map-world-screenshot-encoder-tests WorldScreenshotEncoderTests.cpp)
target_link_libraries(war3-map-world-screenshot-encoder-tests PRIVATE War3MapEditor::QtModuleWorld)
target_compile_definitions(war3-map-world-screenshot-encoder-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-screenshot-encoder-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-world-screenshot-encoder COMMAND war3-map-world-screenshot-encoder-tests)
set_tests_properties(qt-world-screenshot-encoder PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
target_link_libraries(war3-map-world-animation-diagnostics-tests PRIVATE War3MapEditor::QtModuleWorld)
target_compile_definitions(war3-map-world-animation-diagnostics-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-animation-diagnostics-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-world-animation-diagnostics COMMAND war3-map-world-animation-diagnostics-tests)
set_tests_properties(qt-world-animation-diagnostics PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
qt_add_executable(war3-map-world-resource-reader-tests WorldResourceReaderTests.cpp)
target_link_libraries(war3-map-world-resource-reader-tests PRIVATE War3MapEditor::QtModuleWorld)
target_compile_definitions(war3-map-world-resource-reader-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-resource-reader-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-world-resource-reader COMMAND war3-map-world-resource-reader-tests)
set_tests_properties(qt-world-resource-reader PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
target_link_libraries(war3-map-world-placement-pending-tests PRIVATE War3MapEditor::QtModuleWorld)
target_compile_definitions(war3-map-world-placement-pending-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-placement-pending-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-world-placement-pending COMMAND war3-map-world-placement-pending-tests)
set_tests_properties(qt-world-placement-pending PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-world-pathing-helper-tests WorldPathingHelperPresenterTests.cpp)
target_link_libraries(war3-map-world-pathing-helper-tests PRIVATE War3MapEditor::QtModuleWorld)
target_compile_definitions(war3-map-world-pathing-helper-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-pathing-helper-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-world-pathing-helper COMMAND war3-map-world-pathing-helper-tests)
set_tests_properties(qt-world-pathing-helper PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

foreach(world_r08_part IN ITEMS WorldInspectorR08 WorldAssetBrowser WorldSelectionPresentation)
    qt_add_executable(war3-map-${world_r08_part}-tests ${world_r08_part}Tests.cpp)
    target_link_libraries(war3-map-${world_r08_part}-tests PRIVATE War3MapEditor::QtModuleWorld Qt6::Test)
    target_compile_definitions(war3-map-${world_r08_part}-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-${world_r08_part}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME qt-${world_r08_part} COMMAND war3-map-${world_r08_part}-tests)
    set_tests_properties(qt-${world_r08_part} PROPERTIES TIMEOUT 60
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endforeach()
# 选取圈附加层须通过渲染核心同一份输入校验（插件提交前的规则）。
target_link_libraries(war3-map-WorldSelectionPresentation-tests PRIVATE War3RenderCore::SkinInput)

target_link_libraries(war3-map-icon-resource-layer-tests PRIVATE War3MapEditor::QtProject)

target_link_libraries(war3-map-object-field-tests PRIVATE War3MapEditor::QtProject War3MapEditor::QtModuleObjects)

# 工程层不创建主窗口，验证多工程隔离及失败快照不改变保存状态。
qt_add_executable(war3-map-editor-project-tests EditorProjectTests.cpp)
qt_add_executable(war3-map-lua-scripts-tests LuaScriptsTests.cpp)
target_link_libraries(war3-map-lua-scripts-tests PRIVATE War3MapEditor::QtModuleTriggers War3MapEditor::QtModuleAssets War3MapEditor::QtProject War3MapEditor::QtUiKit Qt6::Test)
target_compile_options(war3-map-lua-scripts-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-lua-scripts-tests)
add_test(NAME qt-lua-scripts COMMAND war3-map-lua-scripts-tests)
set_tests_properties(qt-lua-scripts PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-directory-asset-tests DirectoryAssetTests.cpp)
target_link_libraries(war3-map-directory-asset-tests PRIVATE War3MapEditor::QtModuleTriggers War3MapEditor::QtModuleAssets War3MapEditor::QtProject War3MapEditor::QtUiKit Qt6::Test)
target_compile_definitions(war3-map-directory-asset-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-directory-asset-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-directory-assets COMMAND war3-map-directory-asset-tests)
set_tests_properties(qt-directory-assets PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-native-lua-code-tests NativeLuaCodeTests.cpp)
target_link_libraries(war3-map-native-lua-code-tests PRIVATE War3MapEditor::QtModuleTriggers War3MapEditor::QtProject War3MapEditor::QtUiKit Qt6::Test)
target_compile_definitions(war3-map-native-lua-code-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-native-lua-code-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-native-lua-code COMMAND war3-map-native-lua-code-tests)
set_tests_properties(qt-native-lua-code PROPERTIES TIMEOUT 40 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
target_link_libraries(war3-map-editor-project-tests PRIVATE War3MapEditor::QtProject Qt6::Test)
target_compile_options(war3-map-editor-project-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-editor-project COMMAND war3-map-editor-project-tests)
set_tests_properties(qt-editor-project PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

target_link_libraries(war3-map-qt-model-tests PRIVATE War3MapEditor::QtModuleTriggers War3MapEditor::QtModuleObjects)

target_link_libraries(war3-map-variable-dialog-tests PRIVATE War3MapEditor::QtModuleTriggers)

target_link_libraries(war3-map-native-workbench-tests PRIVATE War3MapEditor::QtModuleVariables War3MapEditor::QtModuleAssets War3MapEditor::QtModuleTriggers War3MapEditor::QtModuleObjects War3MapEditor::QtModuleManager)

target_link_libraries(war3-map-project-watch-tests PRIVATE War3MapEditor::QtModuleVariables War3MapEditor::QtModuleAssets War3MapEditor::QtModuleTriggers War3MapEditor::QtModuleObjects War3MapEditor::QtModuleManager)

qt_add_executable(war3-map-variables-module-tests VariablesModuleTests.cpp)
target_link_libraries(war3-map-variables-module-tests PRIVATE War3MapEditor::QtModuleVariables Qt6::Test)
target_compile_options(war3-map-variables-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-variables-module COMMAND war3-map-variables-module-tests)
set_tests_properties(qt-variables-module PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

# 资源模块：导入队列去重 / 移除 / 清空、标签切换、在真实 Shell 中移动保持同一控件。
qt_add_executable(war3-map-assets-module-tests AssetsModuleTests.cpp)
target_link_libraries(war3-map-assets-module-tests PRIVATE War3MapEditor::QtModuleAssets Qt6::Test)
qt_add_executable(war3-map-campaign-module-tests CampaignModuleTests.cpp)
target_link_libraries(war3-map-campaign-module-tests PRIVATE War3MapEditor::QtModuleCampaign Qt6::Test)
target_compile_definitions(war3-map-campaign-module-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-campaign-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-campaign-module COMMAND war3-map-campaign-module-tests)
set_tests_properties(qt-campaign-module PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
target_compile_options(war3-map-assets-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-assets-module COMMAND war3-map-assets-module-tests)
set_tests_properties(qt-assets-module PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

target_link_libraries(war3-map-object-catalog-widget-tests PRIVATE War3MapEditor::QtModuleObjects)

# 触发器模块：地图脚本 war3map.j 只读查看、编码回退与脚本内搜索。
qt_add_executable(war3-map-trigger-module-tests TriggerModuleTests.cpp)
target_link_libraries(war3-map-trigger-module-tests PRIVATE War3MapEditor::QtModuleTriggers Qt6::Test)
target_compile_options(war3-map-trigger-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-trigger-module COMMAND war3-map-trigger-module-tests)
set_tests_properties(qt-trigger-module PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

# 取值面板对 YDWE 函数的支持：局部变量、Null 跟随 typename、AnyReturnType、scriptcode、替换函数参数。
qt_add_executable(war3-map-trigger-value-tests TriggerValueTests.cpp)
target_link_libraries(war3-map-trigger-value-tests PRIVATE War3MapEditor::QtModuleTriggers Qt6::Test)
target_compile_options(war3-map-trigger-value-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-trigger-values COMMAND war3-map-trigger-value-tests)
war3map_deploy_fonts(war3-map-trigger-value-tests)
set_tests_properties(qt-trigger-values PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

# 物体管理：概况、物体 / 触发器 / 变量 / 函数各页的引用、过滤、详情与视图状态。
qt_add_executable(war3-map-manager-module-tests ManagerModuleTests.cpp)
target_link_libraries(war3-map-manager-module-tests PRIVATE War3MapEditor::QtModuleManager Qt6::Test)
target_compile_options(war3-map-manager-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-manager-module COMMAND war3-map-manager-module-tests)
set_tests_properties(qt-manager-module PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-map-entities-tests MapEntitiesModuleTests.cpp)
target_link_libraries(war3-map-map-entities-tests PRIVATE War3MapEditor::QtModuleMapEntities Qt6::Test)
target_compile_options(war3-map-map-entities-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-map-entities COMMAND war3-map-map-entities-tests)
set_tests_properties(qt-map-entities PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-map-settings-tests MapSettingsModuleTests.cpp)
target_include_directories(war3-map-map-settings-tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(war3-map-map-settings-tests PRIVATE War3MapEditor::QtModuleMapSettings Qt6::Test)
target_compile_options(war3-map-map-settings-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-map-settings COMMAND war3-map-map-settings-tests)
set_tests_properties(qt-map-settings PROPERTIES TIMEOUT 40
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-sound-module-tests SoundModuleTests.cpp)
target_link_libraries(war3-map-sound-module-tests PRIVATE War3MapEditor::QtModuleSounds Qt6::Test)
target_compile_options(war3-map-sound-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-sound-module COMMAND war3-map-sound-module-tests)
set_tests_properties(qt-sound-module PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-object-direct-edit-tests ObjectDirectEditTests.cpp)
target_link_libraries(war3-map-object-direct-edit-tests PRIVATE War3MapEditor::QtModuleObjects Qt6::Test)
target_compile_options(war3-map-object-direct-edit-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-object-direct-edit COMMAND war3-map-object-direct-edit-tests)

qt_add_executable(war3-map-object-resource-picker-tests ObjectResourcePickerTests.cpp)
target_include_directories(war3-map-object-resource-picker-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-object-resource-picker-tests PRIVATE
    War3MapEditor::ObjectResourceAdapter Qt6::Test)
target_compile_options(war3-map-object-resource-picker-tests PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(war3-map-object-resource-picker-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
war3map_deploy_fonts(war3-map-object-resource-picker-tests)
add_test(NAME qt-object-resource-picker COMMAND war3-map-object-resource-picker-tests -o -,txt)
set_tests_properties(qt-object-resource-picker PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
set_tests_properties(qt-object-direct-edit PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-test-settings-tests MapTestSettingsTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/MapTestSettingsPage.cpp"
    "${PROJECT_SOURCE_DIR}/src/QtProject/MapSavePreferences.cpp")
target_include_directories(war3-map-test-settings-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-test-settings-tests PRIVATE War3MapEditor::QtAgent War3MapEditor::Core Qt6::Test)
target_compile_options(war3-map-test-settings-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-map-test-settings COMMAND war3-map-test-settings-tests)
set_tests_properties(qt-map-test-settings PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-recovery-settings-tests RecoverySettingsTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/RecoverySettingsPage.cpp")
target_include_directories(war3-map-recovery-settings-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-recovery-settings-tests PRIVATE War3MapEditor::QtAgent Qt6::Test)
target_compile_options(war3-map-recovery-settings-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-recovery-settings-tests)
add_test(NAME qt-recovery-settings COMMAND war3-map-recovery-settings-tests)
set_tests_properties(qt-recovery-settings PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-settings-layout-tests SettingsLayoutTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/MapTestSettingsPage.cpp"
    "${PROJECT_SOURCE_DIR}/src/QtApp/RecoverySettingsPage.cpp"
    "${PROJECT_SOURCE_DIR}/src/QtApp/WorldEffectsSettingsPage.cpp")
target_include_directories(war3-map-settings-layout-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-settings-layout-tests PRIVATE War3MapEditor::QtAgent War3MapEditor::Core War3MapEditor::QtUpdate Qt6::Test)
target_compile_options(war3-map-settings-layout-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-settings-layout-tests)
add_test(NAME qt-settings-layout COMMAND war3-map-settings-layout-tests)
add_test(NAME qt-settings-layout-dpr2 COMMAND war3-map-settings-layout-tests)
set_tests_properties(qt-settings-layout qt-settings-layout-dpr2 PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
set_tests_properties(qt-settings-layout-dpr2 PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=2")

qt_add_executable(war3-legacy-map-backup-check-tests LegacyMapBackupCheckTests.cpp)
target_link_libraries(war3-legacy-map-backup-check-tests PRIVATE War3MapEditor::QtProject Qt6::Test)
target_compile_options(war3-legacy-map-backup-check-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-legacy-map-backup-check COMMAND war3-legacy-map-backup-check-tests)
set_tests_properties(qt-legacy-map-backup-check PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-svg-icon-resource-tests SvgIconResourceTests.cpp)
target_link_libraries(war3-map-svg-icon-resource-tests PRIVATE War3MapEditor::QtUiKit Qt6::Test)
target_compile_options(war3-map-svg-icon-resource-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-svg-icon-resources COMMAND war3-map-svg-icon-resource-tests)
set_tests_properties(qt-svg-icon-resources PROPERTIES TIMEOUT 20
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-world-model-diagnostics-tests WorldModelDiagnosticsTests.cpp)
target_link_libraries(war3-map-world-model-diagnostics-tests PRIVATE War3MapEditor::QtModuleWorld Qt6::Test)
target_compile_definitions(war3-map-world-model-diagnostics-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-model-diagnostics-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-world-model-diagnostics-tests)
add_test(NAME qt-world-model-diagnostics COMMAND war3-map-world-model-diagnostics-tests)
set_tests_properties(qt-world-model-diagnostics PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-world-bug-placeholder-tests WorldBugPlaceholderTests.cpp)
target_link_libraries(war3-map-world-bug-placeholder-tests PRIVATE War3MapEditor::QtModuleWorld Qt6::Test)
target_compile_definitions(war3-map-world-bug-placeholder-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-world-bug-placeholder-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-world-bug-placeholder-tests)
add_test(NAME qt-world-bug-placeholder COMMAND war3-map-world-bug-placeholder-tests)
set_tests_properties(qt-world-bug-placeholder PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-object-ordered-picker-tests ObjectOrderedPickerTests.cpp)
target_link_libraries(war3-map-object-ordered-picker-tests PRIVATE War3MapEditor::QtModuleObjects Qt6::Test)
target_compile_options(war3-map-object-ordered-picker-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-object-ordered-picker COMMAND war3-map-object-ordered-picker-tests)
set_tests_properties(qt-object-ordered-picker PROPERTIES TIMEOUT 45
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
qt_add_executable(war3-mcp-code-check-tests McpCodeCheckToolsTests.cpp)
target_link_libraries(war3-mcp-code-check-tests PRIVATE War3MapEditor::McpProtocol Qt6::Test)
target_compile_definitions(war3-mcp-code-check-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-mcp-code-check-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-mcp-code-check-tests NO_GUI)
add_test(NAME qt-mcp-code-check COMMAND war3-mcp-code-check-tests)
set_tests_properties(qt-mcp-code-check PROPERTIES TIMEOUT 30
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-project-cache-revision-tests ProjectCacheRevisionTests.cpp)
target_link_libraries(war3-project-cache-revision-tests PRIVATE War3MapEditor::QtProject Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-project-cache-revision-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-project-cache-revision-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-project-cache-revision-tests)
add_test(NAME qt-project-cache-revision COMMAND war3-project-cache-revision-tests)
set_tests_properties(qt-project-cache-revision PROPERTIES TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-code-view-tests CodeViewTests.cpp)
target_link_libraries(war3-map-code-view-tests PRIVATE War3MapEditor::QtUiKit Qt6::Test)
target_compile_definitions(war3-map-code-view-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-code-view-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-code-view-tests)
war3map_deploy_fonts(war3-map-code-view-tests)
add_test(NAME qt-code-view COMMAND war3-map-code-view-tests)
set_tests_properties(qt-code-view PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-code-project-real-tests CodeCheckProjectRealTests.cpp)
target_link_libraries(war3-code-project-real-tests PRIVATE War3MapEditor::QtProject War3MapEditor::QtModuleTriggers Qt6::Test)
target_compile_definitions(war3-code-project-real-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-code-project-real-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-code-project-real-tests)
add_test(NAME qt-code-project-real COMMAND war3-code-project-real-tests)
set_tests_properties(qt-code-project-real PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-code-index-project-tests CodeIndexProjectTests.cpp)
target_link_libraries(war3-code-index-project-tests PRIVATE War3MapEditor::QtProject War3MapEditor::QtModuleTriggers Qt6::Test)
target_compile_definitions(war3-code-index-project-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-code-index-project-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-code-index-project-tests)
add_test(NAME qt-code-index-project COMMAND war3-code-index-project-tests)
set_tests_properties(qt-code-index-project PROPERTIES TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-recent-maps-tests RecentMapsTests.cpp)
target_link_libraries(war3-map-recent-maps-tests PRIVATE War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Test)
target_compile_definitions(war3-map-recent-maps-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-recent-maps-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-recent-maps-tests)
add_test(NAME qt-recent-maps COMMAND war3-map-recent-maps-tests)
set_tests_properties(qt-recent-maps PROPERTIES TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# GUI 与 MCP 的触发器引用重命名使用同一事务与重放身份。
qt_add_executable(war3-recovery-backup-cleanup-tests RecoveryBackupCleanupTests.cpp)
target_link_libraries(war3-recovery-backup-cleanup-tests PRIVATE War3MapEditor::QtProject Qt6::Test)
target_compile_definitions(war3-recovery-backup-cleanup-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-recovery-backup-cleanup-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-recovery-backup-cleanup COMMAND war3-recovery-backup-cleanup-tests)
set_tests_properties(qt-recovery-backup-cleanup PROPERTIES TIMEOUT 60
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-trigger-rename-tests TriggerRenameTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveObjectTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveObjectTools.h"
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveEditorTools.cpp"
    "${PROJECT_SOURCE_DIR}/src/QtApp/LiveEditorTools.h")
target_include_directories(war3-map-trigger-rename-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-trigger-rename-tests PRIVATE
    War3MapEditor::QtModuleTriggers War3MapEditor::QtProject War3MapEditor::McpProtocol War3MapEditor::ObjectResourceAdapter Qt6::Test)
target_compile_definitions(war3-map-trigger-rename-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-rename-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_qt_runtime(war3-map-trigger-rename-tests)
war3map_deploy_fonts(war3-map-trigger-rename-tests)
add_test(NAME qt-trigger-rename COMMAND war3-map-trigger-rename-tests)
set_tests_properties(qt-trigger-rename PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

qt_add_executable(war3-map-navigation-module-tests NavigationModuleTests.cpp)
target_link_libraries(war3-map-navigation-module-tests PRIVATE
    War3MapEditor::QtModuleTriggers War3MapEditor::QtModuleVariables
    War3MapEditor::QtModuleObjects War3MapEditor::QtProject
    War3MapEditor::QtShell War3MapEditor::QtUiKit Qt6::Test)
target_compile_definitions(war3-map-navigation-module-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-navigation-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-navigation-modules COMMAND war3-map-navigation-module-tests)
set_tests_properties(qt-navigation-modules PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-navigation-real-tests NavigationRealTests.cpp)
target_link_libraries(war3-map-navigation-real-tests PRIVATE
    War3MapEditor::QtModuleWorld War3MapEditor::QtModuleMapEntities Qt6::Test)
target_compile_definitions(war3-map-navigation-real-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-navigation-real-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-navigation-real COMMAND war3-map-navigation-real-tests)
set_tests_properties(qt-navigation-real PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-global-search-dialog-tests GlobalSearchDialogTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/GlobalSearchDialog.cpp")
target_include_directories(war3-map-global-search-dialog-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-global-search-dialog-tests PRIVATE War3MapEditor::Services War3MapEditor::QtUiKit Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-global-search-dialog-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-global-search-dialog-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-global-search-dialog COMMAND war3-map-global-search-dialog-tests)
set_tests_properties(qt-global-search-dialog PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
qt_add_executable(war3-map-search-reveal-module-tests SearchRevealModuleTests.cpp)
target_include_directories(war3-map-search-reveal-module-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-search-reveal-module-tests PRIVATE War3MapEditor::QtModuleTriggers War3MapEditor::QtModuleAssets War3MapEditor::MapSizeAnalysis
    War3MapEditor::QtModuleObjects War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Test)
target_compile_definitions(war3-map-search-reveal-module-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-search-reveal-module-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-search-reveal-modules COMMAND war3-map-search-reveal-module-tests)
set_tests_properties(qt-search-reveal-modules PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-project-search-capture-tests ProjectSearchCaptureTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/ProjectSearchCapture.cpp")
target_include_directories(war3-map-project-search-capture-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-project-search-capture-tests PRIVATE War3MapEditor::QtProject War3MapEditor::Services Qt6::Test)
target_compile_definitions(war3-map-project-search-capture-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-project-search-capture-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-project-search-capture COMMAND war3-map-project-search-capture-tests)
set_tests_properties(qt-project-search-capture PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-map-size-dialog-tests MapSizeDialogTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/MapSizeDialog.cpp")
target_include_directories(war3-map-map-size-dialog-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-map-size-dialog-tests PRIVATE War3MapEditor::MapSizeAnalysis War3MapEditor::QtUiKit Qt6::Widgets Qt6::Test)
target_compile_definitions(war3-map-map-size-dialog-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-map-size-dialog-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-map-size-dialog COMMAND war3-map-map-size-dialog-tests)
set_tests_properties(qt-map-size-dialog PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(war3-map-project-search-capture-real-tests ProjectSearchCaptureRealTests.cpp
    "${PROJECT_SOURCE_DIR}/src/QtApp/ProjectSearchCapture.cpp")
target_include_directories(war3-map-project-search-capture-real-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/QtApp")
target_link_libraries(war3-map-project-search-capture-real-tests PRIVATE War3MapEditor::QtProject War3MapEditor::Services Qt6::Test)
target_compile_definitions(war3-map-project-search-capture-real-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-project-search-capture-real-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME qt-project-search-capture-real COMMAND war3-map-project-search-capture-real-tests)
set_tests_properties(qt-project-search-capture-real PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# U4：真实共享服务、一次 Snapshot Undo 与非模态窗口回调合同。
qt_add_executable(war3-map-trigger-replace-dialog-tests TriggerReplaceDialogTests.cpp)
target_link_libraries(war3-map-trigger-replace-dialog-tests PRIVATE
    War3MapEditor::QtModuleTriggers War3MapEditor::QtUiKit War3MapEditor::Services Qt6::Test Qt6::Widgets)
target_compile_definitions(war3-map-trigger-replace-dialog-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-replace-dialog-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-trigger-replace-dialog-tests)
add_test(NAME qt-trigger-replace-dialog COMMAND war3-map-trigger-replace-dialog-tests -o -,txt)
set_tests_properties(qt-trigger-replace-dialog PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

# U4 大图验收仅消费显式环境变量的已拥有副本，缺输入跳过。
qt_add_executable(war3-map-trigger-replace-real-tests TriggerReplaceRealTests.cpp)
target_link_libraries(war3-map-trigger-replace-real-tests PRIVATE
    War3MapEditor::QtModuleTriggers War3MapEditor::QtUiKit War3MapEditor::Services Qt6::Test Qt6::Widgets Psapi)
target_compile_definitions(war3-map-trigger-replace-real-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-replace-real-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-trigger-replace-real-tests)
add_test(NAME qt-trigger-replace-real COMMAND war3-map-trigger-replace-real-tests -o -,txt)
set_tests_properties(qt-trigger-replace-real PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

# U4: one compact scalar command preserves the complete before/after identity.
qt_add_executable(war3-map-trigger-scalar-command-tests TriggerScalarCommandTests.cpp)
target_link_libraries(war3-map-trigger-scalar-command-tests PRIVATE
    War3MapEditor::QtProject Qt6::Test Qt6::Widgets)
target_compile_definitions(war3-map-trigger-scalar-command-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-scalar-command-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-trigger-scalar-command-tests)
add_test(NAME qt-trigger-scalar-command COMMAND war3-map-trigger-scalar-command-tests -o -,txt)
set_tests_properties(qt-trigger-scalar-command PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# U4 后台预览使用独立拥有的输入；单独核对取消、换源和窗口销毁。
qt_add_executable(war3-map-trigger-replace-async-tests TriggerReplaceAsyncTests.cpp)
target_link_libraries(war3-map-trigger-replace-async-tests PRIVATE
    War3MapEditor::QtModuleTriggers War3MapEditor::QtUiKit War3MapEditor::Services Qt6::Test Qt6::Widgets)
target_compile_definitions(war3-map-trigger-replace-async-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-replace-async-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-trigger-replace-async-tests)
add_test(NAME qt-trigger-replace-async COMMAND war3-map-trigger-replace-async-tests -o -,txt)
set_tests_properties(qt-trigger-replace-async PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

# U9：真实共用组件测试，不另编格式或业务 WTS 解析实现。
qt_add_executable(war3-map-warcraft-text-preview-tests WarcraftTextPreviewTests.cpp)
target_link_libraries(war3-map-warcraft-text-preview-tests PRIVATE War3MapEditor::QtUiKit Qt6::Test)
target_compile_definitions(war3-map-warcraft-text-preview-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-warcraft-text-preview-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-warcraft-text-preview-tests)
add_test(NAME qt-warcraft-text-preview COMMAND war3-map-warcraft-text-preview-tests -o -,txt)
set_tests_properties(qt-warcraft-text-preview PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

# 仅显式授权的已有地图副本；缺输入77，真实输入失败不能降为跳过。
qt_add_executable(war3-map-object-colour-preview-real-tests ObjectColourPreviewRealTests.cpp)
target_link_libraries(war3-map-object-colour-preview-real-tests PRIVATE
    War3MapEditor::QtModuleObjects War3MapEditor::QtProject War3MapEditor::QtUiKit
    War3MapEditor::WarcraftText Qt6::Test)
if(WIN32)
    target_link_libraries(war3-map-object-colour-preview-real-tests PRIVATE Psapi)
endif()
target_compile_definitions(war3-map-object-colour-preview-real-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-object-colour-preview-real-tests PRIVATE /W4 /permissive- /utf-8)
war3map_deploy_fonts(war3-map-object-colour-preview-real-tests)
add_test(NAME qt-object-colour-preview-real COMMAND war3-map-object-colour-preview-real-tests -o -,txt)
set_tests_properties(qt-object-colour-preview-real PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
