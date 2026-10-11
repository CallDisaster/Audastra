qt_add_library(War3MapQtModuleWorld STATIC WorldModule.cpp WorldModule.h WorldModuleEditing.cpp WorldViewport.cpp WorldViewport.h WorldViewportCameraFrame.cpp WorldViewportPicking.cpp WorldViewportInternals.h RenderPluginFailure.h
    WorldTerrainAssets.cpp WorldTerrainAssets.h WorldTerrainBuild.cpp WorldTerrainBuild.h WorldEnvironment.h WorldPlacementModels.cpp WorldPlacementModels.h
    WorldNavigation.cpp WorldNavigationBookmark.h
    WorldPanels.cpp WorldToolbarLayout.cpp WorldPanelWidgets.cpp WorldPanelWidgets.h WorldTerrainPanel.cpp WorldInspector.cpp
    WorldModelPreview.cpp WorldModelPreview.h
    WorldAnimationPlayer.cpp WorldAnimationPlayer.h WorldAnimationInstances.cpp WorldViewportAnimation.cpp WorldModuleAnimation.cpp WorldModuleInstances.cpp
    WorldAnimationDiagnostics.cpp WorldAnimationDiagnostics.h WorldInstanceTiming.h WorldAnimationPrefetch.cpp
    WorldSelectionPresentation.cpp WorldSelectionPresentation.h WorldSelectionPresenter.cpp WorldSelectionPresenter.h
    WorldBuildingSplatPresenter.cpp WorldBuildingSplatPresenter.h
    WorldModelBugPlaceholderPresenter.cpp WorldModelBugPlaceholderPresenter.h
    WorldPremPresenter.cpp WorldPremPresenter.h
    WorldPathingHelperPresenter.cpp WorldPathingHelperPresenter.h
    WorldPlacementPreview.cpp WorldPlacementPreview.h
    WorldPlacementPending.cpp WorldPlacementPending.h
    WorldOutlinerModel.cpp WorldOutlinerModel.h WorldSelectionAdapter.cpp
    WorldSelectionProperties.cpp WorldSelectionProperties.h
    WorldAssetBrowser.cpp WorldAssetBrowser.h WorldThumbnailCache.cpp WorldThumbnailCache.h
    WorldViewportEditorState.cpp WorldViewportVisibility.cpp
    WorldViewportMeasurement.cpp WorldFrameRecorder.cpp WorldFrameRecorder.h
    WorldScreenshotEncoder.cpp WorldScreenshotEncoder.h
    WorldViewportToolService.cpp WorldViewportToolService.h)
add_library(War3MapEditor::QtModuleWorld ALIAS War3MapQtModuleWorld)
target_include_directories(War3MapQtModuleWorld PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# 摆放面板复用物编的物体目录控件与条目（同一选择器，不另写一套）。
target_link_libraries(War3MapQtModuleWorld PUBLIC War3MapEditor::QtProject War3MapEditor::QtShell War3MapEditor::QtModuleObjects
    War3MapEditor::RenderSupport Qt6::Widgets)
target_compile_definitions(War3MapQtModuleWorld PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtModuleWorld PRIVATE /W4 /permissive- /utf-8)
