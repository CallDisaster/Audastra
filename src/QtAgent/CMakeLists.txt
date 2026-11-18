add_library(War3MapQtAgentCore STATIC
    AgentTypes.cpp
    TokenMeter.cpp TokenMeter.h
    ProjectMemoryService.cpp ProjectMemoryService.h
    ExternalMcpManager.cpp ExternalMcpManager.h
    AgentController.cpp
    AgentSessionStore.cpp
    AgentSettingsStore.cpp
    ApplicationSettingsTransfer.cpp
    ApplicationSettingsCommit.cpp
    MapBackupMigrationService.cpp MapBackupMigrationService.h
    RecoveryPreferences.h
    CredentialStore.cpp
    ProviderCatalog.cpp)
add_library(War3MapEditor::QtAgentCore ALIAS War3MapQtAgentCore)
target_include_directories(War3MapQtAgentCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtAgentCore PUBLIC Qt6::Core Qt6::Network PRIVATE Crypt32 War3MapEditor::Core War3MapEditor::ScriptBuild)
target_compile_definitions(War3MapQtAgentCore PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(AUDASTRA_DEV_SOURCE_FALLBACKS)
    target_compile_definitions(War3MapQtAgentCore PRIVATE WAR3MAP_AGENT_RUNTIME_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tools/agent-runtime")
endif()
target_compile_options(War3MapQtAgentCore PRIVATE /W4 /permissive- /utf-8)

add_library(War3MapQtAgent STATIC
    AgentSettingsDialog.cpp
    AgentWindow.cpp
    AgentConversationView.cpp
    AgentLayoutDiagnostics.cpp
    AnimatedCounter.cpp
    ComposerWidgets.cpp)
add_library(War3MapEditor::QtAgent ALIAS War3MapQtAgent)
target_include_directories(War3MapQtAgent PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtAgent PUBLIC War3MapEditor::QtAgentCore War3MapEditor::QtUiKit Qt6::Widgets)
target_compile_definitions(War3MapQtAgent PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtAgent PRIVATE /W4 /permissive- /utf-8)
