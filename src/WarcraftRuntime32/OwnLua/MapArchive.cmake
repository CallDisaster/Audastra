# 只重编 Core 的现有归档实现为 Win32；不保留第二套解码器。
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/CheckWar3RenderCore.cmake")
set(own_lua_core "${CMAKE_CURRENT_LIST_DIR}/../../Core")
add_library(audastra-own-lua-mpq STATIC
    "${own_lua_core}/Types.cpp"
    "${own_lua_core}/Mpq/MpqArchive.cpp"
    "${own_lua_core}/Mpq/MpqCrypt.cpp"
    "${own_lua_core}/Mpq/MpqDecompress.cpp"
    "${own_lua_core}/Mpq/MpqAudioDecode.cpp"
    "${own_lua_core}/Mpq/MpqDeflate.cpp"
    "${own_lua_core}/Mpq/MpqExplode.cpp"
    "${own_lua_core}/Mpq/MpqInflate.cpp"
    "${own_lua_core}/Mpq/MpqWriter.cpp")
target_include_directories(audastra-own-lua-mpq PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../../include" "${WAR3_RENDER_CORE_SOURCE_DIR}/include")
target_compile_definitions(audastra-own-lua-mpq PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(audastra-own-lua-mpq PRIVATE /utf-8 /W4 /permissive-)

add_library(audastra-own-lua-archive STATIC "${CMAKE_CURRENT_LIST_DIR}/LuaMapArchive.cpp")
target_link_libraries(audastra-own-lua-archive PRIVATE audastra-own-lua-mpq bcrypt)
target_include_directories(audastra-own-lua-archive PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(audastra-own-lua-archive PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(audastra-own-lua-archive PRIVATE /utf-8 /W4 /permissive-)
