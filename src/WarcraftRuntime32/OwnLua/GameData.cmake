# 原有 Core 游戏来源优先级与 JASS parser 的 Win32 重编目标，不另造搜索器或词法器。
add_library(audastra-own-lua-game-data STATIC
    "${CMAKE_CURRENT_LIST_DIR}/NativeTypeHierarchy.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Core/Game/GameDataSource.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Core/Hash.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Core/Resources/ImportTable.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Core/Text/SlkCells.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Core/Text/TextUtil.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Core/Text/SourceEncoding.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Jass/JassLexer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../Jass/JassParser.cpp")
target_link_libraries(audastra-own-lua-game-data PUBLIC audastra-own-lua-mpq PRIVATE audastra-own-lua-host bcrypt)
target_include_directories(audastra-own-lua-game-data PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(audastra-own-lua-game-data PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(audastra-own-lua-game-data PRIVATE /utf-8 /W4 /permissive-)

add_library(audastra-own-lua-game STATIC "${CMAKE_CURRENT_LIST_DIR}/GameVm127.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GameImage127.cpp" "${CMAKE_CURRENT_LIST_DIR}/GameNativeProvider.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/GameNativeObserver.cpp" "${CMAKE_CURRENT_LIST_DIR}/OwnedLuaSession.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../Japi/GameSession.cpp")
target_link_libraries(audastra-own-lua-game PUBLIC audastra-own-lua-game-data audastra-own-lua-host
    audastra-japi-runtime PRIVATE audastra-own-lua-archive bcrypt audastra-detours)
target_compile_definitions(audastra-own-lua-game PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(audastra-own-lua-game PRIVATE /utf-8 /W4 /permissive-)
