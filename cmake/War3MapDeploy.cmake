# 运行时部署（Windows）。
#
# Qt 以动态库形式使用（LGPL，架构文档“授权”一节），不做单文件静态链接。
# 可执行文件隐式链接的 DLL 只能放在 exe 同目录，Windows 加载器才找得到；
# Qt 的 DLL 含数据导出，也不能延迟加载后再改搜索路径。插件、翻译、字体等可以放进子目录，由 qt.conf 指定。
#
# 1. 编辑器构建后自动运行 windeployqt 补齐 Qt DLL 与插件，构建目录里的 exe 可以直接双击。
# 2. 目标 war3map-dist 生成干净的发布目录 <build>/dist/War3MapEditor：
#      war3-map-editor.exe、war3-map-cli.exe、Qt*.dll、VC 运行库、qt.conf
#      plugins/  translations/  fonts/  licenses/
#    构建目录里的测试程序不进入发布目录。

find_program(WAR3MAP_WINDEPLOYQT NAMES windeployqt6 windeployqt
    HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" "${Qt6_DIR}/../../../bin"
    DOC "Qt windeployqt")

# VC 运行库随应用一起放在根目录（app-local），目标机器无需另装 VC Redist。
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)
set(WAR3MAP_RUNTIME_LIBS "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")

function(war3map_deploy_qt_runtime target)
    if(NOT WAR3MAP_WINDEPLOYQT)
        message(WARNING "${target}: windeployqt not found; run the exe with Qt bin on PATH")
        return()
    endif()
    # 构建目录里的 Qt 测试与编辑器同目录，会先加载这里的 Qt DLL 与 platforms/；
    # 一并部署 qoffscreen，否则测试找不到 offscreen 插件时 Qt 会弹模态错误框把测试卡死。
    # 中文翻译放到 exe 旁的 translations/，标准按钮（取消、关闭等）才显示中文。
    set(plugin_args --include-plugins qoffscreen)
    if("NO_GUI" IN_LIST ARGN)
        set(plugin_args --no-plugins)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${WAR3MAP_WINDEPLOYQT}" --verbose 0 --translations zh_CN --no-compiler-runtime
                --no-system-d3d-compiler --no-opengl-sw ${plugin_args} "$<TARGET_FILE:${target}>"
        COMMENT "windeployqt: Qt runtime for ${target}"
        VERBATIM)
    foreach(runtime IN LISTS WAR3MAP_RUNTIME_LIBS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endforeach()
endfunction()

function(war3map_add_dist_target)
    if(NOT WAR3MAP_WINDEPLOYQT)
        return()
    endif()
    set(dist_dir "${CMAKE_BINARY_DIR}/dist/War3MapEditor")
    add_custom_target(war3map-dist
        COMMAND "${CMAKE_COMMAND}"
            "-DDIST_DIR=${dist_dir}"
            "-DWINDEPLOYQT=${WAR3MAP_WINDEPLOYQT}"
            "-DEXECUTABLES=$<TARGET_FILE:war3-map-editor>|$<TARGET_FILE:war3-map-cli>|$<TARGET_FILE:audastra-updater>"
            "-DRUNTIME_LIBS=${WAR3MAP_RUNTIME_LIBS};$<TARGET_FILE:war3-render-d3d11>;${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-launcher.exe;${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-bootstrap.dll"
            "-DGAME_TEST_HELPERS=${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-launcher.exe|${AUDASTRA_RUNTIME32_OUTPUT_DIR}/audastra-warcraft-bootstrap.dll"
            "-DFONT_DIR=${PROJECT_SOURCE_DIR}/third_party/fonts/files"
            "-DLICENSE_DIR=${PROJECT_SOURCE_DIR}/third_party/fonts/LICENSES"
            "-DPROJECT_LICENSE=${PROJECT_SOURCE_DIR}/LICENSE.txt"
            "-DUPDATE_LICENSE_DIR=${PROJECT_SOURCE_DIR}/third_party/audastra-update"
            "-DTHIRD_PARTY_NOTICES=${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "-DDETOURS_LICENSE=${PROJECT_SOURCE_DIR}/third_party/detours/LICENSE.md"
            "-DLUA53_LICENSE=${PROJECT_SOURCE_DIR}/third_party/lua53/LICENSE.txt"
            -P "${PROJECT_SOURCE_DIR}/cmake/War3MapDist.cmake"
        DEPENDS war3-map-editor war3-map-cli audastra-updater audastra-runtime32
        COMMENT "Assembling ${dist_dir}"
        VERBATIM)
endfunction()
