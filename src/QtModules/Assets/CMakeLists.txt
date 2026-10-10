qt_add_library(War3MapQtModuleAssets STATIC
    AssetsModule.cpp AssetsModule.h
    AssetsActions.cpp AssetsDetail.h
    AssetPropertiesDialog.cpp AssetPropertiesDialog.h)
add_library(War3MapEditor::QtModuleAssets ALIAS War3MapQtModuleAssets)
target_include_directories(War3MapQtModuleAssets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleAssets PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell Qt6::Widgets)
target_compile_definitions(War3MapQtModuleAssets PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleAssets PRIVATE /W4 /permissive- /utf-8)
