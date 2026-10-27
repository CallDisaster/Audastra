add_library(War3MapQtAgent STATIC
    AgentTypes.cpp
    AgentController.cpp
    AgentSessionStore.cpp
    AgentSettingsStore.cpp
    ApplicationSettingsTransfer.cpp
    ApplicationSettingsCommit.cpp
    MapBackupMigrationService.cpp MapBackupMigrationService.h
    RecoveryPreferences.h
    CredentialStore.cpp
    ProviderCatalog.cpp
    AgentSettingsDialog.cpp
    AgentWindow.cpp
    AgentConversationView.cpp
    AgentLayoutDiagnostics.cpp
    AnimatedCounter.cpp
    ComposerWidgets.cpp)
add_library(War3MapEditor::QtAgent ALIAS War3MapQtAgent)
target_include_directories(War3MapQtAgent PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtAgent PUBLIC War3MapEditor::QtUiKit Qt6::Core Qt6::Widgets Qt6::Network PRIVATE Crypt32 War3MapEditor::Core)
target_compile_definitions(War3MapQtAgent PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(AUDASTRA_DEV_SOURCE_FALLBACKS)
    target_compile_definitions(War3MapQtAgent PRIVATE WAR3MAP_AGENT_RUNTIME_SOURCE_DIR="${PROJECT_SOURCE_DIR}/tools/agent-runtime")
endif()
target_compile_options(War3MapQtAgent PRIVATE /W4 /permissive- /utf-8)
