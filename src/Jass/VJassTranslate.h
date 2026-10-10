#pragma once

#include "VJassModel.h"
#include "VJassText.h"

#include "War3MapCore/Jass/VJass.h"

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// vJass 翻译的内部结构：符号表（VJassSymbols.cpp）、表达式与语句改写（VJassRender.cpp）、
// 结构的生成代码（VJassStructs.cpp）、整体输出（VJassTranslator.cpp）。
namespace War3Map::Jass::detail {

// vJass 层面的类型：JASS 类型、结构实例（JASS 中为 integer）、或结构类型本身（用于 S.member / S.method()）。
struct VType {
    enum class Kind : std::uint8_t { Unknown, Jass, Struct, StructType } kind = Kind::Unknown;
    std::string_view jass;
    std::int32_t structIndex = -1;

    static VType Jass(std::string_view name) { return {Kind::Jass, name, -1}; }
    static VType Instance(std::int32_t index) { return {Kind::Struct, "integer", index}; }
    static VType TypeOf(std::int32_t index) { return {Kind::StructType, {}, index}; }
    [[nodiscard]] bool IsInstance() const noexcept { return kind == Kind::Struct; }
    [[nodiscard]] bool IsStructType() const noexcept { return kind == Kind::StructType; }
    [[nodiscard]] bool Is(std::string_view name) const noexcept { return kind == Kind::Jass && jass == name; }
};

struct VGlobalSymbol {
    VType type;
    bool array = false;
    bool fromMap = false; // 地图脚本中声明（参数 / 局部变量与之同名时改名 l__）
};

struct VFunctionSymbol {
    std::vector<VType> params;
    VType returns;
    std::int32_t function = -1; // VFunction 下标；游戏脚本与地图的原生函数为 -1
};

// 正在改写的一段代码（函数 / 方法体，或全局 / 成员的初始值）的上下文。
struct CodeScope {
    const SyntaxPool* pool = nullptr;
    std::int32_t library = -1;
    std::int32_t scope = -1;
    std::int32_t structIndex = -1;
    std::uint32_t order = 0; // 当前代码在输出中的位置；只能调用位置在它之前的方法（0 表示在所有方法之前）
    struct Local {
        VType type;
        std::string name; // 输出名（可能改名为 l__…）
        bool array = false;
    };
    std::unordered_map<std::string_view, Local> locals;
    bool callsMap = false; // 调用或引用了地图脚本中的函数（决定 onDestroy 调用器能否内联）
};

struct Rendered {
    std::string text; // changed 时有效；否则输出沿用源文本
    VType type;
    bool array = false;   // 数组变量本身（尚未加下标）
    bool changed = false;
    bool literal = false; // 字符串字面量；content 为其内容（不含引号，换行已改写为 \n）
    std::string content;
    std::vector<VJassByteOrigin> byteOrigins; // changed表达式内未改片段仍保精确字节来源。
};

// 源文本中的一段替换：[begin, end) 换成 text；text 为空指针表示不变。
struct Piece {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    const std::string* text = nullptr;
    const std::vector<VJassByteOrigin>* byteOrigins = nullptr;
};

class Translation {
public:
    Translation(VProgram& program, const VJassOptions& options, VJassResult& result, std::string name, CheckBudget& budget);

    // 建立名称、符号表、库顺序、结构编号与输出位置，解析函数体；有错误或不支持的特性时返回 false。
    bool Prepare();
    [[nodiscard]] MappedText Emit();

private:
    // ---- VJassSymbols.cpp ----
    [[nodiscard]] bool Live(std::int32_t library) const noexcept;
    void AssignNames();
    void AddBaseScripts();
    void BuildSymbols();
    bool OrderLibraries();
    void OrderScopes();
    bool LinkStructs();
    void AssignOrder();
    void ParseBodies();
    [[nodiscard]] VType ResolveType(std::string_view name, std::int32_t library, std::int32_t structIndex) const;
    [[nodiscard]] std::string_view ScopedName(std::string_view name, std::int32_t library) const;
    [[nodiscard]] std::int32_t FindStruct(std::string_view name, std::int32_t library) const;
    [[nodiscard]] VFunctionSymbol Signature(const VFunction& function, std::int32_t index) const;

