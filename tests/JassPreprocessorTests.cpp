#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Jass/SourceMap.h"
#include "../src/Jass/PpLexer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

// 自研预处理器回归：宏语义按 C99 / Wave 的可观察行为对照，全部为原创小样例。
namespace {

using namespace War3Map;
using namespace War3Map::Jass;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool HasCode(const PreprocessResult& result, std::string_view code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

// 去掉空行，行内连续空白压成一个空格并去掉首尾空白：只比较记号序列与换行结构。
std::string Squeeze(std::string_view text) {
    std::string out;
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        std::string squeezed;
        bool space = false;
        for (const char c : line) {
            if (c == ' ' || c == '\t' || c == '\r') {
                space = !squeezed.empty();
                continue;
            }
            if (space) squeezed.push_back(' ');
            space = false;
            squeezed.push_back(c);
        }
        if (!squeezed.empty()) out += squeezed + "\n";
    }
    return out;
}

PreprocessResult Run(std::string_view text, const PreprocessOptions& options = {}, PreprocessCache* cache = nullptr,
    const std::filesystem::path& directory = {}) {
    return Preprocess(text, "main.j", directory, options, cache);
}

void Expect(std::string_view source, std::string_view expected, const std::string& label) {
    const auto result = Run(source);
    Require(result.Succeeded(), label + ": 预处理失败");
    const auto actual = Squeeze(result.text);
    if (actual != expected) throw std::runtime_error(label + ": 输出不符\n--- 实际 ---\n" + actual + "--- 期望 ---\n" + std::string(expected));
}

void TestObjectMacros() {
    Expect("#define A 1\n#define B A + A\nset x = B\n", "set x = 1 + 1\n", "对象宏递归展开");
    Expect("#define SELF SELF + 1\nset x = SELF\n", "set x = SELF + 1\n", "自引用不再展开");
    Expect("#define A B\n#define B A\nA B\n", "A B\n", "互相引用在隐藏集处停止");
    Expect("#define X 1\n#undef X\nX\n", "X\n", "#undef");
    // '$' 属于标识符：$FF 与 $X$ 不会被宏 FF / X 替换；字符串与 rawcode 原样保留。
    Expect("#define FF 0\n#define X 9\nset a = $FF + 'X' + \"X\" + $X$\n", "set a = $FF + 'X' + \"X\" + $X$\n", "JASS 字面量不被替换");
    // 未展开的行（含注释）原样复制。
    const auto result = Run("#define Q 1\n// keep me\nfunction f takes nothing returns nothing // tail\n");
    Require(result.text.find("// keep me") != std::string::npos && result.text.find("// tail") != std::string::npos, "注释原样保留");
}

void TestFunctionMacros() {
    Expect("#define ADD(a, b) ((a) + (b))\nset x = ADD(1, 2 * 3)\n", "set x = ((1) + (2 * 3))\n", "函数宏替换");
    Expect("#define F(x) x\nset y = F((1, 2))\n", "set y = (1, 2)\n", "括号内逗号不分参");
    Expect("#define F(x) [x]\nF\n", "F\n", "无括号时不展开");
    Expect("#define S(x) #x\nS(a  \"b\\n\"  c)\n", "\"a \\\"b\\\\n\\\" c\"\n", "# 字符串化转义");
    Expect("#define CAT(a, b) a ## b\nCAT(Get, Unit)(u)\n", "GetUnit(u)\n", "## 拼接");
    Expect("#define CAT(a, b) a ## b\n#define GetUnit X\nCAT(Get, Unit)\n", "X\n", "拼接结果再扫描");
    Expect("#define V(f, ...) f(__VA_ARGS__)\nV(g, 1, 2)\nV(h)\n", "g(1, 2)\nh()\n", "可变参数");
    // 参数先完整展开，再代入（## 与 # 的操作数除外）。
    Expect("#define ONE 1\n#define ID(x) x\n#define STR(x) #x\n#define XSTR(x) STR(x)\nID(ONE) STR(ONE) XSTR(ONE)\n",
        "1 \"ONE\" \"1\"\n", "参数预展开");
    // 行尾宏名从下一行取 '('（Wave 行为：两行合并输出）。
    Expect("#define F(x) <x>\nset a = F\n(7)\n", "set a = <7>\n", "跨行调用");
    // C99 标准 6.10.3.5 示例的一部分：重扫描与隐藏集。
    Expect("#define f(a) a*g\n#define g(a) f(a)\nf(2)(9)\n", "2*9*g\n", "标准重扫描示例");
}

void TestConditionals() {
    Expect("#if 1 + 2 * 3 == 7 && !defined(NOPE)\nyes\n#else\nno\n#endif\n", "yes\n", "#if 表达式");
    Expect("#define V 3\n#if V > 5\na\n#elif V > 2\nb\n#else\nc\n#endif\n", "b\n", "#elif");
    Expect("#ifdef A\na\n#endif\n#ifndef A\nnot_a\n#endif\n", "not_a\n", "#ifdef / #ifndef");
    Expect("#if 0\n#if garbage (\n#endif\nx\n#endif\ny\n", "y\n", "未选中分支中的嵌套条件不求值");
    Expect("#if 0 && (1 / 0)\na\n#else\nb\n#endif\n", "b\n", "短路中不报除零");
    Expect("#if -1 < 0u\na\n#else\nb\n#endif\n", "b\n", "无符号比较");
    Expect("#if 0x10 == 16 && 010 == 8 && 'A' == 65\nok\n#endif\n", "ok\n", "数字与字符常量");
    Expect("#define E(x) defined(x)\n#if !E(Z)\nok\n#endif\n", "ok\n", "展开后出现的 defined");

    Require(HasCode(Run("#if 1\nx\n"), "PP_UNTERMINATED_CONDITIONAL"), "缺少 #endif 报错");
    Require(HasCode(Run("#endif\n"), "PP_UNBALANCED_CONDITIONAL"), "多余 #endif 报错");
    Require(!Run("#if true\n#endif\n").Succeeded(), "C99 下 true 视为错误（与 Wave 一致）");
    Require(!Run("#if 1 / 0\n#endif\n").Succeeded(), "除以零报错");
    Require(!Run("#error stop here\n").Succeeded(), "#error 失败");
    Require(!Run("#frobnicate\n").Succeeded(), "未知指令报错");
}

void TestIncludes() {
    const auto root = std::filesystem::temp_directory_path() / "war3-jass-pp-tests";
    std::filesystem::remove_all(root);
    WriteText(root / "sys" / "lib.h", "#pragma once\n#define LIB_VALUE 42\nsys_lib\n");
    WriteText(root / "sys" / "only.h", "sys_only\n");
    WriteText(root / "user" / "only.h", "user_only\n");
    WriteText(root / "main" / "only.h", "main_only\n");
    WriteText(root / "main" / "nested" / "inner.h", "#include \"sibling.h\"\n");
    WriteText(root / "main" / "nested" / "sibling.h", "nested_sibling\n");
    WriteText(root / "main" / "force.h", "#define FORCED 7\n");
    WriteText(root / "main" / "loop.h", "#include \"loop.h\"\n");

    PreprocessOptions options;
    options.systemIncludes = {root / "sys"};
    options.userIncludes = {root / "user"};
    options.forceIncludes = {"force.h"};
    options.defines = {{"FLAG", ""}, {"LEVEL", "3"}};
    PreprocessCache cache;
    const std::string_view mainText =
        "#include <lib.h>\n#include <lib.h>\n#include \"only.h\"\n#include <only.h>\n#include \"nested/inner.h\"\n"
        "#define HDR <lib.h>\n#include HDR\nv = LIB_VALUE FORCED FLAG LEVEL __LINE__\n";
    const auto first = Run(mainText, options, &cache, root / "main");
    Require(first.Succeeded(), "包含预处理失败");
    Require(Squeeze(first.text) == "sys_lib\nmain_only\nsys_only\nnested_sibling\nv = 42 7 1 3 8\n",
        "包含查找顺序、#pragma once、计算式包含与预定义宏\n" + Squeeze(first.text));
    Require(first.cacheHits == 0 && cache.Entries() > 0, "首轮填充缓存");
    const auto second = Run(mainText, options, &cache, root / "main");
    Require(second.text == first.text && second.cacheHits > 0, "内容未变时命中缓存且输出一致");

    // 行映射：输出中 nested_sibling 所在行指向 sibling.h 第 1 行，最后一行指向主文件第 8 行。
    std::uint32_t lineNumber = 0;
    std::uint32_t siblingLine = 0;
    std::uint32_t valueLine = 0;
    std::istringstream lines(first.text);
    for (std::string line; std::getline(lines, line);) {
        ++lineNumber;
        if (line.find("nested_sibling") != std::string::npos) siblingLine = lineNumber;
        if (line.find("v = ") != std::string::npos) valueLine = lineNumber;
    }
    Require(siblingLine != 0 && first.sourceMap.Describe(siblingLine).ends_with("sibling.h:1"), "包含文件的行映射: " + first.sourceMap.Describe(siblingLine));
    Require(first.sourceMap.Describe(valueLine) == "main.j:8", "主文件行映射: " + first.sourceMap.Describe(valueLine));
    const auto splicedInclude=Run("#include \"on\\\nly.h\"\n__LINE__\n",options,nullptr,root/"main");
    Require(splicedInclude.Succeeded() && Squeeze(splicedInclude.text)=="main_only\n3\n",
        "包含名raw文本也删除续行，后续行号仍指向物理来源");

    PreprocessOptions guarded;
    guarded.userIncludes = {root / "user"}; guarded.systemIncludes = {root / "sys"};
    std::vector<IncludeCandidateRequest> attempts;
    guarded.includeCandidatePolicy = [&](const IncludeCandidateRequest& request) {
        attempts.push_back(request);
        return Result<IncludeCandidateEligibility>::Success(request.searchRole == IncludeSearchRole::CurrentFile ?
            IncludeCandidateEligibility::SkippedOutOfScope : IncludeCandidateEligibility::Allowed);
    };
    int providerCalls = 0;
    guarded.includeProvider = [&](const std::filesystem::path& candidate) {
        ++providerCalls;
        Require(candidate.parent_path() != root / "main", "范围外候选绝不调用内容 provider");
        return Result<PreprocessIncludeContent>::Success({candidate.parent_path() == root / "user", "VISIBLE_USER\n"});
    };
    const auto eligible = Run("#include \"only.h\"\n", guarded, nullptr, root / "main");
    Require(eligible.Succeeded() && providerCalls == 1 && eligible.text.find("VISIBLE_USER") != std::string::npos &&
        attempts.size() == 2 && attempts[0].searchOrdinal == 0 && attempts[1].searchRole == IncludeSearchRole::User &&
        attempts[1].requesterLine == 1 && attempts[1].requesterOffset == 9,
        "资格省略继续查找且保留请求阶段位置与优先级，不伪造 Missing");
    guarded.includeProvider = [&](const std::filesystem::path&) {
        ++providerCalls; return Result<PreprocessIncludeContent>::Failure("Fixture", "READ_REFUSED", "higher candidate rejected");
    };
    providerCalls = 0;
    const auto noFallback = Run("#include \"only.h\"\n", guarded, nullptr, root / "main");
    Require(!noFallback.Succeeded() && providerCalls == 1 && HasCode(noFallback, "READ_REFUSED"), "真正 provider 失败不继续低层候选");
    guarded.includeCandidatePolicy = [](const IncludeCandidateRequest&) {
        return Result<IncludeCandidateEligibility>::Success(IncludeCandidateEligibility::SkippedOutOfScope);
    };
    providerCalls = 0;
    Require(HasCode(Run("#include \"only.h\"\n", guarded, nullptr, root / "main"), "SCRIPT_INCLUDE_PATH_SCOPE") && providerCalls == 0,
        "全部范围省略仍报 scope，不探测任何候选");
    guarded.includeCandidatePolicy = [](const IncludeCandidateRequest&) { return Result<IncludeCandidateEligibility>{}; };
    const auto emptyPolicy = Run("#include \"only.h\"\n", guarded, nullptr, root / "main");
    Require(!emptyPolicy.Succeeded() && HasCode(emptyPolicy, "PP_INCLUDE_POLICY_FAILED") && providerCalls == 0,
        "没有诊断或值的 policy 失败不能静默删掉 include");

    // 内容变化后按哈希重新分词。
    WriteText(root / "main" / "only.h", "main_only_changed\n");
    const auto third = Run(mainText, options, &cache, root / "main");
    Require(third.text.find("main_only_changed") != std::string::npos, "内容变化后不复用旧词法结果");

    Require(!Run("#include \"missing.h\"\n", options, nullptr, root / "main").Succeeded(), "缺少包含文件报错");
    const auto loop = Run("#include \"loop.h\"\n", options, nullptr, root / "main");
    Require(!loop.Succeeded() && HasCode(loop, "PP_INCLUDE_DEPTH"), "递归包含在深度上限处报错");
    std::filesystem::remove_all(root);
}

void TestLineEndings() {
    // CRLF 与单独 CR 都是换行；反斜杠续行。
    Expect("#define A 1\r\n#if A\r\nx\r\n#endif\r\n", "x\n", "CRLF");
    Expect("#define A 1\r#if A\rx\r#endif\r", "x\n", "单独 CR");
    Expect("#define LONG 1 + \\\n 2\nLONG\n", "1 + 2\n", "续行");
    // JASS 字符串可跨行：其中的 # 与宏名不被处理。
    Expect("#define A 9\ns = \"line1\n#define A 1\nA\"\nA\n", "s = \"line1\n#define A 1\nA\"\n9\n", "跨行字符串");
}

void TestPhysicalLineSplices() {
    using namespace War3Map::Jass::detail;
    for (const auto ending:{"\n","\r\n","\r"}) {
        const std::string source=std::string("function sample takes integer a,\\")+ending+
            " boolean b returns nothing"+ending+"endfunction"+ending+"__LINE__"+ending;
        const auto result=Run(source);
        Require(result.Succeeded() && Squeeze(result.text)==
            "function sample takes integer a, boolean b returns nothing\nendfunction\n4\n", "普通声明三种物理续行");
        Require(result.sourceMap.Describe(2)=="main.j:3" && result.sourceMap.Describe(3)=="main.j:4",
            "续行删除不改变随后声明/宏的物理源行映射");
        auto joined=SplicePpLines(source);
        Require(joined.Succeeded(), "有界phase2来源");
        auto tokens=LexPp(joined.value->text);
        joined.value->MapOrigins(tokens);
        const auto found=std::find_if(tokens.begin(),tokens.end(),[](const auto& token){return token.text=="boolean";});
        Require(found!=tokens.end() && found->line==2 && found->offset==source.find("boolean") &&
            found->End()==found->offset+7 && tokens.back().offset==source.size(), "token物理偏移/行/EOF不改为逻辑位置");
    }
    Expect("#define AB 7\nA\\\nB\n", "7\n", "分词之前连接宏名");
    Expect("#define S \"a\\\r\nb\"\nS\ncall f(\"a\\n\")\n", "\"ab\"\ncall f(\"a\\n\")\n", "字符串物理续行与字母转义区分");
    Expect("#define BAD 9\n// keep \\\nBAD\nBAD\n", "// keep BAD\n9\n", "行注释先续接后结束");
    Expect("/\\\n* keep *\\\n/ live\n", "/* keep */ live\n", "注释边界先续接再分词");
    Expect("#define STR(x) #x\nSTR(\"a\\\nb\")\n", "\"\\\"ab\\\"\"\n", "字符串化消费逻辑记号");
    Expect("<? local s=\"a\\\nb\" ?>\n", "<? local s=\"ab\" ?>\n", "模板原文使用同一phase2规则");
    const auto failed=Run("function f takes integer a,\\\nboolean b returns nothing\n#error stop\\\nnow\n");
    Require(HasCode(failed,"PP_ERROR_DIRECTIVE") && failed.diagnostics.back().message.find("main.j:3: #error stopnow")!=std::string::npos,
        "错误指令文本逻辑续接且诊断仍为物理第三行");
    const auto gbk=SplicePpLines(std::string("\x81\x5C\nx"),64,Text::SourceEncoding::Gbk);
    Require(gbk.Succeeded() && gbk.value->splices.empty(), "显式GBK尾5C不是ASCII反斜杠续行");
    auto crossed=SplicePpLines("AB\\\nCD\n");
    auto crossedTokens=LexPp(crossed.value->text);
    crossed.value->MapOrigins(crossedTokens);
    Require(crossedTokens.front().text=="ABCD" && crossedTokens.front().End()==6 &&
        crossedTokens.back().offset==7 && crossedTokens.back().line==3, "跨续接记号的End和EOF使用完整物理跨度");
    Require(!SplicePpLines("a\\\nb",4).Succeeded(), "额外逻辑原文和位置表有界");
}

void TestSourceMapCompose() {
    SourceMap inner;
    const auto file = inner.AddFile("a.j");
    inner.Map(1, {file, 10});
    SourceMap outer;
    const auto middle = outer.AddFile("mid.j");
    outer.Map(1, {middle, 1});
    outer.Map(3, {middle, 5});
    const auto composed = outer.ComposeWith(inner, 4);
    Require(composed.Describe(2) == "a.j:11" && composed.Describe(3) == "a.j:14", "映射组合: " + composed.Describe(3));
}

void TestEncodedLexer() {
    using namespace War3Map::Jass::detail;
    using Text::SourceEncoding;
    const std::string source = "$宏名1\r\n\"中文😀\" '中文'";
    const auto utf8 = LexPp(source, SourceEncoding::Utf8);
    Require(utf8.Succeeded() && utf8.value->at(0).text == "$宏名1" && utf8.value->at(2).line == 2
        && utf8.value->at(2).text == "\"中文😀\"" && utf8.value->at(3).kind == PpKind::Char, "UTF8记号/字面量原保");
    const std::string gbk = std::string("// \x81\x5C\r\n$\xB1\xE4\xC1\xBF \"\x81\x5C\" '\x81\x5C'");
    const auto result = LexPp(gbk, SourceEncoding::Gbk);
    Require(result.Succeeded() && result.value->at(1).line == 2 && result.value->at(1).offset == 7
        && result.value->at(1).kind == PpKind::Identifier && result.value->at(2).text == gbk.substr(13, 4)
        && result.value->at(3).text == gbk.substr(18, 4), "GBK尾5C不延续注释/吞引号");
    Require(LexSingle("$宏名1", SourceEncoding::Utf8).value == PpKind::Identifier
        && LexSingle(gbk.substr(7, 5), SourceEncoding::Gbk).value == PpKind::Identifier
        && !LexSingle("x y", SourceEncoding::Utf8).Succeeded(), "粘贴结果共用编码与单记号规则");
    const std::string number = "1\x81\x65+2";
    const auto numeric = LexPp(number, SourceEncoding::Gbk);
    Require(numeric.Succeeded() && numeric.value->at(1).Is("+"), "GBK尾65不是指数标记e");
    const auto broken = LexPp(std::string("\"\x81\""), SourceEncoding::Gbk);
    Require(!broken.value && broken.diagnostics.front().origin->offset == 1, "坏GBK字符串不退回标点/部分成功");
    for (const auto text : {"udg_。", "/* unfinished", "\"unfinished", "// \x1A\nvalid"}) {
        Require(!LexPp(text, SourceEncoding::Utf8).Succeeded(), "编码PP严格拒绝非法记号/边界");
    }
    Require(!LexPp("", static_cast<SourceEncoding>(99)).Succeeded(), "空PP来源也拒绝未知编码");
}

} // namespace

int main() {
    try {
        TestObjectMacros();
        TestFunctionMacros();
        TestConditionals();
        TestIncludes();
        TestLineEndings();
        TestPhysicalLineSplices();
        TestSourceMapCompose();
        TestEncodedLexer();
    } catch (const std::exception& error) {
        std::cerr << "JassPreprocessor: " << error.what() << '\n';
        return 1;
    }
    std::cout << "JassPreprocessor: 8 scenario groups passed\n";
    return 0;
}
