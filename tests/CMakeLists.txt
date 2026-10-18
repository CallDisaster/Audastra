include_directories("${WAR3_RENDER_CORE_SOURCE_DIR}/tests")
include("${CMAKE_CURRENT_SOURCE_DIR}/helpers/TestLua.cmake")

add_executable(war3-map-cli-resource-check-tests ResourceCheckCommandTests.cpp
    "${PROJECT_SOURCE_DIR}/src/Cli/ResourceCheckCommand.cpp")
target_include_directories(war3-map-cli-resource-check-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Cli" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(war3-map-cli-resource-check-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-cli-resource-check-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-cli-resource-check-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME cli-resource-check COMMAND war3-map-cli-resource-check-tests)
set_tests_properties(cli-resource-check PROPERTIES TIMEOUT 90)

add_executable(war3-map-cli-archive-member-tests ArchiveMemberCommandTests.cpp
    "${PROJECT_SOURCE_DIR}/src/Cli/ArchiveMemberCommand.cpp")
target_include_directories(war3-map-cli-archive-member-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Cli")
target_link_libraries(war3-map-cli-archive-member-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-cli-archive-member-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-cli-archive-member-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME cli-archive-member-add COMMAND war3-map-cli-archive-member-tests)
set_tests_properties(cli-archive-member-add PROPERTIES TIMEOUT 60)

add_executable(war3-map-cli-terrain-query-tests CliTerrainQueryTests.cpp
    "${PROJECT_SOURCE_DIR}/src/Cli/TerrainQueryCommand.cpp")
target_include_directories(war3-map-cli-terrain-query-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Cli" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(war3-map-cli-terrain-query-tests PRIVATE War3MapEditor::RenderSupport audastra-json)
target_compile_definitions(war3-map-cli-terrain-query-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-cli-terrain-query-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME cli-terrain-query COMMAND war3-map-cli-terrain-query-tests)
set_tests_properties(cli-terrain-query PROPERTIES TIMEOUT 60)

add_executable(war3-map-cli-map-edit-save-tests CliMapEditSaveTests.cpp
    "${PROJECT_SOURCE_DIR}/src/Cli/MapEditSaveCommand.cpp")
target_include_directories(war3-map-cli-map-edit-save-tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Cli" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(war3-map-cli-map-edit-save-tests PRIVATE War3MapEditor::Services audastra-json)
target_compile_definitions(war3-map-cli-map-edit-save-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-cli-map-edit-save-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME cli-map-edit-save COMMAND war3-map-cli-map-edit-save-tests)
set_tests_properties(cli-map-edit-save PROPERTIES TIMEOUT 60)

add_executable(war3-map-source-encoding-compatibility-tests SourceEncodingCompatibilityTests.cpp)
target_link_libraries(war3-map-source-encoding-compatibility-tests PRIVATE War3MapEditor::Core)
target_compile_definitions(war3-map-source-encoding-compatibility-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-source-encoding-compatibility-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME source-encoding-shared-compatibility COMMAND war3-map-source-encoding-compatibility-tests)
set_tests_properties(source-encoding-shared-compatibility PROPERTIES TIMEOUT 30)

add_executable(war3-map-trigger-replace-tests TriggerReplaceTests.cpp)
target_link_libraries(war3-map-trigger-replace-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-trigger-replace-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-replace-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-trigger-replace COMMAND war3-map-trigger-replace-tests)
set_tests_properties(services-trigger-replace PROPERTIES TIMEOUT 90)

add_executable(audastra-project-save-cancellation-tests ProjectSaveCancellationTests.cpp)
target_link_libraries(audastra-project-save-cancellation-tests PRIVATE War3MapEditor::Core)
target_compile_definitions(audastra-project-save-cancellation-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-project-save-cancellation-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME core-project-save-cancellation COMMAND audastra-project-save-cancellation-tests)
set_tests_properties(core-project-save-cancellation PROPERTIES TIMEOUT 90)

foreach(code_jass_test IN ITEMS JassDeclarationIndex VJassSourceMap JassSymbols)
    if(code_jass_test STREQUAL "JassDeclarationIndex")
        set(code_jass_target war3-jass-declaration-index-tests)
        set(code_jass_name jass-declaration-index)
    elseif(code_jass_test STREQUAL "JassSymbols")
        set(code_jass_target war3-jass-symbols-tests)
        set(code_jass_name jass-symbols)
    else()
        set(code_jass_target war3-vjass-source-map-tests)
        set(code_jass_name jass-vjass-source-map)
    endif()
    add_executable(${code_jass_target} jass/${code_jass_test}Tests.cpp)
    target_link_libraries(${code_jass_target} PRIVATE War3MapEditor::Jass)
    target_compile_definitions(${code_jass_target} PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(${code_jass_target} PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME ${code_jass_name} COMMAND ${code_jass_target})
    set_tests_properties(${code_jass_name} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(war3-map-core-tests CoreTests.cpp)
add_executable(war3-map-test-service-tests services/MapTestServiceTests.cpp)
target_link_libraries(war3-map-test-service-tests PRIVATE War3MapEditor::Core)
target_compile_definitions(war3-map-test-service-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-test-service-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME core-map-test-service COMMAND war3-map-test-service-tests)

add_executable(war3-map-test-slk-tests services/MapTestSlkTests.cpp)
target_link_libraries(war3-map-test-slk-tests PRIVATE War3MapEditor::ScriptBuild War3MapEditor::ObjectData)
target_compile_definitions(war3-map-test-slk-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-test-slk-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-map-test-slk COMMAND war3-map-test-slk-tests)
set_tests_properties(services-map-test-slk PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

add_executable(war3-map-test-launch-fixture services/MapTestLaunchFixture.cpp)
target_sources(war3-map-test-launch-fixture PRIVATE "${PROJECT_SOURCE_DIR}/src/WarcraftRuntime32/OwnLua/LuaMailboxRuntime.cpp")
target_include_directories(war3-map-test-launch-fixture PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(war3-map-test-launch-fixture PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-test-launch-fixture PRIVATE /W4 /permissive- /utf-8)
target_link_options(war3-map-test-launch-fixture PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
add_executable(war3-map-test-launch-tests services/MapTestLaunchTests.cpp)
target_link_libraries(war3-map-test-launch-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-test-launch-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-test-launch-tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(war3-map-test-launch-tests war3-map-test-launch-fixture)
add_test(NAME services-map-test-launch COMMAND war3-map-test-launch-tests $<TARGET_FILE:war3-map-test-launch-fixture>)
set_tests_properties(services-map-test-launch PROPERTIES TIMEOUT 45)

add_executable(war3-map-test-runtime-tests services/MapTestRuntimeTests.cpp)
target_link_libraries(war3-map-test-runtime-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-test-runtime-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-test-runtime-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-map-test-runtime COMMAND war3-map-test-runtime-tests)
set_tests_properties(services-map-test-runtime PROPERTIES TIMEOUT 45)
add_test(NAME services-map-test-runtime32 COMMAND "${CMAKE_CTEST_COMMAND}"
    --test-dir "${AUDASTRA_RUNTIME32_BINARY_DIR}" -C $<CONFIG> -j 1 --output-on-failure --no-tests=error)
set_tests_properties(services-map-test-runtime32 PROPERTIES TIMEOUT 70)
target_link_libraries(
    war3-map-core-tests
    PRIVATE
        War3MapEditor::Core
        War3MapEditor::ObjectData
        war3-render-stub)
target_compile_definitions(war3-map-core-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-core-tests PRIVATE /W4 /permissive- /utf-8)
add_custom_command(
    TARGET war3-map-core-tests
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:war3-render-stub>"
        "$<TARGET_FILE_DIR:war3-map-core-tests>")
add_test(NAME core-foundation COMMAND war3-map-core-tests)

foreach(service_test IN ITEMS MapArchiveSession MapBackupInspection ProjectStore EditRevision VerifiedFile ImportTable ResourceResolver MapDocument MapDataEditSession MapDataFieldEditing MapDataPlacementEditing PlacementEditing PlacementCreateDelete TerrainEditing TerrainRampEditing TerrainSavePlan TerrainDerivedCodec PathingRestrictionEditing PathingSavePolicy MapSettings LniMapSettings MapPlacements MapAuxiliary CoreTextIo WorldEditStrings BlpImage BlpOpaqueQuantize BlpJpegEncode)
    add_executable(war3-map-${service_test}-tests ${service_test}Tests.cpp)
    target_link_libraries(war3-map-${service_test}-tests PRIVATE War3MapEditor::Core)
    target_compile_definitions(war3-map-${service_test}-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-${service_test}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME core-${service_test} COMMAND war3-map-${service_test}-tests)
endforeach()

foreach(backup_group IN ITEMS inventory identity budgets reparse)
    add_test(NAME core-map-backup-${backup_group} COMMAND war3-map-MapBackupInspection-tests ${backup_group})
    set_tests_properties(core-map-backup-${backup_group} PROPERTIES TIMEOUT 45)
endforeach()
target_link_libraries(war3-map-MapBackupInspection-tests PRIVATE audastra-json)
set_tests_properties(core-map-backup-reparse PROPERTIES SKIP_RETURN_CODE 77)

add_executable(war3-map-map-size-analysis-tests MapSizeAnalysisTests.cpp)
target_link_libraries(war3-map-map-size-analysis-tests PRIVATE War3MapEditor::MapSizeAnalysis)
target_compile_definitions(war3-map-map-size-analysis-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-map-size-analysis-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-map-size-analysis COMMAND war3-map-map-size-analysis-tests)
set_tests_properties(services-map-size-analysis PROPERTIES TIMEOUT 30)

add_executable(war3-map-map-size-real-tests MapSizeRealTests.cpp)
target_link_libraries(war3-map-map-size-real-tests PRIVATE War3MapEditor::MapSizeAnalysis audastra-json)
target_compile_definitions(war3-map-map-size-real-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-map-size-real-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-map-size-analysis-real COMMAND war3-map-map-size-real-tests)
set_tests_properties(services-map-size-analysis-real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)

foreach(command_service_test IN ITEMS CommandRegistry ProjectSearch ProjectChanges WorldSelection BuildingSplatService PlacementPresentationService)
    add_executable(war3-map-${command_service_test}-tests ${command_service_test}Tests.cpp)
    target_link_libraries(war3-map-${command_service_test}-tests PRIVATE War3MapEditor::Services)
    target_compile_definitions(war3-map-${command_service_test}-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-${command_service_test}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME services-${command_service_test} COMMAND war3-map-${command_service_test}-tests)
endforeach()

add_executable(war3-map-script-build-tests ScriptBuildTests.cpp)
add_executable(war3-map-trigger-transaction-save-tests services/TriggerTransactionSaveTests.cpp)
target_link_libraries(war3-map-trigger-transaction-save-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-trigger-transaction-save-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-transaction-save-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-trigger-transaction-save COMMAND war3-map-trigger-transaction-save-tests)
add_executable(war3-map-trigger-graph-editing-tests trigger/TriggerGraphEditingTests.cpp)
target_link_libraries(war3-map-trigger-graph-editing-tests PRIVATE War3MapEditor::Trigger)
target_compile_definitions(war3-map-trigger-graph-editing-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-trigger-graph-editing-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME trigger-graph-editing COMMAND war3-map-trigger-graph-editing-tests)
target_link_libraries(war3-map-script-build-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-script-build-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-script-build-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-build COMMAND war3-map-script-build-tests)

add_executable(war3-map-script-japi-placeholder-tests ScriptJapiPlaceholderTests.cpp)
target_link_libraries(war3-map-script-japi-placeholder-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-script-japi-placeholder-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-script-japi-placeholder-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-japi-placeholder COMMAND war3-map-script-japi-placeholder-tests)

add_executable(war3-map-script-object-property-tests ScriptObjectPropertyTests.cpp)
target_link_libraries(war3-map-script-object-property-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-script-object-property-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-script-object-property-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-object-property COMMAND war3-map-script-object-property-tests)

add_executable(war3-map-script-callback-action-tests ScriptCallbackActionTests.cpp)
target_link_libraries(war3-map-script-callback-action-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-script-callback-action-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-script-callback-action-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-callback-action COMMAND war3-map-script-callback-action-tests)

add_executable(war3-map-lua-trigger-unit-tests LuaTriggerUnitTests.cpp LuaTriggerCandidateTests.cpp)
audastra_use_test_lua(war3-map-lua-trigger-unit-tests)
target_link_libraries(war3-map-lua-trigger-unit-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-lua-trigger-unit-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN
    LUA_TEST_RUNNER="${CMAKE_CURRENT_SOURCE_DIR}/LuaTriggerRuntimeTests.lua")
target_compile_options(war3-map-lua-trigger-unit-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-lua-trigger-unit COMMAND war3-map-lua-trigger-unit-tests)
set_tests_properties(script-lua-trigger-unit PROPERTIES TIMEOUT 30)

add_executable(audastra-lua-test-reload-tests LuaTestReloadTests.cpp
    "${PROJECT_SOURCE_DIR}/src/WarcraftRuntime32/OwnLua/LuaMailboxRuntime.cpp"
    "${AUD_LUA53_SOURCE_DIR}/lauxlib.c" "${AUD_LUA53_SOURCE_DIR}/lbaselib.c" "${AUD_LUA53_SOURCE_DIR}/lmathlib.c")
target_link_libraries(audastra-lua-test-reload-tests PRIVATE War3MapEditor::Services audastra-lua53-core)
target_compile_definitions(audastra-lua-test-reload-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(audastra-lua-test-reload-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-lua-test-reload COMMAND audastra-lua-test-reload-tests)
set_tests_properties(script-lua-test-reload PROPERTIES TIMEOUT 30)

add_executable(audastra-lua-custom-reload-tests LuaCustomReloadTests.cpp
    "${AUD_LUA53_SOURCE_DIR}/lauxlib.c" "${AUD_LUA53_SOURCE_DIR}/lbaselib.c" "${AUD_LUA53_SOURCE_DIR}/lmathlib.c")
target_link_libraries(audastra-lua-custom-reload-tests PRIVATE War3MapEditor::Services audastra-lua53-core)
target_compile_definitions(audastra-lua-custom-reload-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(audastra-lua-custom-reload-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-lua-custom-reload COMMAND audastra-lua-custom-reload-tests)
set_tests_properties(script-lua-custom-reload PROPERTIES TIMEOUT 30)

add_executable(war3-map-initialization-plan-tests MapInitializationPlanTests.cpp)
target_link_libraries(war3-map-initialization-plan-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-initialization-plan-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-initialization-plan-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-map-initialization-plan COMMAND war3-map-initialization-plan-tests)

add_executable(war3-map-lua-map-script-tests LuaMapScriptTests.cpp)
audastra_use_test_lua(war3-map-lua-map-script-tests)
target_link_libraries(war3-map-lua-map-script-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-lua-map-script-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN
    LUA_MAP_TEST_RUNNER="${CMAKE_CURRENT_SOURCE_DIR}/LuaMapRuntimeTests.lua")
target_compile_options(war3-map-lua-map-script-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-lua-map-script COMMAND war3-map-lua-map-script-tests)
set_tests_properties(script-lua-map-script PROPERTIES TIMEOUT 30)

add_executable(audastra-classic-lua-compatibility-tests ClassicLuaCompatibilityTests.cpp)
target_link_libraries(audastra-classic-lua-compatibility-tests PRIVATE War3MapEditor::ScriptBuild audastra-lua53-core)
target_compile_definitions(audastra-classic-lua-compatibility-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-classic-lua-compatibility-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-classic-lua-compatibility COMMAND audastra-classic-lua-compatibility-tests)
set_tests_properties(script-classic-lua-compatibility PROPERTIES TIMEOUT 30)

add_executable(audastra-classic-lua-entry-tests ClassicLuaEntryPipelineTests.cpp)
target_link_libraries(audastra-classic-lua-entry-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(audastra-classic-lua-entry-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-classic-lua-entry-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-classic-lua-entry COMMAND audastra-classic-lua-entry-tests)
set_tests_properties(script-classic-lua-entry PROPERTIES TIMEOUT 30)

add_executable(war3-map-lua-map-pipeline-tests LuaMapPipelineTests.cpp)
audastra_use_test_lua(war3-map-lua-map-pipeline-tests)
target_link_libraries(war3-map-lua-map-pipeline-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-lua-map-pipeline-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-lua-map-pipeline-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-lua-map-pipeline COMMAND war3-map-lua-map-pipeline-tests)
set_tests_properties(script-lua-map-pipeline PROPERTIES TIMEOUT 30)

add_executable(war3-map-script-target-selection-tests services/MapScriptTargetSelectionTests.cpp)
target_link_libraries(war3-map-script-target-selection-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-script-target-selection-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-script-target-selection-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-script-target-selection COMMAND war3-map-script-target-selection-tests)

add_executable(war3-map-external-process-tests ExternalProcessTests.cpp)
target_link_libraries(war3-map-external-process-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-external-process-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-external-process-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-external-process COMMAND war3-map-external-process-tests)
set_tests_properties(script-external-process PROPERTIES TIMEOUT 30)

add_executable(war3-map-script-save-bundle-tests ScriptSaveBundleTests.cpp)
target_link_libraries(war3-map-script-save-bundle-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-script-save-bundle-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-script-save-bundle-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-save-bundle COMMAND war3-map-script-save-bundle-tests)
add_executable(war3-map-lua-template-tests LuaTemplateSandboxTests.cpp)
target_link_libraries(war3-map-lua-template-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-lua-template-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-lua-template-tests PRIVATE /W4 /permissive- /utf-8)
# 进程内模板沙盒；slk 部分只读 FORGE_YDWE_ROOT 下的 w3x2lni 数据，未设置时只跳过该部分。
add_test(NAME script-lua-template COMMAND war3-map-lua-template-tests)
set_tests_properties(script-lua-template PROPERTIES SKIP_RETURN_CODE 77)

add_executable(war3-map-script-overlay-tests MapScriptOverlayTests.cpp)
target_link_libraries(war3-map-script-overlay-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-script-overlay-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-script-overlay-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME script-map-overlay COMMAND war3-map-script-overlay-tests)

foreach(slk_part IN ITEMS Plan Proof Session Materializer)
    string(TOLOWER "${slk_part}" slk_part_name)
    add_executable(war3-map-slk-${slk_part_name}-tests SlkTemplate${slk_part}Tests.cpp)
    target_link_libraries(war3-map-slk-${slk_part_name}-tests PRIVATE War3MapEditor::ScriptBuild)
    target_compile_definitions(war3-map-slk-${slk_part_name}-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-slk-${slk_part_name}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME slk-template-${slk_part_name} COMMAND war3-map-slk-${slk_part_name}-tests)
endforeach()

add_executable(war3-map-slk-dataset-tests SlkDatasetTests.cpp)
target_link_libraries(war3-map-slk-dataset-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-map-slk-dataset-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-slk-dataset-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME slk-dataset COMMAND war3-map-slk-dataset-tests)

foreach(slk_service IN ITEMS MapSourceSnapshot OrderedCreation)
    add_executable(war3-map-slk-${slk_service}-tests Slk${slk_service}Tests.cpp)
    target_link_libraries(war3-map-slk-${slk_service}-tests PRIVATE War3MapEditor::ScriptBuild)
    target_compile_definitions(war3-map-slk-${slk_service}-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-slk-${slk_service}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME slk-${slk_service} COMMAND war3-map-slk-${slk_service}-tests)
endforeach()

add_executable(war3-map-render-d3d11-tests RenderD3D11Tests.cpp RealTerrainPairAudit.cpp RenderD3D11ModelSourceMipTests.h
    RenderD3D11TerrainTransitionTests.h)
target_include_directories(war3-map-render-d3d11-tests PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(war3-map-render-d3d11-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-render-d3d11-tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(war3-map-render-d3d11-tests war3-render-d3d11 war3-render-d3d11-test)
add_custom_command(TARGET war3-map-render-d3d11-tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:war3-render-d3d11>" "$<TARGET_FILE_DIR:war3-map-render-d3d11-tests>"
    VERBATIM)
add_custom_command(TARGET war3-map-render-d3d11-tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:war3-render-d3d11-test>" "$<TARGET_FILE_DIR:war3-map-render-d3d11-tests>"
    VERBATIM)
add_test(NAME render-d3d11-headless COMMAND war3-map-render-d3d11-tests)
set_tests_properties(render-d3d11-headless PROPERTIES TIMEOUT 60)
add_test(NAME render-d3d11-model-source-mips COMMAND war3-map-render-d3d11-tests --model-source-mips)
set_tests_properties(render-d3d11-model-source-mips PROPERTIES TIMEOUT 60)
add_test(NAME render-terrain-transition-seams COMMAND war3-map-render-d3d11-tests --terrain-transition-seams)
set_tests_properties(render-terrain-transition-seams PROPERTIES TIMEOUT 60)

add_executable(war3-map-terrain-tests MapTerrainTests.cpp)
target_link_libraries(war3-map-terrain-tests PRIVATE War3MapEditor::Core)
target_compile_definitions(war3-map-terrain-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-terrain-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME map-terrain COMMAND war3-map-terrain-tests)

add_executable(war3-terrain-edit-inspection-tests TerrainEditInspectionTests.cpp)
target_link_libraries(war3-terrain-edit-inspection-tests PRIVATE War3MapEditor::Core)
target_compile_definitions(war3-terrain-edit-inspection-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(war3-terrain-edit-inspection-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME terrain-edit-inspection COMMAND war3-terrain-edit-inspection-tests)

foreach(render_part IN ITEMS TerrainTable TerrainMaterials TerrainTextureAtlas BlpMipChain TerrainPicking TerrainInspection ViewRay TerrainGroundMesh TerrainMinimapCandidate TerrainMinimapPostprocess TerrainMinimapRepresentativeColor TerrainMinimapAssets ClassicMinimapCandidate WorldModelScene StaticModelTexsSampler TerrainCliffs SceneLighting BuildingSplatPreview OwnedStaticModel ClassicObjectTexture)
    add_executable(war3-map-${render_part}-tests ${render_part}Tests.cpp)
    target_link_libraries(war3-map-${render_part}-tests PRIVATE War3MapEditor::RenderSupport)
    target_compile_definitions(war3-map-${render_part}-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-${render_part}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME render-${render_part} COMMAND war3-map-${render_part}-tests)
endforeach()

target_link_libraries(war3-map-StaticModelTexsSampler-tests PRIVATE War3RenderCore::SkinInput)
target_link_libraries(war3-map-BuildingSplatPreview-tests PRIVATE War3MapEditor::Services War3RenderCore::SkinInput)

add_executable(war3-jass-budget-tests jass/JassBudgetTests.cpp)
target_link_libraries(war3-jass-budget-tests PRIVATE War3MapEditor::Jass)
target_compile_definitions(war3-jass-budget-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-jass-budget-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME jass-budget COMMAND war3-jass-budget-tests)
set_tests_properties(jass-budget PROPERTIES TIMEOUT 20)

add_executable(war3-jass-real-context-tests jass/JassRealContextTests.cpp)
add_executable(war3-jass-symbols-real-tests jass/JassRealContextTests.cpp)
target_link_libraries(war3-jass-symbols-real-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-jass-symbols-real-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN FORGE_SYMBOL_COLLECTION_REAL_GATE=1)
target_compile_options(war3-jass-symbols-real-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME jass-symbols-real COMMAND war3-jass-symbols-real-tests)
set_tests_properties(jass-symbols-real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

add_executable(war3-vjass-real-context-tests jass/VJassRealContextTests.cpp)
target_link_libraries(war3-vjass-real-context-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-vjass-real-context-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-vjass-real-context-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME jass-vjass-real-context COMMAND war3-vjass-real-context-tests)
set_tests_properties(jass-vjass-real-context PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
target_link_libraries(war3-jass-real-context-tests PRIVATE War3MapEditor::ScriptBuild)
target_compile_definitions(war3-jass-real-context-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-jass-real-context-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME jass-real-context COMMAND war3-jass-real-context-tests)
set_tests_properties(jass-real-context PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)

foreach(jass_test IN ITEMS Preprocessor Checker VJass)
    string(TOLOWER "${jass_test}" jass_test_name)
    add_executable(war3-map-jass-${jass_test_name}-tests Jass${jass_test}Tests.cpp)
    target_link_libraries(war3-map-jass-${jass_test_name}-tests PRIVATE War3MapEditor::Jass)
    target_compile_definitions(war3-map-jass-${jass_test_name}-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-jass-${jass_test_name}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME jass-${jass_test_name} COMMAND war3-map-jass-${jass_test_name}-tests)
endforeach()

add_subdirectory(trigger)
add_subdirectory(ui_library)
add_subdirectory(services)
add_subdirectory(object_data)
add_subdirectory(compat)
add_subdirectory(mpq)
add_subdirectory(mcp)

add_executable(audastra-update-tests update/UpdateTests.cpp)
target_include_directories(audastra-update-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(audastra-update-tests PRIVATE War3MapEditor::Core audastra-sodium audastra-miniz audastra-json)
target_compile_definitions(audastra-update-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-update-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME audastra-update-core COMMAND audastra-update-tests)
add_executable(audastra-launcher-paths-tests launcher/LauncherPathsTests.cpp)
target_include_directories(audastra-launcher-paths-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_options(audastra-launcher-paths-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME audastra-launcher-paths COMMAND audastra-launcher-paths-tests)
add_executable(audastra-update-installer-tests update/UpdateInstallerTests.cpp)
target_include_directories(audastra-update-installer-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(audastra-update-installer-tests PRIVATE War3MapEditor::Core audastra-sodium audastra-miniz audastra-json)
target_compile_definitions(audastra-update-installer-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-update-installer-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME audastra-update-installer COMMAND audastra-update-installer-tests)
set_tests_properties(audastra-update-installer PROPERTIES TIMEOUT 90)
add_executable(audastra-update-packaging-probe update/PackagingProbe.cpp)
target_link_libraries(audastra-update-packaging-probe PRIVATE War3MapEditor::Core audastra-json)
target_compile_definitions(audastra-update-packaging-probe PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-update-packaging-probe PRIVATE /W4 /permissive- /utf-8)

foreach(feedback_test IN ITEMS Privacy Queue Export)
    string(TOLOWER "${feedback_test}" feedback_test_name)
    add_executable(audastra-feedback-${feedback_test_name}-tests feedback/${feedback_test}Tests.cpp)
    target_include_directories(audastra-feedback-${feedback_test_name}-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(audastra-feedback-${feedback_test_name}-tests PRIVATE
        War3MapEditor::Core audastra-miniz audastra-json)
    target_compile_definitions(audastra-feedback-${feedback_test_name}-tests PRIVATE
        NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-feedback-${feedback_test_name}-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME audastra-feedback-${feedback_test_name} COMMAND audastra-feedback-${feedback_test_name}-tests)
endforeach()
add_subdirectory(warvk_codegen)

# 设计令牌生成物（Web CSS 变量与 C++ 主题常量）必须与 assets/design/tokens.json 一致。
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(NAME svg-icon-source-references
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/dev/check-svg-icons.py" --root "${PROJECT_SOURCE_DIR}")
add_test(NAME svg-icon-source-scanner-tests
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/dev/tests/test_check_svg_icons.py")
set_tests_properties(svg-icon-source-references svg-icon-source-scanner-tests PROPERTIES TIMEOUT 20
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(NAME p1-acceptance-scripts
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/dev/p1-acceptance-tests.py")
add_test(NAME p1-ydwe-host-utf8
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/dev/p1-host-enable-utf8-tests.py")
add_test(NAME p1-native-loader-scripts
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/dev/p1-native-loader-tests.py")
add_test(NAME p1-object-matrix-scripts
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/dev/tests/test_p1_object_matrix.py")
set_tests_properties(p1-acceptance-scripts p1-ydwe-host-utf8 p1-native-loader-scripts
    p1-object-matrix-scripts PROPERTIES TIMEOUT 120)
set_tests_properties(p1-ydwe-host-utf8 PROPERTIES SKIP_RETURN_CODE 77)
if(WIN32)
    find_program(FORGE_TEST_PWSH NAMES pwsh REQUIRED)
    # 只隔离原创 direct-save 窗口夹具；Default Desktop 观察合同保留原入口。
    add_executable(audastra-native-desktop-test-host native/NativeDesktopTestHost.cpp)
    target_compile_definitions(audastra-native-desktop-test-host PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-native-desktop-test-host PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(audastra-native-desktop-test-host PRIVATE user32 bcrypt)
    add_test(NAME p1-ydwe-direct-save-contract
        COMMAND audastra-native-desktop-test-host --powershell "${FORGE_TEST_PWSH}" --script
            "${PROJECT_SOURCE_DIR}/tools/dev/ydwe-save-direct-tests.ps1")
    set_tests_properties(p1-ydwe-direct-save-contract PROPERTIES TIMEOUT 120)
    add_test(NAME p1-input-desktop-ownership
        COMMAND "${FORGE_TEST_PWSH}" -NoLogo -NoProfile -NonInteractive -File
            "${PROJECT_SOURCE_DIR}/tools/dev/p1-input-desktop-tests.ps1")
    set_tests_properties(p1-input-desktop-ownership PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    add_test(NAME p1-input-desktop-observation
        COMMAND "${FORGE_TEST_PWSH}" -NoLogo -NoProfile -NonInteractive -File
            "${PROJECT_SOURCE_DIR}/tools/dev/p1-input-desktop-observation-tests.ps1")
    set_tests_properties(p1-input-desktop-observation PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    add_test(NAME audastra-update-packaging
        COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tests/update/PackageUpdateTests.py"
            --powershell "${FORGE_TEST_PWSH}"
            --package-script "${PROJECT_SOURCE_DIR}/tools/dev/package-update.ps1")
    set_tests_properties(audastra-update-packaging PROPERTIES TIMEOUT 120)
endif()
add_test(NAME design-tokens-generated
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/design_tokens.py" --check)
add_test(NAME p3-mpq-append-oracle
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/dev/tests/test_mpq_append_oracle.py")
set_tests_properties(p3-mpq-append-oracle PROPERTIES TIMEOUT 120
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

if(TARGET Qt6::Test)
    # 原创端到端目标使用独立编译期公钥。不得安装或加入产品发布清单。
    set(audastra_test_version_dir "${CMAKE_CURRENT_BINARY_DIR}/update-test-generated")
    file(MAKE_DIRECTORY "${audastra_test_version_dir}")
    configure_file(update/UpdaterTestBuildInfo.h.in
        "${audastra_test_version_dir}/AudastraVersion.generated.h" COPYONLY)
    add_executable(audastra-test-updater "${PROJECT_SOURCE_DIR}/src/Updater/Main.cpp")
    target_include_directories(audastra-test-updater BEFORE PRIVATE "${audastra_test_version_dir}")
    target_link_libraries(audastra-test-updater PRIVATE War3MapEditor::Core audastra-json user32)
    target_compile_definitions(audastra-test-updater PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-test-updater PRIVATE /W4 /permissive- /utf-8)
    foreach(fixture_minor IN ITEMS 1 2 3)
        add_executable(audastra-update-health-fixture-v${fixture_minor} update/UpdateHealthFixture.cpp)
        target_include_directories(audastra-update-health-fixture-v${fixture_minor} BEFORE PRIVATE "${audastra_test_version_dir}")
        target_link_libraries(audastra-update-health-fixture-v${fixture_minor} PRIVATE War3MapEditor::Core audastra-json)
        target_compile_definitions(audastra-update-health-fixture-v${fixture_minor} PRIVATE
            NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN
            AUDASTRA_UPDATE_HEALTH_FIXTURE_VERSION="0.${fixture_minor}.0"
            AUDASTRA_UPDATE_HEALTH_FIXTURE_COMMIT="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
        target_compile_options(audastra-update-health-fixture-v${fixture_minor} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
    add_subdirectory(qt)
endif()

include("${WAR3_RENDER_CORE_SOURCE_DIR}/src/RenderCore/TestTargets.cmake")
target_link_libraries(war3-render-skin-input-tests PRIVATE War3RenderCore::Contract)

# 第3步服务及WARP接线登记，业务实现不放在共享CMake。
include("${PROJECT_SOURCE_DIR}/src/Render/Animation/TestTargets.cmake")
include("${PROJECT_SOURCE_DIR}/src/Render/Particles/TestTargets.cmake")
target_link_libraries(war3-map-render-d3d11-tests PRIVATE War3MapEditor::RenderSupport War3MapEditor::Services audastra-json)

target_link_libraries(war3-map-render-d3d11-tests PRIVATE War3RenderCore::Contract)
add_executable(war3-map-frame-measurement-tests FrameMeasurementTests.cpp)
target_link_libraries(war3-map-frame-measurement-tests PRIVATE War3MapEditor::RenderSupport)
target_compile_options(war3-map-frame-measurement-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME render-frame-measurement COMMAND war3-map-frame-measurement-tests)
add_executable(war3-animation-preview-budget-tests AnimationPreviewBudgetTests.cpp)
target_link_libraries(war3-animation-preview-budget-tests PRIVATE War3MapEditor::RenderSupport War3RenderCore::SkinInput)
target_include_directories(war3-animation-preview-budget-tests PRIVATE "${WAR3_RENDER_CORE_SOURCE_DIR}/tests")
target_compile_options(war3-animation-preview-budget-tests PRIVATE /W4 /WX /permissive- /utf-8 /fp:precise)
foreach(group IN ITEMS controller admission)
    add_test(NAME model-animation-preview-budget-${group} COMMAND war3-animation-preview-budget-tests ${group})
    set_tests_properties(model-animation-preview-budget-${group} PROPERTIES TIMEOUT 60)
endforeach()

add_executable(war3-map-project-search-tests ProjectSearchTests.cpp)
target_link_libraries(war3-map-project-search-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-project-search-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-project-search-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-project-search COMMAND war3-map-project-search-tests)
set_tests_properties(services-project-search PROPERTIES TIMEOUT 60)

add_executable(war3-map-terrain-minimap-save-tests services/TerrainMinimapSaveTests.cpp)
target_link_libraries(war3-map-terrain-minimap-save-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-terrain-minimap-save-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-terrain-minimap-save-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-terrain-minimap-save COMMAND war3-map-terrain-minimap-save-tests)
set_tests_properties(services-terrain-minimap-save PROPERTIES TIMEOUT 120)

add_executable(war3-map-terrain-pathing-candidate-tests services/TerrainPathingRestrictionCandidateTests.cpp)
target_link_libraries(war3-map-terrain-pathing-candidate-tests PRIVATE War3MapEditor::Services)
target_compile_definitions(war3-map-terrain-pathing-candidate-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-terrain-pathing-candidate-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME services-terrain-pathing-candidate COMMAND war3-map-terrain-pathing-candidate-tests)
set_tests_properties(services-terrain-pathing-candidate PROPERTIES TIMEOUT 120)

# U9：纯文字格式与既有来源编码兼容入口，参数对应同一测试程序的独立组。
add_executable(war3-map-warcraft-text-tests WarcraftTextTests.cpp)
target_link_libraries(war3-map-warcraft-text-tests PRIVATE War3MapEditor::WarcraftText War3MapEditor::Core)
target_compile_definitions(war3-map-warcraft-text-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-warcraft-text-tests PRIVATE /W4 /permissive- /utf-8)
foreach(warcraft_text_group IN ITEMS format budgets source)
    add_test(NAME warcraft-text-${warcraft_text_group} COMMAND war3-map-warcraft-text-tests ${warcraft_text_group})
    set_tests_properties(warcraft-text-${warcraft_text_group} PROPERTIES TIMEOUT 30)
endforeach()

# 扫描器目标在 Render 中定义；测试必须在根 enable_testing() 之后登记。
if(TARGET war3-mdx-preview-scan-tests)
    add_test(NAME render-mdx-preview-scan COMMAND war3-mdx-preview-scan-tests)
    set_tests_properties(render-mdx-preview-scan PROPERTIES TIMEOUT 60)
endif()
if(TARGET war3-mdx-preview-scope-tests)
    add_test(NAME render-mdx-preview-scope COMMAND war3-mdx-preview-scope-tests $<TARGET_FILE:war3-mdx-scan>)
    set_tests_properties(render-mdx-preview-scope PROPERTIES TIMEOUT 60)
endif()
