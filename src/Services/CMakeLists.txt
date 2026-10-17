# 服务层：跨模块的只读查询（工程级引用索引等）。只依赖 Core / 格式库，不依赖 Qt。
add_library(War3MapServices STATIC
    CampaignService.cpp
    CampaignSourceCapture.cpp
    ObjectCatalogSources.cpp
    CommandRegistry.cpp
    ProjectSearch.cpp
    TriggerReplace.cpp
    NameResolution.cpp
    TriggerStringEditing.cpp
    ProjectChanges.cpp
    WorldSelection.cpp
    WorldInstanceRegistry.cpp
    BuildingSplatService.cpp
    WorldModelBugPlaceholderService.cpp
    PlacementPresentationService.cpp
    PlacementCreation.cpp
    ProjectReferenceIndex.cpp
    AuxiliaryEditing.cpp
    AuxiliaryTransaction.cpp
    ObjectDeletionAssessment.cpp
    ObjectCommands.cpp
    ObjectResourceQuery.cpp
    ObjectResourceCatalog.cpp
    ObjectResourceFiles.cpp
    ObjectResourceInfo.cpp
    ObjectResourceDirectory.cpp
    ObjectTransactionSnapshot.cpp
    ObjectTransactionSaveGuards.cpp
    MapSaveService.cpp
    MapSaveScriptPreservation.cpp
    LuaTestReloadController.cpp
    MapCreateService.cpp
    ScriptSavePreparation.cpp
    CodeCheck.cpp
    CodeCheckProject.cpp
    CodeCheckProjectLua.cpp
    LuaRuntimeCatalog.cpp
    MapScriptTargetSelection.cpp
    LuaMapTargetConfiguration.cpp
    LuaMapSaveTarget.cpp
    MapEditSaveService.cpp
    MapEditSaveValidation.cpp
    MapDataQuery.cpp
    ImportInventoryQuery.cpp
    MapObjectReferenceCapture.cpp
    MapObjectReferenceSources.cpp
    ScriptMigrationPreflight.cpp
    LuaTriggerCandidate.cpp
    LuaSourceQueries.cpp
    LuaCandidateProfile.cpp
    TerrainBlightSaveService.cpp
    TerrainMinimapSave.cpp
    TerrainEditService.cpp)
add_library(War3MapEditor::Services ALIAS War3MapServices)
target_link_libraries(War3MapServices PUBLIC War3MapEditor::Core War3MapEditor::ObjectData War3MapTrigger War3MapEditor::UiLibrary War3MapEditor::ScriptBuild)
target_link_libraries(War3MapServices PUBLIC War3MapEditor::RenderSupport)
target_link_libraries(War3MapServices PUBLIC War3MapEditor::Lua)
target_link_libraries(War3MapServices PUBLIC War3MapEditor::RuntimeLuaApi)
target_compile_definitions(War3MapServices PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(War3MapServices PRIVATE /W4 /permissive- /utf-8)
endif()

# 地图体积分析：只读取已拥有的会话元数据，不引入 Qt、保存或资源正文读取。
add_library(War3MapMapSizeAnalysis STATIC MapSizeAnalysis.cpp)
add_library(War3MapEditor::MapSizeAnalysis ALIAS War3MapMapSizeAnalysis)
target_link_libraries(War3MapMapSizeAnalysis PUBLIC War3MapEditor::Core War3MapEditor::ObjectData)
target_compile_definitions(War3MapMapSizeAnalysis PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(War3MapMapSizeAnalysis PRIVATE /W4 /permissive- /utf-8)
endif()
