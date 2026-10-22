cmake_minimum_required(VERSION 3.24)

set(WAR3MAPEDITOR_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(War3MapTriggerStandalone LANGUAGES CXX)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "War3MapTrigger standalone checks require Windows x64")
    endif()
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    include(CTest)
    enable_testing()

    add_library(War3MapCore STATIC
        "${WAR3MAPEDITOR_REPOSITORY_ROOT}/src/Core/Game/WorldEditStrings.cpp"
        "${WAR3MAPEDITOR_REPOSITORY_ROOT}/src/Core/Hash.cpp"
        "${WAR3MAPEDITOR_REPOSITORY_ROOT}/src/Core/Types.cpp")
    target_include_directories(War3MapCore PUBLIC
        "${WAR3MAPEDITOR_REPOSITORY_ROOT}/include")
    target_compile_definitions(War3MapCore PRIVATE
        NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_link_libraries(War3MapCore PRIVATE bcrypt)
    if(MSVC)
        target_compile_options(War3MapCore PRIVATE /W4 /permissive- /utf-8)
    endif()
endif()

if(NOT TARGET War3MapCore)
    message(FATAL_ERROR "src/Trigger requires the War3MapCore target")
endif()

add_library(War3MapTrigger STATIC
    ScriptBuildService.cpp
    JassBackend/JassCompiler.cpp
    TriggerCodec.cpp
    TriggerCodeEditing.cpp
    GuiConversionSnapshots.cpp
    WctSource.cpp
    WtgDecoder.cpp
    WtgSource.cpp
    TriggerDataText.cpp
    TriggerDirectory.cpp
    TriggerEcaMove.cpp
    TriggerBlocks.cpp
    TriggerEditing.cpp
    TriggerEditBatch.cpp
    TriggerTransactionSnapshot.cpp
    TriggerGraphEditing.cpp
    TriggerArgumentReplacement.cpp
    TriggerPresentation.cpp
    TriggerTextNotation.cpp
    TriggerTypes.cpp
    TriggerVariableReference.cpp
    UiCallbackActions.cpp
    UiCatalog.cpp)

target_include_directories(War3MapTrigger
    PUBLIC
        "${WAR3MAPEDITOR_REPOSITORY_ROOT}/include")

target_link_libraries(War3MapTrigger PUBLIC War3MapCore)

target_compile_definitions(War3MapTrigger
    PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(War3MapTrigger PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(War3MapEditor::Trigger ALIAS War3MapTrigger)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND BUILD_TESTING)
    add_subdirectory(
        "${WAR3MAPEDITOR_REPOSITORY_ROOT}/tests/trigger"
        "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