    // ---- VJassRender.cpp ----
    Rendered Render(std::int32_t expr, CodeScope& scope);
    Rendered RenderImpl(std::int32_t expr, CodeScope& scope);
    Rendered RenderName(const Expr& expr, CodeScope& scope);
    Rendered RenderMember(const Expr& expr, CodeScope& scope);
    Rendered RenderIndex(const Expr& expr, CodeScope& scope);
    Rendered RenderCall(const Expr& expr, CodeScope& scope);
    Rendered RenderMethodCall(const Expr& expr, CodeScope& scope);
    Rendered RenderFunctionRef(const Expr& expr, CodeScope& scope);
    Rendered RenderConcat(const Expr& expr, CodeScope& scope);
    std::vector<Rendered> RenderArguments(const Expr& call, CodeScope& scope, const VFunctionSymbol* signature);
    // 成员访问 a.b 的左侧：结构类型或实例；隐含 this 且 b 为静态成员时按静态访问。
    struct MemberBase {
        Rendered rendered;
        std::int32_t structIndex = -1;
        bool viaType = false; // 通过结构类型访问（静态）
    };
    bool ResolveBase(const Expr& member, CodeScope& scope, MemberBase& base);
    [[nodiscard]] std::string MemberText(const MemberBase& base, const Expr& left, const VMember& member, std::int32_t owner) const;
    // 调用方法：非静态方法以 self 为第一个参数。
    [[nodiscard]] std::string MethodCall(std::int32_t method, const std::string* self, const std::vector<std::string>& arguments) const;
    [[nodiscard]] std::vector<std::string> ArgumentTexts(const Expr& call, const std::vector<Rendered>& arguments, const CodeScope& scope) const;
    bool CheckOrder(std::int32_t method, const CodeScope& scope, std::uint32_t line);
    [[nodiscard]] std::string TextOf(const Rendered& rendered, const Expr& expr) const;
    [[nodiscard]] static Piece PieceOf(const Expr& expr, const Rendered& rendered);
    // 源文本 [begin, end) 中替换 pieces（按位置排序、互不重叠）；没有改动时返回 false。
    bool Splice(std::uint32_t begin, std::uint32_t end, std::span<const Piece> pieces, std::string& out,
        std::vector<VJassByteOrigin>* origins = nullptr) const;
    bool Splice(std::uint32_t begin, std::uint32_t end, std::initializer_list<Piece> pieces, std::string& out,
        std::vector<VJassByteOrigin>* origins = nullptr) const {
        return Splice(begin, end, std::span<const Piece>(pieces.begin(), pieces.size()), out, origins);
    }
    void Statements(const std::vector<std::int32_t>& statements, CodeScope& scope, std::vector<TextEdit>& edits);
    void Statement(const Stmt& stmt, CodeScope& scope, std::vector<TextEdit>& edits);
    bool AssignmentCall(const Stmt& stmt, CodeScope& scope, std::vector<TextEdit>& edits);
    void Replace(std::int32_t expr, CodeScope& scope, std::vector<TextEdit>& edits);
    [[nodiscard]] std::string LocalName(std::string_view name, std::int32_t library) const;
    [[nodiscard]] std::string OutputType(const VType& type) const;
    [[nodiscard]] std::uint32_t Offset(std::string_view view) const noexcept;

    // 函数 / 方法：声明行、函数体、结束行；BodyText 只输出函数体（生成的 sa__ / sc__ 复用）。
    CodeScope ScopeFor(std::int32_t function);
    void EmitFunction(MappedOutput& out, std::int32_t function);
    MappedText BodyText(std::int32_t function, CodeScope& scope);
    std::string HeaderText(const VFunction& function, const CodeScope& scope) const;

