# 编辑器是x64，经典游戏是x86。只编译小型独立桥，不复制游戏或创建完整项目分身。
include(ExternalProject)
set(AUDASTRA_RUNTIME32_BINARY_DIR "${CMAKE_BINARY_DIR}/warcraft-runtime32")
set(AUDASTRA_RUNTIME32_OUTPUT_DIR "${AUDASTRA_RUNTIME32_BINARY_DIR}/$<CONFIG>")
set(runtime_targets audastra-warcraft-launcher audastra-warcraft-bootstrap)
if(WAR3MAPEDITOR_BUILD_TESTS)
    set(runtime_targets audastra-runtime-fixture-tests audastra-runtime-witness-tests
        audastra-own-lua-protocol-tests audastra-own-lua-host-tests audastra-own-lua-native-tests
        audastra-own-lua-archive-tests audastra-own-lua-callback-tests audastra-own-lua-type-tests
        audastra-own-lua-code-tests audastra-own-lua-launch-tests audastra-game-ui-tests audastra-japi-runtime-tests
        audastra-map-runtime-tests audastra-owner-tests audastra-owner-fixture)
endif()
list(APPEND runtime_targets audastra-japi audastra-japi-loader audastra-map-runtime)
ExternalProject_Add(audastra-runtime32
    SOURCE_DIR "${PROJECT_SOURCE_DIR}/src/WarcraftRuntime32"
    BINARY_DIR "${AUDASTRA_RUNTIME32_BINARY_DIR}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM Win32
    CMAKE_ARGS "-DBUILD_TESTING=${WAR3MAPEDITOR_BUILD_TESTS}" "-DAUD_LUA53_SOURCE_DIR=${AUD_LUA53_SOURCE_DIR}"
        "-DWAR3_RENDER_CORE_SOURCE_DIR=${WAR3_RENDER_CORE_SOURCE_DIR}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config $<CONFIG>
        --target ${runtime_targets} -- /m:1 /nr:false /p:CL_MPCount=1 /v:minimal
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
        "${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-launcher.exe"
        "${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-bootstrap.dll"
        "${AUDASTRA_RUNTIME32_OUTPUT_DIR}/warvk_runtime.dll")
