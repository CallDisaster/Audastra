# 界面组件库：设计令牌主题、字体、SVG 图标与通用控件。只依赖 Qt 和纯文字格式库。
qt_add_library(War3MapQtUiKit STATIC
    Brand/ApplicationBrand.cpp
    Brand/ApplicationBrand.h
    Fonts/FontLoader.cpp
    Fonts/FontLoader.h
    Icons/GeometricIcons.cpp
    Icons/GeometricIcons.h
    Icons/SvgIcons.cpp
    Icons/SvgIcons.h
    Markdown/MarkdownRenderer.cpp
    Markdown/MarkdownRenderer.h
    Markdown/TexRenderer.cpp
    Markdown/TexRenderer.h
    Theme/DesignTokens.generated.h
    Theme/ThemeManager.cpp
    Theme/ThemeManager.h
    Widgets/CodeView.cpp
    Widgets/CodeView.h
    Widgets/CodeViewTypes.h
    Widgets/CodeViewHighlight.cpp Widgets/CodeViewHighlight.h
    Widgets/CodeViewGutter.cpp Widgets/CodeViewGutter.h
    Widgets/CodeProblemsView.cpp Widgets/CodeProblemsView.h
    Widgets/ColumnLayout.cpp
    Widgets/ColumnLayout.h
    Widgets/CommandPalette.cpp
    Widgets/CommandPalette.h
    Widgets/ChangesPanel.cpp
    Widgets/ChangesPanel.h
    Widgets/ElidedLabel.cpp
    Widgets/ElidedLabel.h
    Widgets/Motion.cpp
    Widgets/Motion.h
    Widgets/PopoverSurface.cpp
    Widgets/PopoverSurface.h
    Widgets/RoundedPopups.cpp
    Widgets/RoundedPopups.h
    Widgets/SearchField.cpp
    Widgets/SearchField.h
    Widgets/WarcraftTextPreview.cpp
    Widgets/WarcraftTextPreview.h)
add_library(War3MapEditor::QtUiKit ALIAS War3MapQtUiKit)

qt_add_resources(War3MapQtUiKit "war3map_uikit_style"
    PREFIX "/war3map/uikit"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/Theme"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/Theme/Workbench.qss")

# 图标源文件与 Web 原型共用 assets/icons；资源路径为 :/war3map/icons/<相对路径>.svg。
file(GLOB_RECURSE war3map_uikit_icons CONFIGURE_DEPENDS
    RELATIVE "${PROJECT_SOURCE_DIR}/assets/icons"
    "${PROJECT_SOURCE_DIR}/assets/icons/*.svg")
list(TRANSFORM war3map_uikit_icons PREPEND "${PROJECT_SOURCE_DIR}/assets/icons/")
qt_add_resources(War3MapQtUiKit "war3map_uikit_icons"
    PREFIX "/war3map/icons"
    BASE "${PROJECT_SOURCE_DIR}/assets/icons"
    FILES ${war3map_uikit_icons})

# 产品图标在导出时完成 SVG 栅格化；界面绘制只读内嵌 PNG，不产生运行时外部依赖。
set(audastra_brand_assets "${PROJECT_SOURCE_DIR}/assets/brand")
qt_add_resources(War3MapQtUiKit "audastra_brand"
    PREFIX "/audastra/brand"
    BASE "${audastra_brand_assets}"
    FILES
        "${audastra_brand_assets}/audastra-512.png"
        "${audastra_brand_assets}/icon-16.png"
        "${audastra_brand_assets}/icon-24.png"
        "${audastra_brand_assets}/icon-32.png"
        "${audastra_brand_assets}/icon-48.png"
        "${audastra_brand_assets}/icon-64.png"
        "${audastra_brand_assets}/icon-128.png"
        "${audastra_brand_assets}/icon-256.png")

target_include_directories(War3MapQtUiKit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(War3MapQtUiKit PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg War3MapEditor::WarcraftText)
target_compile_definitions(War3MapQtUiKit PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapQtUiKit PRIVATE /W4 /permissive- /utf-8)

# 自带字体：tools/fetch-fonts.ps1 下载后在构建时复制到可执行文件旁的 fonts 目录。
set(WAR3MAPEDITOR_FONT_DIR "${PROJECT_SOURCE_DIR}/third_party/fonts/files" CACHE INTERNAL "")
function(war3map_deploy_fonts target)
    file(GLOB font_files CONFIGURE_DEPENDS
        "${WAR3MAPEDITOR_FONT_DIR}/*.ttf" "${WAR3MAPEDITOR_FONT_DIR}/*.otf")
    if(font_files)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/fonts"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${font_files} "$<TARGET_FILE_DIR:${target}>/fonts"
            VERBATIM)
    else()
        message(STATUS "${target}: bundled fonts not fetched (run tools/fetch-fonts.ps1); using system fonts")
    endif()
endfunction()