    // ---- VJassStructs.cpp ----
    [[nodiscard]] std::string MethodName(const VFunction& method) const;
    [[nodiscard]] const VMember* FindMember(std::int32_t structIndex, std::string_view name, std::int32_t& owner) const;
    // staticness：-1 不限，0 只找实例方法，1 只找静态方法；[] / []= 运算符不比较名称。
    [[nodiscard]] std::int32_t FindMethod(std::int32_t structIndex, std::string_view name, OperatorKind op, int staticness = -1) const;
    [[nodiscard]] std::int32_t Root(std::int32_t structIndex) const;
    [[nodiscard]] bool HasChildren(std::int32_t structIndex) const;
    [[nodiscard]] std::vector<std::int32_t> RootListOrder() const;
    [[nodiscard]] std::string DestroyName(std::int32_t structIndex, const CodeScope& scope) const;
    void PrepareDestructors();
    void EmitStructGlobals(MappedOutput& out);
    void EmitStructFunctions(MappedOutput& out);
    void EmitStruct(MappedOutput& out, std::int32_t structIndex);
    void EmitStructInitializers(MappedOutput& out);
    MappedText MemberInits(std::int32_t structIndex);
    MappedText Destructor(std::int32_t structIndex, bool direct) const;

    // ---- VJassTranslator.cpp ----
    void EmitGlobal(MappedOutput& out, const VGlobal& global);
    void EmitItems(MappedOutput& out, const std::vector<VItem>& items, std::uint32_t begin, std::uint32_t end);
    void EmitScope(MappedOutput& out, std::int32_t index);
    void EmitItem(MappedOutput& out, const VItem& item);
    void AppendSource(MappedOutput& out, std::uint32_t begin, std::uint32_t end) const;
    void Fail(std::uint32_t line, std::string reason);
    void Error(std::uint32_t line, std::string code, std::string message);
    void Warning(std::uint32_t line, std::string code, std::string message);
    void Report(DiagnosticSeverity severity, std::uint32_t line, std::string code, std::string message);
    [[nodiscard]] bool Failed() const noexcept;

    VProgram& program_;
    const VJassOptions& options_;
    VJassResult& result_;
    std::string name_;
    std::string_view text_;
    CheckBudget& budget_;
    mutable TranslationBudget textBudget_;
    mutable SourceTrace trace_;
    std::size_t libraryNesting_ = 0;

    std::vector<std::unordered_map<std::string_view, std::string_view>> libraryNames_; // 每个库：源名称 → 输出名
    std::unordered_map<std::string_view, VGlobalSymbol> globals_;     // 输出名 → 全局变量
    std::unordered_map<std::string_view, VFunctionSymbol> functions_; // 输出名 → 函数签名（不含方法）
    std::vector<VFunctionSymbol> signatures_;                         // 每个 VFunction（含方法）的签名
    std::unordered_map<std::string_view, std::int32_t> structs_;      // 结构全名 → 下标
    std::vector<FunctionBody> bodies_;                                // 每个 VFunction 的函数体
    std::vector<std::int32_t> libraryOrder_;
    std::vector<std::int32_t> structOrder_;           // 按类型编号
    std::vector<std::uint32_t> destructorOrder_;      // 每个结构：库内直接析构函数的位置（有 onDestroy 且无子结构时）
    std::unordered_map<std::int32_t, MappedText> destroyBodies_; // onDestroy 复用体保留实际编辑来源。
    std::unordered_map<std::int32_t, bool> destroyCallsMap_;
    std::deque<std::string> storage_; // 改写后名称的稳定存储（供 string_view 键引用）
    std::string initName_;            // jasshelper__initstructs…
    std::string mainInsertion_;       // 插入 main 中 InitBlizzard 之后的初始化调用
    std::int32_t mainFunction_ = -1;
    bool usesTriggers_ = false; // 需要 f__arg_this（有 onDestroy 或子结构）
};

} // namespace War3Map::Jass::detail
