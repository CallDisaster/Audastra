if(NOT DEFINED EDITOR OR NOT DEFINED FIXTURE_WRITER OR
   NOT DEFINED QT_BIN_DIR OR NOT DEFINED WORKSPACE OR
   NOT DEFINED EXPECTED_EXIT_CODE OR NOT DEFINED GENERATE_FIXTURE OR
   NOT DEFINED REMOVE_UNIT_FIXTURE OR NOT DEFINED REQUIRE_LOADED_WORKSPACE)
    message(FATAL_ERROR "启动探针测试缺少必要参数。")
endif()

if(GENERATE_FIXTURE)
    set(fixture_extra_arguments)
    if(CUSTOM_TEXT_FIXTURE)
        list(APPEND fixture_extra_arguments --custom-text)
    endif()
    execute_process(
        COMMAND "${FIXTURE_WRITER}" "${WORKSPACE}" ${fixture_extra_arguments}
        RESULT_VARIABLE fixture_exit_code
        OUTPUT_VARIABLE fixture_stdout
        ERROR_VARIABLE fixture_stderr
        TIMEOUT 10)
    if(NOT "${fixture_exit_code}" STREQUAL "0")
        message(FATAL_ERROR
            "生成合成工作区失败：退出码 ${fixture_exit_code}\n"
            "stdout:\n${fixture_stdout}\n"
            "stderr:\n${fixture_stderr}")
    endif()

    foreach(required_fixture IN ITEMS
            "ui/config"
            "ui/base/define.txt"
            "ui/base/event.txt"
            "ui/base/condition.txt"
            "ui/base/action.txt"
            "ui/base/call.txt"
            "map/war3map.wtg"
            "map/war3map.wct"
            "table/unit.ini")
        if(NOT EXISTS "${WORKSPACE}/${required_fixture}")
            message(FATAL_ERROR "合成工作区缺少 ${required_fixture}")
        endif()
    endforeach()
endif()

if(REMOVE_UNIT_FIXTURE)
    file(REMOVE "${WORKSPACE}/table/unit.ini")
endif()

set(probe_arguments --workspace "${WORKSPACE}")
if(REQUIRE_LOADED_WORKSPACE)
    list(APPEND probe_arguments --require-loaded-workspace)
else()
    list(APPEND probe_arguments --exit-after-open)
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "QT_QPA_PLATFORM=offscreen"
        "QT_PLUGIN_PATH=${QT_BIN_DIR}/../plugins"
        "PATH=${QT_BIN_DIR};$ENV{PATH}"
        "${EDITOR}" ${probe_arguments}
    RESULT_VARIABLE actual_exit_code
    OUTPUT_VARIABLE process_stdout
    ERROR_VARIABLE process_stderr
    TIMEOUT 30)

if(NOT "${actual_exit_code}" STREQUAL "${EXPECTED_EXIT_CODE}")
    message(FATAL_ERROR
        "启动探针退出码错误：期望 ${EXPECTED_EXIT_CODE}，实际 ${actual_exit_code}\n"
        "stdout:\n${process_stdout}\n"
        "stderr:\n${process_stderr}")
endif()
