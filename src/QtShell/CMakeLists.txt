# 主窗口外壳：模块注册表、停靠布局（主区 / 右侧组 / 浮动窗口）、顶栏切换器、导航与撤销路由。
# 只依赖界面组件库；具体编辑器模块在组装处登记，这里不认识任何模块。
qt_add_library(War3MapQtShell STATIC
    RecentMapsList.cpp RecentMapsList.h
    Dock/DockHost.cpp
    Dock/DockHost.h
    Dock/FloatFrame.cpp
    Dock/FloatFrame.h
    Dock/ModuleDrag.cpp
    Dock/ModuleDrag.h
    Dock/ModuleSwitcher.cpp
    Dock/ModuleSwitcher.h
    Dock/SideGroup.cpp
    Dock/SideGroup.h
    Layout/DockLayout.cpp
    Layout/DockLayout.h
    Layout/LayoutStore.cpp
    Layout/LayoutStore.h
    Modules/EditorModule.h
    Modules/ModuleHost.h
    Modules/ModuleRegistry.cpp
    Modules/ModuleRegistry.h
    Modules/NavigationHistory.cpp
    Modules/NavigationHistory.h
    Shell/HeaderBar.cpp
    Shell/HeaderBar.h
    Shell/StartupPage.cpp
    Shell/StartupPage.h
    Shell/ShellWindow.cpp
    Shell/ShellWindow.h)
add_library(War3MapEditor::QtShell ALIAS War3MapQtShell)

target_include_directories(War3MapQtShell PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtShell PUBLIC War3MapEditor::QtUiKit Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(War3MapQtShell PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtShell PRIVATE /W4 /permissive- /utf-8)
