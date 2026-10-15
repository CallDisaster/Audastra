# 原创回归使用自行构建的官方 Lua CLI；不探测或启动本机 YDWE。
# core 源码已由 src/Lua 核验；这里补齐 CLI 和标准库，逐文件核验后仅用于测试。
set(test_lua_sources)
foreach(manifest IN ITEMS
    "${PROJECT_SOURCE_DIR}/third_party/lua53/RUNTIME_SOURCE_MANIFEST.json"
    "${CMAKE_CURRENT_LIST_DIR}/TestLuaSourceManifest.json")
    file(READ "${manifest}" test_lua_manifest)
    string(JSON test_lua_count LENGTH "${test_lua_manifest}" files)
    math(EXPR test_lua_last "${test_lua_count} - 1")
    foreach(index RANGE ${test_lua_last})
        string(JSON source GET "${test_lua_manifest}" files ${index} path)
        string(JSON expected GET "${test_lua_manifest}" files ${index} sha256)
        if(NOT EXISTS "${AUD_LUA53_SOURCE_DIR}/${source}" OR IS_DIRECTORY "${AUD_LUA53_SOURCE_DIR}/${source}")
            message(FATAL_ERROR "Test Lua CLI source missing: ${source}. Run tools/dev/prepare-update-deps.ps1.")
        endif()
        file(SHA256 "${AUD_LUA53_SOURCE_DIR}/${source}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Test Lua 5.3.6 source SHA256 mismatch: ${source}")
        endif()
        if(source MATCHES "\\.c$")
            list(APPEND test_lua_sources "${AUD_LUA53_SOURCE_DIR}/${source}")
        endif()
    endforeach()
endforeach()

add_executable(audastra-test-lua ${test_lua_sources})
target_link_libraries(audastra-test-lua PRIVATE audastra-lua53-core)
target_compile_definitions(audastra-test-lua PRIVATE _CRT_SECURE_NO_WARNINGS)

function(audastra_use_test_lua target)
    add_dependencies(${target} audastra-test-lua)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    target_compile_definitions(${target} PRIVATE
        "AUD_TEST_LUA_EXECUTABLE=\"$<TARGET_FILE:audastra-test-lua>\"")
endfunction()
