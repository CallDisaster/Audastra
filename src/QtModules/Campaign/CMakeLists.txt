qt_add_library(War3MapQtModuleCampaign STATIC
    CampaignModule.cpp CampaignModule.h CampaignActions.cpp CampaignSettings.cpp CampaignShared.cpp)
add_library(War3MapEditor::QtModuleCampaign ALIAS War3MapQtModuleCampaign)
target_include_directories(War3MapQtModuleCampaign PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleCampaign PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell War3MapEditor::QtModuleObjects
    War3MapEditor::QtModuleAssets War3MapEditor::Services Qt6::Widgets)
target_compile_definitions(War3MapQtModuleCampaign PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleCampaign PRIVATE /W4 /permissive- /utf-8)
