# 配置与每次实际构建复用同一份锁定核验，避免换提交后复用旧缓存。
include("${CMAKE_CURRENT_LIST_DIR}/CheckWar3RenderCore.cmake")
add_custom_target(forge-render-core-lock-check ALL
    COMMAND "${CMAKE_COMMAND}" --log-level=WARNING
        "-DWAR3_RENDER_CORE_SOURCE_DIR=${WAR3_RENDER_CORE_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/CheckWar3RenderCore.cmake"
    VERBATIM)
set(WAR3_RENDER_CORE_BUILD_TESTS OFF CACHE BOOL "共享测试由 FORGE 统一登记" FORCE)
set(WAR3_RENDER_CORE_BUILD_TOOLS OFF CACHE BOOL "夹具工具在共享仓库独立构建" FORCE)
add_subdirectory("${WAR3_RENDER_CORE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/shared-render-core")

# INTERFACE 的依赖向使用 Result/插件合同/CPU 库的宿主目标传递。
add_dependencies(war3-render-contract forge-render-core-lock-check)
