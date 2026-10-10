add_executable(war3-map-mpq-tests MpqTests.cpp)
target_include_directories(war3-map-mpq-tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/Core")
target_link_libraries(war3-map-mpq-tests PRIVATE War3MapEditor::Core)
target_compile_definitions(war3-map-mpq-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    MPQ_TEST_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures")
target_compile_options(war3-map-mpq-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME mpq-reader COMMAND war3-map-mpq-tests)

add_executable(war3-map-mpq-audio-tests MpqAudioDecodeTests.cpp)
target_include_directories(war3-map-mpq-audio-tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Core")
target_link_libraries(war3-map-mpq-audio-tests PRIVATE War3MapEditor::Core)
target_compile_definitions(war3-map-mpq-audio-tests PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(war3-map-mpq-audio-tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME mpq-audio-decode-boundary COMMAND war3-map-mpq-audio-tests)
