add_executable(war3-map-cli Main.cpp ScriptBuildCommand.cpp ScriptBuildCommand.h UiLibraryCommand.cpp UiLibraryCommand.h MapDataRoundtripCommand.cpp MapDataRoundtripCommand.h MapDataQueryCommand.cpp MapDataQueryCommand.h TerrainQueryCommand.cpp TerrainQueryCommand.h TerrainBlightSaveCommand.cpp TerrainBlightSaveCommand.h MapEditSaveCommand.cpp MapEditSaveCommand.h ObjectModelListCommand.cpp ObjectModelListCommand.h ObjectLayerCommand.cpp ObjectLayerCommand.h)
target_link_libraries(
    war3-map-cli
    PRIVATE
        War3MapEditor::Core
        War3MapEditor::Trigger
        War3MapEditor::ObjectData
        War3MapEditor::UiLibrary
        War3MapEditor::ScriptBuild
        War3MapEditor::Services
        War3MapEditor::RenderSupport)

target_sources(war3-map-cli PRIVATE ScriptMigrationCommand.cpp ScriptMigrationCommand.h)
target_sources(war3-map-cli PRIVATE MapCreateCommand.cpp MapCreateCommand.h)
target_sources(war3-map-cli PRIVATE MapTestCommand.cpp MapTestCommand.h)
target_sources(war3-map-cli PRIVATE WctSourceCommand.cpp WctSourceCommand.h)
target_sources(war3-map-cli PRIVATE WtgSourceCommand.cpp WtgSourceCommand.h)
target_sources(war3-map-cli PRIVATE TriggerTextCommand.cpp TriggerTextCommand.h)
target_sources(war3-map-cli PRIVATE LuaTriggerCandidateCommand.cpp LuaTriggerCandidateCommand.h)

target_sources(war3-map-cli PRIVATE ArchiveInspectCommand.cpp ArchiveInspectCommand.h)
target_sources(war3-map-cli PRIVATE ArchiveMemberCommand.cpp ArchiveMemberCommand.h)
target_sources(war3-map-cli PRIVATE ImportQueryCommand.cpp ImportQueryCommand.h)
target_sources(war3-map-cli PRIVATE UpdateCommand.cpp UpdateCommand.h)
target_sources(war3-map-cli PRIVATE NameQueryCommand.cpp NameQueryCommand.h)
target_sources(war3-map-cli PRIVATE ResourceCheckCommand.cpp ResourceCheckCommand.h)
target_sources(war3-map-cli PRIVATE GameExtractCommand.cpp GameExtractCommand.h)
target_link_libraries(war3-map-cli PRIVATE audastra-json)
target_compile_definitions(war3-map-cli PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-cli PRIVATE /W4 /permissive- /utf-8)
