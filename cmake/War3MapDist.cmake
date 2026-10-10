# 由 war3map-dist 以 cmake -P 调用：从零组装发布目录（先清空，避免残留旧文件）。
cmake_minimum_required(VERSION 3.24)

foreach(required DIST_DIR WINDEPLOYQT EXECUTABLES GAME_TEST_HELPERS)
    if(NOT ${required})
        message(FATAL_ERROR "War3MapDist: ${required} is required")
    endif()
endforeach()

# 独立测试组件是产品必需项；缺失时在清理或部署目录之前停止。
string(REPLACE "|" ";" game_test_helpers "${GAME_TEST_HELPERS}")
foreach(helper IN LISTS game_test_helpers)
    if(NOT EXISTS "${helper}" OR IS_DIRECTORY "${helper}")
        message(FATAL_ERROR "War3MapDist: missing standalone game test helper: ${helper}")
    endif()
endforeach()

# 固定更新依赖已静态链接，发布前必须带上其许可原文。
foreach(update_license IN ITEMS json-LICENSE libsodium-LICENSE miniz-LICENSE)
    if(NOT UPDATE_LICENSE_DIR OR NOT EXISTS "${UPDATE_LICENSE_DIR}/${update_license}")
        message(FATAL_ERROR "War3MapDist: missing update dependency license: ${update_license}")
    endif()
endforeach()
if(NOT THIRD_PARTY_NOTICES OR NOT EXISTS "${THIRD_PARTY_NOTICES}")
    message(FATAL_ERROR "War3MapDist: third-party notices are required")
endif()
if(NOT DETOURS_LICENSE OR NOT EXISTS "${DETOURS_LICENSE}")
    message(FATAL_ERROR "War3MapDist: Detours license is required")
endif()
if(NOT LUA53_LICENSE OR NOT EXISTS "${LUA53_LICENSE}")
    message(FATAL_ERROR "Lua 5.3 MIT license is required for distribution")
endif()

# 只清空发布目录本身（位于构建树内），不触碰其它位置。
if(EXISTS "${DIST_DIR}")
    file(REMOVE_RECURSE "${DIST_DIR}")
endif()
file(MAKE_DIRECTORY "${DIST_DIR}")

string(REPLACE "|" ";" executables "${EXECUTABLES}")
foreach(executable IN LISTS executables)
    file(COPY "${executable}" DESTINATION "${DIST_DIR}")
    get_filename_component(name "${executable}" NAME)
    list(APPEND deployed "${DIST_DIR}/${name}")
endforeach()

# Qt DLL 放根目录（加载器要求），插件与翻译放子目录。
execute_process(
    # qoffscreen 供 --exit-after-open 等无界面探针使用（QT_QPA_PLATFORM=offscreen）。
    COMMAND "${WINDEPLOYQT}" --verbose 0 --no-compiler-runtime --no-system-d3d-compiler --no-opengl-sw
            --include-plugins qoffscreen --dir "${DIST_DIR}" --plugindir "${DIST_DIR}/plugins" --translations zh_CN ${deployed}
    RESULT_VARIABLE deploy_result)
if(NOT deploy_result EQUAL 0)
    message(FATAL_ERROR "windeployqt failed: ${deploy_result}")
endif()
file(WRITE "${DIST_DIR}/qt.conf" "[Paths]\nPrefix = .\nPlugins = plugins\nTranslations = translations\n")

foreach(library IN LISTS RUNTIME_LIBS)
    if(EXISTS "${library}")
        file(COPY "${library}" DESTINATION "${DIST_DIR}")
    endif()
endforeach()

if(FONT_DIR AND EXISTS "${FONT_DIR}")
    file(GLOB fonts "${FONT_DIR}/*.ttf" "${FONT_DIR}/*.otf")
    if(fonts)
        file(COPY ${fonts} DESTINATION "${DIST_DIR}/fonts")
    endif()
endif()
if(LICENSE_DIR AND EXISTS "${LICENSE_DIR}")
    file(COPY "${LICENSE_DIR}/" DESTINATION "${DIST_DIR}/licenses/fonts")
endif()
if(PROJECT_LICENSE AND EXISTS "${PROJECT_LICENSE}")
    file(COPY "${PROJECT_LICENSE}" DESTINATION "${DIST_DIR}/licenses")
endif()
foreach(update_license IN ITEMS json-LICENSE libsodium-LICENSE miniz-LICENSE)
    file(COPY "${UPDATE_LICENSE_DIR}/${update_license}" DESTINATION "${DIST_DIR}/licenses/update")
endforeach()
file(COPY "${THIRD_PARTY_NOTICES}" DESTINATION "${DIST_DIR}/licenses")
file(COPY_FILE "${DETOURS_LICENSE}" "${DIST_DIR}/licenses/Detours-LICENSE.txt")
file(COPY_FILE "${LUA53_LICENSE}" "${DIST_DIR}/licenses/Lua53-LICENSE.txt")
file(WRITE "${DIST_DIR}/licenses/Qt.txt"
    "本程序以动态库形式使用 Qt 6.8.3（LGPLv3）。Qt 的 DLL 位于程序目录，可自行替换为兼容版本。\n"
    "Qt 源代码：https://download.qt.io/archive/qt/6.8/6.8.3/\n")
message(STATUS "War3MapDist: ${DIST_DIR}")
