if(NOT DEFINED EDITOR OR NOT DEFINED FIXTURE_WRITER OR NOT DEFINED QT_BIN_DIR
   OR NOT DEFINED WORK_ROOT OR NOT DEFINED PROBE_CASE)
    message(FATAL_ERROR "世界探针测试缺少必要参数。")
endif()

# 每次保留独立证据；生成器只接收本次新目录，不覆盖并行或历史运行。
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
set(run_root "${WORK_ROOT}/${PROBE_CASE}-${run_id}")
set(workspace "${run_root}/input/workspace")
set(capture "${run_root}/output/capture")
file(MAKE_DIRECTORY "${run_root}/output")
set(fixture_arguments)
if(PROBE_CASE STREQUAL "empty-terrain" OR PROBE_CASE STREQUAL "focused-empty-terrain")
    list(APPEND fixture_arguments --world-terrain)
elseif(NOT PROBE_CASE STREQUAL "missing-terrain")
    message(FATAL_ERROR "未知世界探针测试用例。")
endif()
execute_process(COMMAND "${FIXTURE_WRITER}" "${workspace}" ${fixture_arguments}
    RESULT_VARIABLE fixture_exit TIMEOUT 10)
if(NOT "${fixture_exit}" STREQUAL "0")
    message(FATAL_ERROR "无法生成原创世界探针样例：${fixture_exit}")
endif()

# 使用真实插件在桌面范围外的本进程窗口绘制，不激活作者窗口或运行地图脚本。
set(probe_arguments --workspace "${workspace}" --exit-after-open --probe-world 0
    --probe-world-frame-only --probe-world-ready-timeout 15 --probe-world-output "${capture}")
if(PROBE_CASE STREQUAL "focused-empty-terrain")
    list(APPEND probe_arguments --probe-world-focus-host-key synthetic-missing-model
        --probe-world-sequence-index 0 --probe-world-elapsed-ms 0 --probe-world-global-ms 0)
endif()
function(run_world_probe exit_name stdout_name stderr_name)
    # Windows 图形驱动按 argv[0] 查找程序配置；使用原生路径，避免驱动将
    # 正斜线形式误当成整条命令行。参数、真实渲染后端和完成帧断言保持相同。
    file(TO_NATIVE_PATH "${EDITOR}" editor_native)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env "QT_QPA_PLATFORM=windows"
        "QT_PLUGIN_PATH=${QT_BIN_DIR}/../plugins" "PATH=${QT_BIN_DIR};$ENV{PATH}"
        "${editor_native}" ${probe_arguments}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    set(${exit_name} "${result}" PARENT_SCOPE)
    set(${stdout_name} "${output}" PARENT_SCOPE)
    set(${stderr_name} "${errors}" PARENT_SCOPE)
endfunction()
run_world_probe(probe_exit probe_stdout probe_stderr)
file(WRITE "${run_root}/output/probe.log" "${probe_stdout}")
file(WRITE "${run_root}/output/probe.stderr.log" "${probe_stderr}")
if(PROBE_CASE STREQUAL "missing-terrain")
    if(NOT "${probe_exit}" STREQUAL "4" OR NOT probe_stdout MATCHES "world-probe-outcome=terrain-load-failed"
       OR NOT probe_stdout MATCHES "MAP_MEMBER_UNAVAILABLE"
       OR EXISTS "${capture}/world.png" OR EXISTS "${capture}/world.json")
        message(FATAL_ERROR "缺失地形必须明确失败且不发布截图：${probe_exit}\n${probe_stdout}\n${probe_stderr}")
    endif()
    return()
endif()
if(PROBE_CASE STREQUAL "focused-empty-terrain")
    if(NOT "${probe_exit}" STREQUAL "4" OR NOT probe_stdout MATCHES "world-focus=refused-empty-model-scene"
       OR EXISTS "${capture}/world.png" OR EXISTS "${capture}/world.json")
        message(FATAL_ERROR "纯地形不得伪造模型/动画时钟取证：${probe_exit}\n${probe_stdout}\n${probe_stderr}")
    endif()
    return()
endif()
if(NOT "${probe_exit}" STREQUAL "0" OR NOT EXISTS "${capture}/world.png"
   OR NOT EXISTS "${capture}/world.json")
    message(FATAL_ERROR "原创纯地形未获得完成帧：${probe_exit}\n${probe_stdout}\n${probe_stderr}")
endif()
file(READ "${capture}/world.json" receipt)
string(JSON source GET "${receipt}" capture_source)
string(JSON model_scene GET "${receipt}" probe model_scene)
string(JSON frame GET "${receipt}" completion frame_number)
string(JSON metric_frame GET "${receipt}" probe capture_frame_metrics frame_number)
string(JSON revision GET "${receipt}" completion document_revision)
string(JSON width GET "${receipt}" completion width)
string(JSON height GET "${receipt}" completion height)
string(JSON camera_distance GET "${receipt}" completion camera distance)
string(JSON prem_applicable GET "${receipt}" probe prem_preview applicable)
string(JSON placeholder_applicable GET "${receipt}" probe bug_placeholders applicable)
if(NOT source STREQUAL "renderer_completed_frame" OR NOT model_scene STREQUAL "completed-empty"
   OR NOT frame GREATER 0 OR NOT frame STREQUAL metric_frame OR NOT revision GREATER 0
   OR NOT width GREATER 0 OR NOT height GREATER 0 OR NOT camera_distance GREATER 0
   OR prem_applicable OR placeholder_applicable)
    message(FATAL_ERROR "纯地形完成帧身份或无模型证据不正确：${receipt}")
endif()
file(READ "${capture}/world.png" png_header LIMIT 8 HEX)
if(NOT png_header STREQUAL "89504e470d0a1a0a")
    message(FATAL_ERROR "完成帧不是PNG文件。")
endif()
file(SHA256 "${capture}/world.png" original_image_sha)
file(SHA256 "${capture}/world.json" original_receipt_sha)
run_world_probe(duplicate_exit duplicate_stdout duplicate_stderr)
file(SHA256 "${capture}/world.png" image_sha)
file(SHA256 "${capture}/world.json" receipt_sha)
if(NOT "${duplicate_exit}" STREQUAL "4" OR NOT duplicate_stdout MATCHES "refused-existing-or-invalid-directory"
   OR NOT original_image_sha STREQUAL image_sha OR NOT original_receipt_sha STREQUAL receipt_sha)
    message(FATAL_ERROR "重复捕获必须拒绝覆盖历史证据。${duplicate_stdout}\n${duplicate_stderr}")
endif()
