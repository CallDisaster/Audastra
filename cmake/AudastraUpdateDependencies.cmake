# 第三方源码/库放构建目录或显式本机缓存，不纳入仓库。
# 下载与 MSVC /MD 静态构建见 tools/dev/prepare-update-deps.ps1。
set(AUD_SODIUM_SOURCE_DIR "" CACHE PATH "Verified libsodium 1.0.22 source directory")
set(AUD_SODIUM_LIBRARY "" CACHE FILEPATH "libsodium static library built with the matching MSVC /MD runtime")
set(AUD_MINIZ_SOURCE_DIR "" CACHE PATH "Verified miniz 3.1.2 amalgamation directory")
set(AUD_JSON_INCLUDE_DIR "" CACHE PATH "Verified nlohmann/json 3.12.0 include directory")
foreach(required IN ITEMS
    "${AUD_SODIUM_SOURCE_DIR}/src/libsodium/include/sodium.h" "${AUD_SODIUM_LIBRARY}"
    "${AUD_MINIZ_SOURCE_DIR}/miniz.c" "${AUD_MINIZ_SOURCE_DIR}/miniz.h"
    "${AUD_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
    if(NOT EXISTS "${required}" OR IS_DIRECTORY "${required}")
        message(FATAL_ERROR "Update dependencies missing. Run tools/dev/prepare-update-deps.ps1 and pass its CMake paths.")
    endif()
endforeach()
function(audastra_check_dependency path expected)
    file(SHA256 "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Update dependency SHA256 mismatch: ${path}")
    endif()
endfunction()
audastra_check_dependency("${AUD_MINIZ_SOURCE_DIR}/miniz.c" "e2c1aeb66eef9191d8c3feb164db2def2335a61d039bf04ed849f6b042433b30")
audastra_check_dependency("${AUD_MINIZ_SOURCE_DIR}/miniz.h" "b53b62ed122e559b8f679e3cb787a0b0035fe87a58f909da0e44931678f4e85f")
audastra_check_dependency("${AUD_JSON_INCLUDE_DIR}/nlohmann/json.hpp" "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
add_library(audastra-sodium STATIC IMPORTED GLOBAL)
set_target_properties(audastra-sodium PROPERTIES
    IMPORTED_LOCATION "${AUD_SODIUM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${AUD_SODIUM_SOURCE_DIR}/src/libsodium/include"
    INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC
    INTERFACE_LINK_LIBRARIES advapi32)
add_library(audastra-miniz STATIC "${AUD_MINIZ_SOURCE_DIR}/miniz.c")
target_include_directories(audastra-miniz PUBLIC "${AUD_MINIZ_SOURCE_DIR}")
target_compile_definitions(audastra-miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
add_library(audastra-json INTERFACE)
target_include_directories(audastra-json INTERFACE "${AUD_JSON_INCLUDE_DIR}")
