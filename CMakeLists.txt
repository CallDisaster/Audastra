cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AUD_VERSION_SOURCE)
string(STRIP "${AUD_VERSION_SOURCE}" AUD_VERSION_SOURCE)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" AUD_VERSION_NUMERIC "${AUD_VERSION_SOURCE}")
project(War3MapEditor64 VERSION ${AUD_VERSION_NUMERIC} LANGUAGES C CXX)

set(RELEASE_OWNER "CallDisaster" CACHE STRING "Public GitHub release owner")
set(RELEASE_REPOSITORY "Audastra" CACHE STRING "Public GitHub release repository")
# 正式发布的更新签名公钥（公开信息）；私钥只在发布者本机，不入库。置空则禁用安装更新。
set(UPDATE_PUBLIC_KEY_HEX "b44c85ea9fbc85c51884ac307788cdf145275935f11afa2d0ff6511c92751a26" CACHE STRING "Trusted Ed25519 public key; empty disables installation")
set(FEEDBACK_ENDPOINT "" CACHE STRING "Public HTTPS feedback endpoint; empty disables reporting")
set(SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
include(cmake/GenerateAudastraVersion.cmake)
add_custom_target(audastra-version-info ALL
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_ROOT=${SOURCE_ROOT}" "-DOUTPUT_DIR=${OUTPUT_DIR}"
        "-DRELEASE_OWNER=${RELEASE_OWNER}" "-DRELEASE_REPOSITORY=${RELEASE_REPOSITORY}"
        "-DUPDATE_PUBLIC_KEY_HEX=${UPDATE_PUBLIC_KEY_HEX}" "-DFEEDBACK_ENDPOINT=${FEEDBACK_ENDPOINT}"
        -P "${SOURCE_ROOT}/cmake/GenerateAudastraVersion.cmake"
    BYPRODUCTS "${OUTPUT_DIR}/AudastraVersion.generated.h" "${OUTPUT_DIR}/audastra-build-version.json"
    VERBATIM)
add_library(audastra-version INTERFACE)
target_include_directories(audastra-version INTERFACE "${OUTPUT_DIR}")
add_dependencies(audastra-version audastra-version-info)

if(NOT WIN32)
    message(FATAL_ERROR "War3MapEditor64 currently supports Windows x64 only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "War3MapEditor64 must be configured for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WAR3MAPEDITOR_BUILD_QT "Build the Qt 6 desktop application" ON)
option(WAR3MAPEDITOR_BUILD_TESTS "Build the test suite" ON)
# 开发构建可从源码树找 Agent 技能与运行时；发布构建须关闭，避免把本机源码路径编进程序。
option(AUDASTRA_DEV_SOURCE_FALLBACKS "Let development builds find agent skills and runtime in the source tree" ON)

# Vulkan SDK 只在构建期使用（头文件与 glslc / dxc 着色器编译器，供渲染后端）；不随程序分发。
# 未设置 VULKAN_SDK 环境变量时，配置时传 -DWAR3MAPEDITOR_VULKAN_SDK=<SDK 根目录>。
set(WAR3MAPEDITOR_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK root (build-time only)")
if(WAR3MAPEDITOR_VULKAN_SDK)
    set(Vulkan_ROOT "${WAR3MAPEDITOR_VULKAN_SDK}")
endif()
find_package(Vulkan 1.3 QUIET COMPONENTS glslc)
if(Vulkan_FOUND)
    # 只需要 dxc 命令行编译 HLSL，不链接 dxcompiler 库（SDK 不带其 Debug 版）。
    find_program(WAR3MAPEDITOR_DXC_EXECUTABLE dxc
        HINTS "${WAR3MAPEDITOR_VULKAN_SDK}/Bin" NO_DEFAULT_PATH)
    message(STATUS "Vulkan SDK ${Vulkan_VERSION}: ${Vulkan_INCLUDE_DIRS}; dxc: ${WAR3MAPEDITOR_DXC_EXECUTABLE}")
else()
    message(STATUS "Vulkan SDK not found; renderer shader tools unavailable")
endif()

include(cmake/War3RenderCore.cmake)
include(cmake/AudastraUpdateDependencies.cmake)

# Warcraft 文字格式的唯一纯 C++ 实现；界面组件不为此依赖地图业务 Core。
add_library(War3MapWarcraftText STATIC
    src/Core/Text/WarcraftText.cpp
    include/War3MapCore/Text/WarcraftText.h
    include/War3MapCore/Text/Utf8Scalar.h)
add_library(War3MapEditor::WarcraftText ALIAS War3MapWarcraftText)
target_include_directories(War3MapWarcraftText PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(War3MapWarcraftText PUBLIC cxx_std_20)
target_compile_options(War3MapWarcraftText PRIVATE /W4 /permissive- /utf-8)

add_subdirectory(src/Core)
add_subdirectory(src/Trigger)
add_subdirectory(src/ObjectData)
add_subdirectory(src/Jass)
add_subdirectory(src/Lua)
add_subdirectory(src/Runtime)
add_subdirectory(src/ScriptBuild)
include(cmake/AudastraWarcraftRuntime.cmake)
add_subdirectory(src/UiLibrary)
add_subdirectory(src/GameUi)
add_subdirectory(src/Services)
# 静态库存工具仅供显式审计，便携发行不附带本机运行库或该工具。
add_executable(audastra-lua-api-inventory EXCLUDE_FROM_ALL tools/runtime/DumpRuntimeLuaApi.cpp)
target_link_libraries(audastra-lua-api-inventory PRIVATE War3MapEditor::RuntimeLuaApi audastra-json)
target_compile_definitions(audastra-lua-api-inventory PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-lua-api-inventory PRIVATE /W4 /permissive- /utf-8)
# 本机运行库审计的小地图构造器，仅开发者显式构建，不进入便携包。
add_executable(audastra-lua-api-probe-map EXCLUDE_FROM_ALL tools/runtime/PrepareProbeMap.cpp)
target_link_libraries(audastra-lua-api-probe-map PRIVATE War3MapEditor::Services audastra-json)
target_compile_definitions(audastra-lua-api-probe-map PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-lua-api-probe-map PRIVATE /W4 /permissive- /utf-8)
add_subdirectory(src/Cli)
add_executable(audastra-updater src/Updater/Main.cpp)
target_link_libraries(audastra-updater PRIVATE War3MapEditor::Core audastra-json user32)
target_compile_definitions(audastra-updater PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-updater PRIVATE /W4 /permissive- /utf-8)
# 便携包根目录的 Audastra.exe：只启动 bin 下的编辑器；静态运行库，根目录不需要任何 DLL。
if(WIN32)
    enable_language(RC)
    configure_file(src/Launcher/Launcher.rc.in "${CMAKE_CURRENT_BINARY_DIR}/Launcher.rc" @ONLY)
    add_executable(audastra-launcher WIN32 src/Launcher/Main.cpp "${CMAKE_CURRENT_BINARY_DIR}/Launcher.rc")
    set_target_properties(audastra-launcher PROPERTIES OUTPUT_NAME Audastra
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(audastra-launcher PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
    target_compile_options(audastra-launcher PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(audastra-launcher PRIVATE user32)
endif()
add_executable(audastra-sign-update tools/update/SignManifest.cpp)
target_link_libraries(audastra-sign-update PRIVATE War3MapEditor::Core audastra-sodium)
target_compile_definitions(audastra-sign-update PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(audastra-sign-update PRIVATE /W4 /permissive- /utf-8)
add_subdirectory(src/RenderStub)
add_subdirectory(src/Render)

if(WAR3MAPEDITOR_BUILD_QT)
    find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS Core Gui Widgets Svg Multimedia Network Test)
    qt_standard_project_setup()
    include(cmake/War3MapDeploy.cmake)
    add_subdirectory(src/QtUiKit)
    add_subdirectory(src/QtAgent)
    add_subdirectory(src/QtUpdate)
    add_subdirectory(src/QtFeedback)
    add_subdirectory(src/QtProject)
    add_subdirectory(src/QtShell)
    add_subdirectory(src/QtModules/Variables)
    add_subdirectory(src/QtModules/MapEntities)
    add_subdirectory(src/QtModules/MapSettings)
    add_subdirectory(src/QtModules/Sounds)
    add_subdirectory(src/QtModules/Assets)
    add_subdirectory(src/QtModules/Triggers)
    add_subdirectory(src/QtModules/Objects)
    add_subdirectory(src/QtModules/Campaign)
    add_subdirectory(src/QtModules/Manager)
    add_subdirectory(src/QtModules/World)
    add_subdirectory(src/QtApp)
endif()

# 安全 MCP 工作区核心不依赖 Qt；协议适配器只在 Qt 可用时构建。
add_subdirectory(src/McpServer)

# 发布目录：cmake --build <build> --config Release --target war3map-dist
if(WAR3MAPEDITOR_BUILD_QT)
    war3map_add_dist_target()
endif()

if(WAR3MAPEDITOR_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
