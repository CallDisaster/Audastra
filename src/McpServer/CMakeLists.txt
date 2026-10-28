add_library(War3MapMcpWorkspace STATIC McpWorkspace.cpp McpWorkspaceObjects.cpp McpObjectSourceCapture.cpp McpWorkspaceMapData.cpp McpWorkspacePlacements.cpp McpWorkspaceProjectSearchCache.cpp McpWorkspaceTerrain.cpp McpWorkspaceAuxiliary.cpp)
add_library(War3MapEditor::McpWorkspace ALIAS War3MapMcpWorkspace)

target_include_directories(
    War3MapMcpWorkspace
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(
    War3MapMcpWorkspace
    PUBLIC
        War3MapEditor::Core
        War3MapEditor::Trigger
        War3MapEditor::Services)
target_compile_definitions(
    War3MapMcpWorkspace
    PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapMcpWorkspace PRIVATE /W4 /permissive- /utf-8)

if(TARGET Qt6::Core)
    add_library(War3MapMcpProtocol STATIC
        JsonRpc.cpp
        McpServer.cpp
        McpHttpTransport.cpp
        McpTerrain.cpp
        McpTerrainEditTools.cpp
        McpToolTerrainEdit.cpp
        McpTools.cpp
        McpLuaTestTools.cpp
        McpLuaFileTools.cpp McpLuaFileTools.h
        McpWorldViewportTools.cpp
        McpGameUiTools.cpp McpGameUiTools.h
        McpObjectTools.cpp
        McpToolObjects.cpp
        McpVariableTools.cpp
        McpToolVariables.cpp
        McpMapDataTools.cpp
        McpMapCustomSettingsTools.cpp McpMapCustomSettingsTools.h
        McpMapRuntimeTools.cpp McpMapRuntimeTools.h
        McpAuxiliaryTools.cpp McpAuxiliaryTools.h McpToolAuxiliary.cpp
        McpProductTools.cpp
        McpCodeCheckTools.cpp
        McpToolEntities.cpp
        McpImportTools.cpp
        McpObjectResourceTools.cpp McpObjectResourceTools.h McpToolObjectResources.cpp
        McpToolImports.cpp
        McpPlacementTools.cpp
        McpToolPlacements.cpp
        McpToolCommon.cpp
        McpToolTerrain.cpp
        McpToolModelAnimation.cpp
        McpToolMapEdit.cpp
        McpTerrainTextureSlotCommand.cpp McpTerrainTextureSlotCommand.h
        McpToolTriggerGraph.cpp
        McpTriggerCodeIdentity.cpp
        McpToolMapCreate.cpp
        McpToolCampaign.cpp
        McpToolReadOnly.cpp
        McpToolProjectSearch.cpp McpProjectSearchTools.cpp McpProjectSearchTools.h
        McpToolArchiveIdentity.cpp
        McpToolArchiveMembers.cpp
        McpToolWarVK.cpp
        McpToolEdit.cpp
        McpToolInternal.h
        McpResources.cpp)
    add_library(War3MapEditor::McpProtocol ALIAS War3MapMcpProtocol)
    target_include_directories(
        War3MapMcpProtocol
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${PROJECT_SOURCE_DIR}/include")
    target_link_libraries(
        War3MapMcpProtocol
        PUBLIC
            War3MapEditor::McpWorkspace
            War3MapEditor::ObjectData
            War3MapEditor::RenderSupport
            War3MapEditor::Services
            Qt6::Core
            Qt6::Network)
    target_compile_definitions(
        War3MapMcpProtocol
        PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(War3MapMcpProtocol PRIVATE /W4 /permissive- /utf-8)

    add_executable(war3-map-mcp Main.cpp)
    add_dependencies(war3-map-mcp war3-render-d3d11)
    add_custom_command(TARGET war3-map-mcp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:war3-render-d3d11>" "$<TARGET_FILE_DIR:war3-map-mcp>" VERBATIM)
    target_link_libraries(war3-map-mcp PRIVATE War3MapEditor::McpProtocol)
    target_compile_definitions(
        war3-map-mcp
        PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(war3-map-mcp PRIVATE /W4 /permissive- /utf-8)
endif()
