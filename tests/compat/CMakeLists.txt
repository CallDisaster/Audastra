add_executable(War3MapCompatRoundtripTests NativeRoundtripTests.cpp)
target_link_libraries(War3MapCompatRoundtripTests PRIVATE War3MapEditor::ObjectData)

if(MSVC)
    target_compile_options(War3MapCompatRoundtripTests PRIVATE /W4 /permissive- /utf-8)
endif()

add_test(NAME compat-roundtrip COMMAND War3MapCompatRoundtripTests)
