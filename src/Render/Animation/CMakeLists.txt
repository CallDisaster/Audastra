cmake_minimum_required(VERSION 3.24)
# Qt-free 服务的独立检查入口；Windows 编辑器通过 RenderSupport 编译同一份源文件。
get_filename_component(_animation_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ForgeModelAnimation LANGUAGES CXX)
    include("${_animation_root}/cmake/War3RenderCore.cmake")
    add_library(forge-animation-reference STATIC
        "${_animation_root}/src/Core/Types.cpp"
        "${_animation_root}/src/Core/Image/BlpImage.cpp"
        "${_animation_root}/src/Core/Image/JpegDecoder.cpp"
        "${_animation_root}/src/Render/Scene/WorldModelScene.cpp")
    target_link_libraries(forge-animation-reference PUBLIC War3RenderCore::CPU)
    add_subdirectory("${_animation_root}/src/Render/Particles" "${CMAKE_CURRENT_BINARY_DIR}/particles")
    add_library(forge-model-animation STATIC AnimationAssets.cpp AnimationState.cpp AnimationVisibility.cpp AnimationFrame.cpp AnimationParticles.cpp AnimationParticleAssets.cpp AnimationRenderer.cpp AnimationCapturePlan.cpp)
    target_link_libraries(forge-model-animation PUBLIC forge-animation-reference forge-particle-preview)
    foreach(_target IN ITEMS forge-animation-reference forge-model-animation)
        target_compile_features(${_target} PUBLIC cxx_std_20)
        if(MSVC)
            target_compile_options(${_target} PRIVATE /W4 /WX /permissive- /utf-8 /fp:precise)
        else()
            target_compile_options(${_target} PRIVATE -Wall -Wextra -Werror -ffp-contract=off)
        endif()
    endforeach()
    if(NOT MSVC)
        # 只读 Core 解码器已有一处 Diagnostic::origin 省略初始化；保留告警，不改范围外源码。
        set_source_files_properties("${_animation_root}/src/Core/Image/BlpImage.cpp"
            PROPERTIES COMPILE_OPTIONS "-Wno-error=missing-field-initializers")
    endif()
    enable_testing()
    set(_animation_link forge-model-animation)
    include("${CMAKE_CURRENT_LIST_DIR}/TestTargets.cmake")
endif()
