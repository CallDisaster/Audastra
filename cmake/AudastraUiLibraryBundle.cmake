# 编辑器自带的触发器函数库包（AudastraUI）的发布内容。assets/ui-library 原样复制；
# AudastraUI.j 前面拼上 JAPI 客户端 sdk/japi/jass/AudastraJapi.j（包成 vJass library AudastraJapi）。
# 代码注入只扫描作者脚本、不追加被注入文件再引用的库，所以两者放进同一个注入文件。
# 结果：AUDASTRA_UI_LIBRARY_BUNDLE_DIR（包根目录）与 AUDASTRA_UI_LIBRARY_BUNDLE_FILES（全部文件）。
set(AUDASTRA_UI_LIBRARY_SOURCE_DIR "${PROJECT_SOURCE_DIR}/assets/ui-library")
set(AUDASTRA_UI_LIBRARY_BUNDLE_DIR "${PROJECT_BINARY_DIR}/ui-library")
set(_aui_japi "${PROJECT_SOURCE_DIR}/sdk/japi/jass/AudastraJapi.j")
set(_aui_script "AudastraUI/jass/AudastraUI.j")

file(GLOB_RECURSE _aui_sources CONFIGURE_DEPENDS RELATIVE "${AUDASTRA_UI_LIBRARY_SOURCE_DIR}"
    "${AUDASTRA_UI_LIBRARY_SOURCE_DIR}/*")
if(NOT _aui_script IN_LIST _aui_sources)
    message(FATAL_ERROR "AudastraUI 函数库缺少 ${_aui_script}")
endif()
set(AUDASTRA_UI_LIBRARY_BUNDLE_FILES)
foreach(_aui_relative IN LISTS _aui_sources)
    set(_aui_output "${AUDASTRA_UI_LIBRARY_BUNDLE_DIR}/${_aui_relative}")
    if(_aui_relative STREQUAL _aui_script)
        file(READ "${_aui_japi}" _aui_japi_text)
        file(READ "${AUDASTRA_UI_LIBRARY_SOURCE_DIR}/${_aui_relative}" _aui_ui_text)
        # 运行库包按 SHA 锁定这份客户端；检出换行（autocrlf）不同也必须拼出同样的字节。
        string(REPLACE "\r\n" "\n" _aui_japi_text "${_aui_japi_text}")
        string(REPLACE "\r\n" "\n" _aui_ui_text "${_aui_ui_text}")
        set(_aui_composed "library AudastraJapi\n${_aui_japi_text}\nendlibrary\n\n${_aui_ui_text}")
        # file(CONFIGURE) 只替换 @变量@；脚本里出现 @ 就拒绝，不让内容被悄悄改写。
        # file(WRITE) 在 Windows 会改成 CRLF，这里固定 LF，与源文件一致；内容不变时不改输出时间。
        string(FIND "${_aui_composed}" "@" _aui_at)
        if(NOT _aui_at EQUAL -1)
            message(FATAL_ERROR "AudastraUI.j 或 AudastraJapi.j 含 @，不能安全拼接")
        endif()
        file(CONFIGURE OUTPUT "${_aui_output}" CONTENT "${_aui_composed}" @ONLY NEWLINE_STYLE UNIX)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_aui_japi}" "${AUDASTRA_UI_LIBRARY_SOURCE_DIR}/${_aui_relative}")
    else()
        configure_file("${AUDASTRA_UI_LIBRARY_SOURCE_DIR}/${_aui_relative}" "${_aui_output}" COPYONLY)
    endif()
    list(APPEND AUDASTRA_UI_LIBRARY_BUNDLE_FILES "${_aui_output}")
endforeach()

if(WAR3MAPEDITOR_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # 同时检查实际注入包，避免只修源文件而仍分发旧的失败缓存逻辑。
    add_test(NAME audastra-ui-connect-retry-bundle COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/sdk/japi/tests/test_ui_connect.py"
        "${AUDASTRA_UI_LIBRARY_BUNDLE_DIR}/${_aui_script}" "${_aui_japi}")
    set_tests_properties(audastra-ui-connect-retry-bundle PROPERTIES TIMEOUT 30)
endif()
