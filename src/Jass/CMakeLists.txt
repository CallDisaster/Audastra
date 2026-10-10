# 自研脚本编译链（JASS / vJass）：预处理、词法 / 语法分析、类型检查与 vJass 子集翻译。只依赖 Core。
add_library(War3MapJass STATIC
    IdentifierRewrite.cpp
    JassChecker.cpp
    JassCodeCheck.cpp
    JassDeclarationIndex.cpp
    JassSymbolCollector.cpp
    JassLexer.cpp
    JassParser.cpp
    JassTypes.cpp
    PpExpression.cpp
    PpLexer.cpp
    Preprocessor.cpp
    SourceMap.cpp
    VJassRender.cpp
    VJassScan.cpp
    VJassStructs.cpp
    VJassSymbols.cpp
    VJassText.cpp
    VJassTranslator.cpp)
add_library(War3MapEditor::Jass ALIAS War3MapJass)
target_link_libraries(War3MapJass PUBLIC War3MapEditor::Core)
target_compile_definitions(War3MapJass PRIVATE NOMINMAX UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_compile_options(War3MapJass PRIVATE /W4 /permissive- /utf-8)
