add_executable(war3-map-mcp-workspace-tests McpWorkspaceTests.cpp)
target_link_libraries(
    war3-map-mcp-workspace-tests
    PRIVATE War3MapEditor::McpWorkspace)
target_compile_definitions(
    war3-map-mcp-workspace-tests
    PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(
    war3-map-mcp-workspace-tests
    PRIVATE /W4 /permissive- /utf-8)
add_test(NAME mcp-safe-workspace COMMAND war3-map-mcp-workspace-tests)

add_executable(war3-map-mcp-workspace-map-data-tests McpWorkspaceMapDataTests.cpp)
target_link_libraries(war3-map-mcp-workspace-map-data-tests PRIVATE War3MapEditor::McpWorkspace)
target_compile_definitions(war3-map-mcp-workspace-map-data-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-mcp-workspace-map-data-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME mcp-workspace-map-data COMMAND war3-map-mcp-workspace-map-data-tests)

add_executable(war3-map-mcp-workspace-placements-tests McpWorkspacePlacementTests.cpp)
target_link_libraries(war3-map-mcp-workspace-placements-tests PRIVATE War3MapEditor::McpWorkspace)
target_compile_definitions(war3-map-mcp-workspace-placements-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-mcp-workspace-placements-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME mcp-workspace-placements COMMAND war3-map-mcp-workspace-placements-tests)

add_executable(war3-map-mcp-object-reference-tests McpMapObjectReferenceTests.cpp)
target_link_libraries(war3-map-mcp-object-reference-tests PRIVATE War3MapEditor::McpWorkspace)
target_compile_definitions(war3-map-mcp-object-reference-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-mcp-object-reference-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME mcp-map-object-references COMMAND war3-map-mcp-object-reference-tests)

if(TARGET War3MapMcpProtocol)
    add_executable(audastra-mcp-terrain-edit-tests McpTerrainEditToolsTests.cpp McpStraightRampEditCases.h
        McpTerrainBlockTransportCases.h McpTerrainWorkspaceBlockCases.h TerrainResizeToolCases.h TerrainTextureSlotEditCases.h
        ../TerrainBlockFixtures.h ../TerrainConnectedRampFixtures.h)
    target_link_libraries(audastra-mcp-terrain-edit-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(audastra-mcp-terrain-edit-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-mcp-terrain-edit-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-terrain-edit COMMAND audastra-mcp-terrain-edit-tests)
    set_tests_properties(mcp-terrain-edit PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(audastra-mcp-world-viewport-tests McpWorldViewportToolsTests.cpp)
    target_link_libraries(audastra-mcp-world-viewport-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(audastra-mcp-world-viewport-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-mcp-world-viewport-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-world-viewport COMMAND audastra-mcp-world-viewport-tests)
    set_tests_properties(mcp-world-viewport PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(audastra-mcp-object-resource-tests McpObjectResourceToolsTests.cpp)
    target_link_libraries(audastra-mcp-object-resource-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(audastra-mcp-object-resource-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-mcp-object-resource-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-object-resource COMMAND audastra-mcp-object-resource-tests)
    set_tests_properties(mcp-object-resource PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-campaign-tests McpCampaignTests.cpp)
    target_link_libraries(war3-map-mcp-campaign-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-campaign-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-campaign-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-campaign COMMAND war3-map-mcp-campaign-tests)
    set_tests_properties(mcp-campaign PROPERTIES TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-product-tools-tests McpProductToolsTests.cpp)
    target_link_libraries(war3-map-mcp-product-tools-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-product-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-product-tools-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-product-tools COMMAND war3-map-mcp-product-tools-tests)
    set_tests_properties(mcp-product-tools PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-import-tools-tests McpImportToolsTests.cpp)
    target_link_libraries(war3-map-mcp-import-tools-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-import-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-import-tools-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-import-tools COMMAND war3-map-mcp-import-tools-tests)
    set_tests_properties(mcp-import-tools PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-placement-tools-tests McpPlacementToolsTests.cpp McpPlacementHostTests.cpp)
    target_link_libraries(war3-map-mcp-placement-tools-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-placement-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-placement-tools-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-placement-tools COMMAND war3-map-mcp-placement-tools-tests)
    set_tests_properties(mcp-placement-tools PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-map-data-tools-tests McpMapDataToolsTests.cpp)
    add_executable(audastra-mcp-auxiliary-tools-tests McpAuxiliaryToolsTests.cpp)
    target_link_libraries(audastra-mcp-auxiliary-tools-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(audastra-mcp-auxiliary-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-mcp-auxiliary-tools-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-auxiliary-tools COMMAND audastra-mcp-auxiliary-tools-tests)
    add_test(NAME mcp-auxiliary-archive-save COMMAND audastra-mcp-auxiliary-tools-tests --s6-archive-save)
    set_tests_properties(mcp-auxiliary-archive-save PROPERTIES TIMEOUT 120)
    set_tests_properties(mcp-auxiliary-tools mcp-auxiliary-archive-save PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    target_link_libraries(war3-map-mcp-map-data-tools-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-map-data-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-map-data-tools-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-map-data-tools COMMAND war3-map-mcp-map-data-tools-tests)
    set_tests_properties(mcp-map-data-tools PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-variable-tools-tests McpVariableToolsTests.cpp)
    target_link_libraries(war3-map-mcp-variable-tools-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-variable-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-variable-tools-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-variable-tools COMMAND war3-map-mcp-variable-tools-tests)
    set_tests_properties(mcp-variable-tools PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-effective-object-tests McpEffectiveObjectTests.cpp)
    target_link_libraries(war3-map-mcp-effective-object-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-effective-object-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-effective-object-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-effective-object-catalog COMMAND war3-map-mcp-effective-object-tests)
    set_tests_properties(mcp-effective-object-catalog PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-object-tools-tests McpObjectToolsTests.cpp)
    target_link_libraries(war3-map-mcp-object-tools-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-object-tools-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-object-tools-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-object-tools COMMAND war3-map-mcp-object-tools-tests)
    set_tests_properties(mcp-object-tools PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-terrain-tests McpTerrainTests.cpp McpStraightRampSaveCases.h McpTerrainBlockSaveCases.h McpTerrainPathingSaveCases.h TerrainResizeSaveCases.h TerrainTextureSlotSaveCases.h ../support/TerrainTextureSlotSaveFixture.h)
    target_link_libraries(war3-map-mcp-terrain-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-terrain-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-terrain-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-terrain-readonly COMMAND war3-map-mcp-terrain-tests)
    target_sources(war3-map-mcp-terrain-tests PRIVATE MapCustomSettingsSaveCases.h ../support/MapCustomSettingsFixture.h)
    set_tests_properties(mcp-terrain-readonly PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_executable(war3-map-mcp-protocol-tests McpProtocolTests.cpp)
    target_link_libraries(
        war3-map-mcp-protocol-tests
        PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(
        war3-map-mcp-protocol-tests
        PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(
        war3-map-mcp-protocol-tests
        PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-safe-tool-surface COMMAND war3-map-mcp-protocol-tests)
    set_tests_properties(
        mcp-safe-tool-surface
        PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()

if(TARGET War3MapMcpProtocol)
    add_executable(war3-map-mcp-project-search-tests McpProjectSearchToolsTests.cpp)
    target_link_libraries(war3-map-mcp-project-search-tests PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(war3-map-mcp-project-search-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp-project-search-tests PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME mcp-project-search COMMAND war3-map-mcp-project-search-tests)
    set_tests_properties(mcp-project-search PROPERTIES TIMEOUT 30
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
