add_library(war3-render-stub SHARED RenderStub.cpp)
target_include_directories(war3-render-stub PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(
    war3-render-stub
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        WAR3_RENDER_PLUGIN_BUILD)
target_compile_options(war3-render-stub PRIVATE /W4 /permissive- /utf-8)

target_link_libraries(war3-render-stub PRIVATE War3RenderCore::Contract)
