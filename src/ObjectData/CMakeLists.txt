add_library(War3MapObjectData STATIC
    LniObjectCodec.cpp
    NativeObjectCodec.cpp
    SkinObjectCatalog.cpp
    ObjectImport.cpp
    ObjectBatchEdit.cpp
    ObjectTypes.cpp
    ObjectFieldSchema.cpp
    ObjectReferenceIndex.cpp
    StandardObjectCatalog.cpp
    ReferenceAudit.cpp
    SlkTableText.cpp
    SlkOptimizer.cpp)

target_include_directories(War3MapObjectData
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include")

target_link_libraries(War3MapObjectData PUBLIC War3MapEditor::Core)

target_compile_definitions(War3MapObjectData
    PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN)

if(MSVC)
    target_compile_options(War3MapObjectData PRIVATE /W4 /permissive- /utf-8)
endif()

add_library(War3MapEditor::ObjectData ALIAS War3MapObjectData)
