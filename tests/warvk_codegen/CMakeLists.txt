find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(NOT TARGET War3MapTrigger)
    message(FATAL_ERROR "tests/warvk_codegen requires the War3MapTrigger target")
endif()

add_executable(
    warvk-generated-static-verifier
    WarVKGeneratedStaticVerifier.cpp)
target_link_libraries(
    warvk-generated-static-verifier
    PRIVATE
        War3MapEditor::Trigger)
target_compile_definitions(
    warvk-generated-static-verifier
    PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(
        warvk-generated-static-verifier
        PRIVATE
            /W4
            /permissive-
            /utf-8)
endif()

add_test(
    NAME warvk-manifest-codegen
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_generate_warvk.py")
set_tests_properties(
    warvk-manifest-codegen
    PROPERTIES
        ENVIRONMENT
            "WARVK_GENERATED_STATIC_VERIFIER=$<TARGET_FILE:warvk-generated-static-verifier>")
