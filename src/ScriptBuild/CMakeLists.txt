# 整图脚本生成：WE + YDTrigger 等价输出、预处理与外部语法检查流水线。只依赖 Core、物编与触发器库。

# 模板沙盒运行库：Lua 5.3.6 核心 + RUNTIME_SOURCE_MANIFEST 已逐文件核验的 auxlib/base/table/string/math/utf8。
# 不编译 io/os/package/debug/动态加载。以上游支持的 C++ 异常模式编译（宿主回调里的 C++ 拥有者可正常析构），
# 符号带 C++ 修饰，与 src/Lua 的 compile-only C 核心库同链不冲突；源文件语言属性只作用于本目录。
set(lua_sandbox_sources)
foreach(manifest IN ITEMS SOURCE_MANIFEST.json RUNTIME_SOURCE_MANIFEST.json)
    file(READ "${PROJECT_SOURCE_DIR}/third_party/lua53/${manifest}" lua_manifest)
    string(JSON lua_count LENGTH "${lua_manifest}" files)
    math(EXPR lua_last "${lua_count} - 1")
    foreach(index RANGE ${lua_last})
        string(JSON source GET "${lua_manifest}" files ${index} path)
        string(JSON expected GET "${lua_manifest}" files ${index} sha256)
        if(NOT EXISTS "${AUD_LUA53_SOURCE_DIR}/${source}" OR IS_DIRECTORY "${AUD_LUA53_SOURCE_DIR}/${source}")
            message(FATAL_ERROR "Lua runtime dependency missing: ${source}. Run tools/dev/prepare-update-deps.ps1.")
        endif()
        file(SHA256 "${AUD_LUA53_SOURCE_DIR}/${source}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Lua 5.3.6 source SHA256 mismatch: ${source}")
        endif()
        if(source MATCHES "\\.c$")
            list(APPEND lua_sandbox_sources "${AUD_LUA53_SOURCE_DIR}/${source}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES lua_sandbox_sources)
set_source_files_properties(${lua_sandbox_sources} PROPERTIES LANGUAGE CXX)
add_library(audastra-lua53-sandbox STATIC ${lua_sandbox_sources})
target_include_directories(audastra-lua53-sandbox PUBLIC "${AUD_LUA53_SOURCE_DIR}")
target_compile_definitions(audastra-lua53-sandbox PRIVATE _CRT_SECURE_NO_WARNINGS)

include("${PROJECT_SOURCE_DIR}/cmake/RuntimeModuleLoaders.cmake")
add_library(War3MapScriptBuild STATIC
    RuntimeModuleProfiles.cpp
    RuntimeModuleBoot.cpp
    ExternalProcess.cpp
    MapTestSlk.cpp
    GameScreenshot127.cpp
    MapTestLaunch.cpp
    MapTestRuntime.cpp
    LuaTemplateSandbox.cpp
    LuaTemplateSlkBridge.cpp
    TemplateImports.cpp
    LuaMapScript.cpp
    ClassicLuaCompatibility.cpp
    ClassicLuaSaveBundle.cpp
    ClassicLuaEntryPipeline.cpp
    LuaAuthorCode.cpp
    LuaCustomReloadAnalysis.cpp
    LuaMapPipeline.cpp
    LuaTriggerEmitter.cpp
    LuaTestReload.cpp
    LuaTriggerExpressions.cpp
    LuaTriggerTypes.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/LuaTriggerRuntimeSource.cpp
    MapGlobals.cpp
    MapInitializationPlan.cpp
    MapRandomInitialization.cpp
    MapObjects.cpp
    MapScriptBuild.cpp
    MapRuntimeBoot.cpp
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
    ${CMAKE_CURRENT_BINARY_DIR}/LuaTemplateSlkSource.cpp)
# 模板 Lua 源码嵌入程序。Slk 是进程内沙盒使用的原创 slk 库。
foreach(embedded Slk)
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
target_link_libraries(War3MapScriptBuild PRIVATE audastra-json audastra-lua53-sandbox)
target_include_directories(War3MapScriptBuild PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(War3MapScriptBuild PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(War3MapScriptBuild PRIVATE /W4 /permissive- /utf-8)
endif()
