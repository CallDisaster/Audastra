# 只使用 Windows SDK 的离线编译器；运行时不链接 d3dcompiler。
cmake_host_system_information(RESULT _windows_sdk_root QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE "KitsRoot10" VIEW BOTH)
file(GLOB _fxc_candidates "${_windows_sdk_root}/bin/*/x64/fxc.exe")
list(SORT _fxc_candidates COMPARE NATURAL ORDER DESCENDING)
find_program(WAR3MAPEDITOR_FXC fxc)
if(NOT WAR3MAPEDITOR_FXC AND _fxc_candidates)
    list(GET _fxc_candidates 0 WAR3MAPEDITOR_FXC)
endif()
if(NOT WAR3MAPEDITOR_FXC)
    message(FATAL_ERROR "D3D11 renderer requires Windows SDK fxc (offline HLSL compiler)")
endif()
foreach(stage IN ITEMS VS PS)
    string(TOLOWER "${stage}" _profile)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Scene${stage}.h"
        COMMAND "${WAR3MAPEDITOR_FXC}" /nologo /WX /O3 /T "${_profile}_5_0" /E "${stage}Main"
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/Scene${stage}.h" /Vn "kScene${stage}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Scene.hlsl"
        DEPENDS Shaders/Scene.hlsl VERBATIM)
endforeach()
foreach(_grid_variant IN ITEMS Grid GridWater)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Terrain${_grid_variant}VS.h"
        COMMAND "${WAR3MAPEDITOR_FXC}" /nologo /WX /O3 /T vs_5_0 /E "VSTerrain${_grid_variant}"
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/Terrain${_grid_variant}VS.h" /Vn "kTerrain${_grid_variant}VS"
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Scene.hlsl"
        DEPENDS Shaders/Scene.hlsl VERBATIM)
endforeach()
foreach(stage IN ITEMS VS PS)
    string(TOLOWER "${stage}" _profile)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Terrain${stage}.h"
        COMMAND "${WAR3MAPEDITOR_FXC}" /nologo /WX /O3 /T "${_profile}_5_0" /E "${stage}Terrain"
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/Terrain${stage}.h" /Vn "kTerrain${stage}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Scene.hlsl"
        DEPENDS Shaders/Scene.hlsl VERBATIM)
endforeach()
foreach(stage IN ITEMS VS PS)
    string(TOLOWER "${stage}" _profile)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Model${stage}.h"
        COMMAND "${WAR3MAPEDITOR_FXC}" /nologo /WX /O3 /T "${_profile}_5_0" /E "${stage}Model"
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/Model${stage}.h" /Vn "kModel${stage}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Scene.hlsl"
        DEPENDS Shaders/Scene.hlsl VERBATIM)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SkinVS.h"
    COMMAND "${WAR3MAPEDITOR_FXC}" /nologo /WX /O3 /T vs_5_0 /E VSSkinnedModel
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/SkinVS.h" /Vn kSkinVS
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/SkinModel.hlsl"
    DEPENDS Shaders/SkinModel.hlsl Shaders/Scene.hlsl VERBATIM)
foreach(stage IN ITEMS VS PS)
    string(TOLOWER "${stage}" _profile)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Particle${stage}.h"
        COMMAND "${WAR3MAPEDITOR_FXC}" /nologo /WX /O3 /T "${_profile}_5_0" /E "${stage}Particle"
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/Particle${stage}.h" /Vn "kParticle${stage}"
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Particle.hlsl"
        DEPENDS Shaders/Particle.hlsl Shaders/Scene.hlsl VERBATIM)
endforeach()
add_library(war3-render-d3d11 SHARED RenderPlugin.cpp Backend/D3D11/D3D11Scene.cpp
    Backend/D3D11/D3D11ModelLayers.cpp
    Backend/D3D11/D3D11Models.cpp Backend/D3D11/D3D11Skin.cpp Backend/D3D11/D3D11Particles.cpp Backend/D3D11/D3D11EditorVisibility.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/ParticleVS.h" "${CMAKE_CURRENT_BINARY_DIR}/ParticlePS.h"
    "${CMAKE_CURRENT_BINARY_DIR}/SkinVS.h"
    "${CMAKE_CURRENT_BINARY_DIR}/ModelVS.h" "${CMAKE_CURRENT_BINARY_DIR}/ModelPS.h"
    "${CMAKE_CURRENT_BINARY_DIR}/SceneVS.h" "${CMAKE_CURRENT_BINARY_DIR}/ScenePS.h"
    "${CMAKE_CURRENT_BINARY_DIR}/TerrainVS.h" "${CMAKE_CURRENT_BINARY_DIR}/TerrainPS.h")
