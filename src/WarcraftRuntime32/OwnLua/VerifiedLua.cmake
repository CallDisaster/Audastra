# 与编辑器 compile-only 库分离。只在自有32位宿主中启用这些经过逐文件核验的标准库。
set(AUD_LUA53_SOURCE_DIR "" CACHE PATH "Verified official Lua 5.3.6 src cache directory")
set(own_lua_sources)
foreach(manifest IN ITEMS SOURCE_MANIFEST.json RUNTIME_SOURCE_MANIFEST.json)
    set(manifest_path "${CMAKE_CURRENT_LIST_DIR}/../../../third_party/lua53/${manifest}")
    file(READ "${manifest_path}" contents)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_path}")
    string(JSON count LENGTH "${contents}" files)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON name GET "${contents}" files ${index} path)
        string(JSON expected GET "${contents}" files ${index} sha256)
        if(NOT EXISTS "${AUD_LUA53_SOURCE_DIR}/${name}" OR IS_DIRECTORY "${AUD_LUA53_SOURCE_DIR}/${name}")
            message(FATAL_ERROR "Own Lua runtime dependency missing: ${name}. Pass the verified AUD_LUA53_SOURCE_DIR cache.")
        endif()
        file(SHA256 "${AUD_LUA53_SOURCE_DIR}/${name}" actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "Own Lua 5.3.6 source SHA256 mismatch: ${name}")
        endif()
        if(name MATCHES "\\.c$")
            list(APPEND own_lua_sources "${AUD_LUA53_SOURCE_DIR}/${name}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES own_lua_sources)
# 上游 Lua 明确支持 C++ 异常模式。宿主回调有 C++ 拥有者，不能让 C longjmp 跳过析构。
# 独立目标全体用 C++ 链接，不改上游文件，也不改变应用的 C 语法检查库。
set_source_files_properties(${own_lua_sources} PROPERTIES LANGUAGE CXX)
add_library(audastra-own-lua53 STATIC ${own_lua_sources})
target_include_directories(audastra-own-lua53 PUBLIC "${AUD_LUA53_SOURCE_DIR}")
target_compile_definitions(audastra-own-lua53 PRIVATE _CRT_SECURE_NO_WARNINGS)

add_library(audastra-own-lua-host STATIC "${CMAKE_CURRENT_LIST_DIR}/LuaHost.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/NativeSignature.cpp" "${CMAKE_CURRENT_LIST_DIR}/LuaMailboxRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/LuaNativeBindings.cpp" "${CMAKE_CURRENT_LIST_DIR}/NativeCallX86.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/NativeRegistry.cpp")
target_include_directories(audastra-own-lua-host PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../../include")
target_link_libraries(audastra-own-lua-host PRIVATE audastra-own-lua53)
target_compile_definitions(audastra-own-lua-host PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(audastra-own-lua-host PRIVATE /utf-8 /W4 /permissive-)
