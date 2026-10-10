# 本机运行库接口库存：只读 PE 注册关系，绝不加载 DLL 或执行作者代码。
add_library(War3MapRuntimeLuaApi STATIC RuntimeLuaApiInventory.cpp RuntimeLuaApiPe.cpp RuntimeLuaApiPe.h)
add_library(War3MapEditor::RuntimeLuaApi ALIAS War3MapRuntimeLuaApi)
target_link_libraries(War3MapRuntimeLuaApi PUBLIC War3MapEditor::Core War3MapEditor::Jass)
target_compile_definitions(War3MapRuntimeLuaApi PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapRuntimeLuaApi PRIVATE /W4 /permissive- /utf-8)
