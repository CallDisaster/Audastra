# 整图脚本生成：WE + YDTrigger 等价输出、预处理与外部语法检查流水线。只依赖 Core、物编与触发器库。
add_library(War3MapScriptBuild STATIC
    ExternalProcess.cpp
    MapTestSlk.cpp
    MapTestLaunch.cpp
    MapTestRuntime.cpp
    LuaTemplateTransaction.cpp
    LuaMapScript.cpp
    LuaAuthorCode.cpp
    LuaMapPipeline.cpp
    LuaTriggerEmitter.cpp
    LuaTriggerExpressions.cpp
    LuaTriggerTypes.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/LuaTriggerRuntimeSource.cpp
    MapGlobals.cpp
    MapInitializationPlan.cpp
    MapObjects.cpp
    MapScriptBuild.cpp
    MapPlayers.cpp
    MapScript.cpp
    GuiTriggerRecovery.cpp
    GuiTriggerSource.cpp
    ScriptInject.cpp
    ScriptNames.cpp
    ScriptQueries.cpp
    ScriptSaveBundle.cpp
    ScriptTemplate.cpp
    SlkTemplatePlan.cpp
    SlkTemplateProof.cpp
    SlkTemplateSession.cpp
    SlkTemplateMaterializer.cpp
    SlkCanonicalMaterializer.cpp
    SlkDataset.cpp
    SlkMapSourceSnapshot.cpp
    SlkOrderedCreation.cpp
    ScriptToolchain.cpp
    ScriptJapiPlaceholder.cpp
    ScriptToolchainLayout.cpp
    ScriptToolchainProfile.cpp
    ScriptFrameworkInputs.cpp
    ScriptBaseInputs.cpp
    ScriptUserSourcePlan.cpp
    ScriptSourceBinding.cpp
    ScriptInjectionSources.cpp
    ScriptPreprocessSources.cpp
    SoundQueries.cpp
    TriggerActions.cpp
    TriggerEmitter.cpp
    TriggerLocals.cpp
    TriggerValues.cpp
    YDTriggerStorage.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/LuaTemplateHostSource.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/LuaTemplateRunnerSource.cpp)
# 模板宿主与驱动 Lua 源码嵌入程序；保存时写入独立工作目录再由 YDWE lua.exe 执行。
foreach(embedded Host Runner)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/LuaTemplate${embedded}Source.cpp
        COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/LuaTemplate${embedded}.lua
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/LuaTemplate${embedded}Source.cpp
            -DSYMBOL=kLuaTemplate${embedded}Source -DNAMESPACE=War3Map::Script::detail
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/LuaTemplate${embedded}.lua ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
        VERBATIM)
endforeach()
add_library(War3MapEditor::ScriptBuild ALIAS War3MapScriptBuild)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/LuaTriggerRuntimeSource.cpp
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/LuaTriggerRuntime.lua
        -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/LuaTriggerRuntimeSource.cpp
        -DSYMBOL=kLuaTriggerRuntimeSource -DNAMESPACE=War3Map::Script::lua
        -P ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/LuaTriggerRuntime.lua ${PROJECT_SOURCE_DIR}/cmake/EmbedText.cmake
    VERBATIM)
target_link_libraries(War3MapScriptBuild PUBLIC War3MapEditor::Core War3MapEditor::ObjectData War3MapEditor::Jass War3MapEditor::Lua War3MapTrigger)
target_link_libraries(War3MapScriptBuild PRIVATE audastra-json)
target_compile_definitions(War3MapScriptBuild PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(War3MapScriptBuild PRIVATE /W4 /permissive- /utf-8)
endif()