target_sources(war3-render-d3d11 PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/TerrainGridVS.h" "${CMAKE_CURRENT_BINARY_DIR}/TerrainGridWaterVS.h")
target_include_directories(war3-render-d3d11 PRIVATE "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(war3-render-d3d11 PRIVATE WAR3_RENDER_PLUGIN_BUILD NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(war3-render-d3d11 PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(war3-render-d3d11 PRIVATE d3d11 dxgi War3RenderCore::SkinInput)

# CPU 材质目录、解码和空间查询共用 Core；不依赖 Qt 或 GPU 插件。
add_library(War3MapRenderSupport STATIC
    Tools/FrameMeasurement.cpp
    Animation/AnimationAssets.cpp
    Animation/AnimationAssetSnapshot.cpp
    Animation/AnimationInstanceDelta.cpp
    Animation/AnimationState.cpp
    Animation/AnimationVisibility.cpp
    Animation/AnimationFrame.cpp
    Animation/AnimationPreviewBudget.cpp
    Animation/PremPreview.cpp
    Animation/AnimationPrem.cpp
    Animation/AnimationParticles.cpp
    Animation/AnimationParticleAssets.cpp
    Animation/AnimationRenderer.cpp
    Animation/ModelSceneLayerRenderer.cpp
    Animation/AnimationCapturePlan.cpp
    Animation/AnimationParticleCoverage.cpp
    Animation/AnimationCaptureWindows.cpp
    Animation/AnimationSourceGuard.cpp
    Assets/TerrainMaterials.cpp
    Assets/TerrainTextureAtlas.cpp
    Scene/TerrainPicking.cpp
    Scene/TerrainInspection.cpp
    Scene/TerrainPreviewMesh.cpp
    Scene/TerrainMaterialPreview.cpp
    Scene/TerrainCliffPreview.cpp
    Scene/TerrainWaterPreview.cpp
    Scene/TerrainGroundMesh.cpp
    Scene/TerrainMinimapCandidate.cpp
    Scene/TerrainMinimapPostprocess.cpp
    Scene/ClassicMinimapCandidate.cpp
    Scene/TerrainMinimapRepresentativeColor.cpp
    Scene/TerrainMinimapAssets.cpp
    Scene/ViewRay.cpp
    Scene/WorldModelScene.cpp
    Scene/OwnedStaticModels.cpp
    Scene/BuildingSplatPreview.cpp
    Scene/BuildingSplatProjection.cpp
    Scene/TerrainCliffs.cpp
    Scene/DayNightLighting.cpp
    Scene/SceneEnvironment.cpp
    Scene/ModelPicking.cpp
    Scene/ModelPickingPrecise.cpp
    Scene/EditorOverlay.cpp)
add_library(War3MapEditor::RenderSupport ALIAS War3MapRenderSupport)
target_link_libraries(War3MapRenderSupport PUBLIC War3MapEditor::Core War3RenderCore::CPU)
target_compile_definitions(War3MapRenderSupport PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapRenderSupport PRIVATE /W4 /permissive- /utf-8)

# UI、CLI 和 MCP 共用的粒子 CPU 库；插件只编译独立输入检查器。
add_subdirectory(Particles)
target_link_libraries(War3MapRenderSupport PUBLIC forge-particle-preview)
target_sources(war3-render-d3d11 PRIVATE Particles/ParticleInput.cpp)

# 同源故障注入只存在于验证 DLL；不增加生产插件 ABI，也不部署此目标。
get_target_property(_render_test_sources war3-render-d3d11 SOURCES)
add_library(war3-render-d3d11-test SHARED ${_render_test_sources})
target_include_directories(war3-render-d3d11-test PRIVATE "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(war3-render-d3d11-test PRIVATE WAR3_RENDER_PLUGIN_BUILD WAR3_RENDER_TEST_FAULTS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(war3-render-d3d11-test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(war3-render-d3d11-test PRIVATE d3d11 dxgi War3RenderCore::SkinInput)

# 无窗口模型预览：复用同一 CPU 场景与 D3D11 完成帧回执，供人工和 Agent 审查。
add_executable(war3-model-preview Tools/ModelPreviewFrame.cpp)
target_link_libraries(war3-model-preview PRIVATE War3MapEditor::RenderSupport)
target_compile_definitions(war3-model-preview PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(war3-model-preview PRIVATE /W4 /permissive- /utf-8)
add_dependencies(war3-model-preview war3-render-d3d11)
add_custom_command(TARGET war3-model-preview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:war3-render-d3d11>" "$<TARGET_FILE_DIR:war3-model-preview>"
    VERBATIM)

# 后台地形预览：绑定源图/W3E 修订及显式相机，发布实际完成帧。
add_executable(war3-terrain-preview Tools/TerrainPreviewFrame.cpp)
target_link_libraries(war3-terrain-preview PRIVATE War3MapEditor::RenderSupport)
target_compile_definitions(war3-terrain-preview PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(war3-terrain-preview PRIVATE /W4 /permissive- /utf-8)
add_dependencies(war3-terrain-preview war3-render-d3d11)
add_custom_command(TARGET war3-terrain-preview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:war3-render-d3d11>" "$<TARGET_FILE_DIR:war3-terrain-preview>"
    VERBATIM)

# 只读真实模型兼容性扫描器；不进入 CTest，不接世界动画。
add_subdirectory(Tools/MdxScan)

# 同进程多实例/多帧动画验收；不触碰旧 ModelPreviewFrame 的命令约定。
add_executable(war3-model-animation "${PROJECT_SOURCE_DIR}/src/Cli/ModelAnimationCommand.cpp")
target_link_libraries(war3-model-animation PRIVATE War3MapEditor::RenderSupport)
target_compile_definitions(war3-model-animation PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(war3-model-animation PRIVATE /W4 /WX /permissive- /utf-8)
add_dependencies(war3-model-animation war3-render-d3d11)
add_custom_command(TARGET war3-model-animation POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:war3-render-d3d11>" "$<TARGET_FILE_DIR:war3-model-animation>" VERBATIM)
