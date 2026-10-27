# 物体编辑器模块：原版目录与地图覆盖、字段模型与委托、对象目录 / 选择弹层。
qt_add_library(War3MapQtModuleObjects STATIC
    ObjectModule.cpp ObjectModule.h ObjectModuleShared.h
    ObjectNavigation.cpp
    ObjectFieldsView.cpp
    ObjectCreation.cpp
    ObjectBatchActions.cpp
    ObjectTreeActions.cpp
    ObjectClassification.cpp ObjectClassification.h
    ObjectViewEntries.cpp ObjectViewEntries.h
    ObjectModels.cpp ObjectModels.h ObjectModelsDetail.h ObjectFieldModel.cpp ObjectFieldDelegate.cpp
    ObjectColourPreviewSource.cpp ObjectColourPreviewSource.h
    LevelAutofillDialog.cpp LevelAutofillDialog.h
    Widgets/ObjectResourcePicker.cpp Widgets/ObjectResourcePicker.h
    Widgets/ObjectCatalogWidgets.cpp Widgets/ObjectCatalogWidgets.h Widgets/ObjectCatalogDetail.h Widgets/ObjectPickerWidgets.cpp
    Widgets/ObjectTreeBrowser.cpp Widgets/ObjectTreeBrowser.h)
add_library(War3MapEditor::QtModuleObjects ALIAS War3MapQtModuleObjects)
target_include_directories(War3MapQtModuleObjects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtModuleObjects PUBLIC
    War3MapEditor::QtProject War3MapEditor::QtShell War3MapEditor::QtUiKit Qt6::Widgets)
target_compile_definitions(War3MapQtModuleObjects PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleObjects PRIVATE /W4 /permissive- /utf-8)
