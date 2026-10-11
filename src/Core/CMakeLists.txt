file(READ "${PROJECT_SOURCE_DIR}/profiles/classic-1.27a-zhCN.json" DEFAULT_PROFILE_JSON)
configure_file(
    CompatibilityProfiles.generated.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/CompatibilityProfiles.generated.h"
    @ONLY)

add_library(War3MapCore STATIC
    Campaign/CampaignInfo.cpp
    CapabilityRegistry.cpp
    MapArchiveInPlace.cpp
    MapArchiveSession.cpp
    Services/MapTestService.cpp
    Services/MapTestWorkspace.cpp
    ProjectStore.cpp
    EditRevision.cpp
    VerifiedFile.cpp
    MapDocument.cpp
    MapData/ClassicMapLayouts.cpp
    MapData/MapSettings.cpp
    MapData/MapSettingsWriter.cpp
    MapData/LniMapSettings.cpp
    MapData/MapPlacements.cpp
    MapData/MapPlacementsWriter.cpp
    MapData/MapAuxiliary.cpp
    MapData/MapAuxiliaryWriter.cpp
    MapData/StringTableWriter.cpp
    MapData/MapDataEditSession.cpp
    MapData/MapDataFieldEditing.cpp
    MapData/MapDataFieldPatchCodec.cpp
    MapData/MapDataPlacementEditing.cpp
    MapData/MapDataPlacementPatchCodec.cpp
    MapData/TerrainEditInspection.cpp
    MapData/MapFields.cpp
    MapData/MapDataEditSessionTerrain.cpp
    MapData/MapDataEditSessionTerrainInternal.h
    MapData/PlacementEditing.cpp
    MapData/MapTerrain.cpp
    MapData/MapPathing.cpp
    MapData/TerrainEditing.cpp
    MapData/TerrainRampEditing.cpp
    Text/TextUtil.cpp
    Text/SourceEncoding.cpp
    Text/SlkCells.cpp
    IO/FileBytes.cpp
    Resources/ImportTable.cpp
    Resources/ResourceResolver.cpp
    Resources/TerrainTable.cpp
    CompatibilityProfile.cpp
    EditorSession.cpp
    Game/GameDataSource.cpp
    Game/WorldEditStringLoader.cpp
    Game/WorldEditStrings.cpp
    Hash.cpp
    Image/BlpImage.cpp
    Image/BlpEncode.cpp
    Image/TextureImage.cpp
    Image/JpegDecoder.cpp
    MapInfo.cpp
    Types.cpp
    Workspace.cpp
    Mpq/ArchiveCandidateService.cpp
    Mpq/MpqArchive.cpp
    Mpq/MpqTableAudit.cpp
    Mpq/MpqCrypt.cpp
    Mpq/MpqDecompress.cpp
    Mpq/MpqAudioDecode.cpp
    Mpq/MpqDeflate.cpp
    Mpq/MpqExplode.cpp
    Mpq/MpqInflate.cpp
    Mpq/MpqWriter.cpp)

target_include_directories(War3MapCore
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(War3MapCore
    PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN)

target_compile_options(War3MapCore PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(War3MapCore PRIVATE bcrypt advapi32 version)
target_link_libraries(War3MapCore PUBLIC audastra-version)
target_sources(War3MapCore PRIVATE
    Update/SemanticVersion.cpp Update/UpdateManifest.cpp Update/UpdateSignature.cpp
    Update/UpdatePath.cpp Update/UpdateArchive.cpp Update/UpdatePlan.cpp
    Update/UpdateInstaller.cpp Update/UpdateInstallerFiles.cpp Update/UpdateInstallerProcess.cpp
    Update/UpdateInstallerJournal.cpp Update/UpdateInstallerRecovery.cpp
    Feedback/Privacy.cpp Feedback/Report.cpp Feedback/JsonCodec.cpp
    Feedback/FeedbackStorage.cpp Feedback/Queue.cpp Feedback/Export.cpp)
target_link_libraries(War3MapCore PRIVATE audastra-sodium audastra-miniz audastra-json)

add_library(War3MapEditor::Core ALIAS War3MapCore)

# 基础 Result/诊断定义来自锁定的共享仓库。
target_link_libraries(War3MapCore PUBLIC War3RenderCore::Contract)
