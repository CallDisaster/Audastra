#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// 触发器参数的类型系统（参数面板、参数对话框、审计共用）。
//
// 规则来自原版 TriggerData 与 YDWE 的公开行为（只研究行为，不复制 YDWE 源码）：
// - Null：类型随前面的参数而定。YDWE 的任意类型函数（局部变量、自定义值等）跟随前面最近的
//   类型选择器参数（typename07_unit → unit，objecttype02_unitcode → unitcode）；
//   原版 SetVariable 跟随前一个参数（变量）的类型。
// - AnyReturnType：YDWE 的局部变量 / 自定义值读取与自定义代码，在任何类型的参数位置都可用。
// - 自定义类型与基础类型互通（degree / radian ↔ real，modelfile ↔ string 等，见 TriggerTypes 第 5 列）。
// - 经典 common.j 的 native handle 子类型沿实际 extends 父链单向传参；
//   UI 第 5 列是别名，不把同父 handle 的兄弟类型或向下转换当作兼容。
// - AnyGlobal：任意全局变量；AnyType：任意值。
namespace War3Map::Trigger {

class MapObjectReferenceSnapshot;

struct TriggerTypeInfo {
    std::string name;
    std::string display;  // 第 4 列；WESTRING_ 键原样保留
    std::string baseType; // 第 5 列（自定义类型的基础类型）
    bool canBeGlobal = false;
    // 第 7 列为 1：编辑器按基础类型取值（modelfile/imagefile 等）；YDWE 框架的 StringExt 同样按 string。
    bool editorUsesBaseType = false;
    bool comparable = false;
};

// YDWE 局部变量（loc_）与循环变量（loop_）。
struct LocalVariable {
    std::string name;
    std::string type;
    bool isArray = false;
    bool loop = false; // YDWEForLoopLocVarMultiple 的 loop_ 变量（integer）
};

class TriggerTypeSystem {
public:
    explicit TriggerTypeSystem(const UiCatalog& catalog, const MapObjectReferenceSnapshot* mapObjects = nullptr);

    enum class Literal { Other, Integer, Real, Boolean, String, Script, ObjectCode };
    // 沿Catalog基类型解析；对象四字码保留字面量角色，不折叠成十进制integer。
    // 类型选择器/未知类型仍由原目录处理，不按任意名称后缀猜测对象角色。
    [[nodiscard]] Literal LiteralKind(std::string_view type) const;
    [[nodiscard]] Status ValidateLiteral(std::string_view type, std::string_view value) const;
    // 与现有 JASS 后端共用同一数值字面量规则，不接受表达式或静默转换。
    [[nodiscard]] static bool IsIntegerLiteral(std::string_view value);
    // 十进制 int32 或 JASS 的 0x/$ 十六进制32位字面量；返回相同有符号位模式。
    // Lua 生成也用本解析，不把 0xFF000000 错当成正数或默认0。
    [[nodiscard]] static std::optional<std::int32_t> ParseIntegerLiteral(std::string_view value);
    [[nodiscard]] static bool IsRealLiteral(std::string_view value);
    // 不带外层引号的四个可安全编码字节；与JASS/Lua生成器共用。
    [[nodiscard]] static bool IsObjectCodeLiteral(std::string_view value);

    [[nodiscard]] const TriggerTypeInfo* Find(std::string_view type) const;
    // 显示名：TriggerTypes 第 4 列（WESTRING_ 键经目录的字符串表解析），无法解析时为类型名本身。
    [[nodiscard]] std::string DisplayName(std::string_view type) const;
    // expected 类型的参数能否接受 actual 类型的值。空的一方保留旧未知行为；
    // UI 标量别名互通，已知经典 native 的子类型只向实际祖先转换。
    [[nodiscard]] bool Accepts(std::string_view expected, std::string_view actual) const;
    // 预设（TriggerParams）的类型列。
    [[nodiscard]] std::optional<std::string> PresetType(std::string_view preset) const;

    // 定义中第 position 个参数（跳过 nothing）的声明类型。
    [[nodiscard]] static std::string DeclaredType(const UiDefinition& definition, std::size_t position);
    // 解析后的期望类型（Null 按上面的规则解析；解析不了时返回 "Null"）。
    [[nodiscard]] std::string ExpectedType(const TriggerDocument& document, const EcaNode& eca,
        std::size_t position, std::span<const LocalVariable> locals = {}) const;
    // 参数当前值的类型：预设 → 类型列；变量 → 变量类型（局部变量读取按名称）；函数 → 返回类型；
    // 直接值 → 空（不约束）。
    [[nodiscard]] std::string ValueType(const TriggerDocument& document, const ArgumentNode& argument,
        std::span<const LocalVariable> locals = {}) const;
    // 可写全局变量目标：真实唯一声明、类型和数组形状；调用/预设/字面量不能充当左值。
    // 下标表达式的递归语义继续由原参数检查处理，不缓存或重排运行时求值。
    // 可选输出始终取实际声明名；原名/udg_引用有多个候选时拒绝，不制造双前缀。
    [[nodiscard]] Status ValidateWritableVariable(const TriggerDocument& document, const ArgumentNode& argument,
        std::string_view expectedType, std::string* declaredName = nullptr) const;
    // 函数的返回类型；AnyReturnType 的局部变量读取按变量名解析到具体类型（找不到时保持 AnyReturnType）。
    [[nodiscard]] std::string ReturnType(const TriggerDocument& document, const EcaNode& call,
        std::span<const LocalVariable> locals = {}) const;
    // 触发器内的 YDWE 局部变量（按首次出现顺序去重，含嵌套函数中的定义）。
    [[nodiscard]] std::vector<LocalVariable> CollectLocals(const TriggerDocument& document,
        const TriggerRecord& trigger) const;

    // 类型选择器（typename、objecttype）：参数值决定后面 Null 参数的类型。
    [[nodiscard]] bool IsTypeSelector(std::string_view type) const;
    // 类型选择器的预设值 → 类型（typename07_unit → unit，objecttype02_unitcode → unitcode）。
    [[nodiscard]] static std::string TypeFromTypename(std::string_view value);

    [[nodiscard]] const UiCatalog& Catalog() const noexcept { return catalog_; }

    // 某类型参数可用的取值种类（与 WE 相同，依据函数库定义而非类型名特判）：
    // 变量 —— TriggerTypes 第 2 列允许全局变量，或 native 父类型有可声明的具体子类型；
    // 预设 —— 有该类型的 TriggerParams；
    // 函数 —— 有返回兼容具体类型的函数，或类型可作变量时（YDWE 任意类型读取函数）；直接值始终可用。
    // 类型未知（空、Null、AnyType 等）时不加限制。
    struct ValueKinds { bool preset = true; bool variable = true; bool function = true; };
    [[nodiscard]] ValueKinds AllowedValueKinds(std::string_view expected) const;

private:
    [[nodiscard]] std::string_view CanonicalUiType(std::string_view type) const;
    const UiCatalog& catalog_;
    const MapObjectReferenceSnapshot* mapObjects_ = nullptr;
    std::unordered_map<std::string, TriggerTypeInfo> types_;
    std::unordered_map<std::string, std::string> presetTypes_;
    std::set<std::string> selectors_;
};

// YDWE 局部变量相关函数名（读取 / 设置 / 循环）。
inline constexpr std::string_view kLocalGet = "YDWEGetAnyTypeLocalVariable";
inline constexpr std::string_view kLocalGetArray = "YDWEGetAnyTypeLocalArray";
inline constexpr std::string_view kLocalSet = "YDWESetAnyTypeLocalVariable";
inline constexpr std::string_view kLocalSetArray = "YDWESetAnyTypeLocalArray";
inline constexpr std::string_view kLoopLocal = "YDWEForLoopLocVarMultiple";
inline constexpr std::string_view kLoopLocalGet = "YDWEForLoopLocVarIndex";

// 全图参数类型审计：衡量类型系统对实际地图（YDWE 制作）的覆盖程度。
struct ArgumentTypeIssue {
    std::string trigger;
    std::string function;
    std::size_t position = 0;
    std::string declared;
    std::string expected;
    std::string actual;
    std::string problem; // unresolved / mismatch / local-type-hint / unknown-type
};

struct ArgumentTypeAudit {
    std::size_t arguments = 0;
    std::size_t nullArguments = 0;      // 声明为 Null 的参数
    std::size_t resolvedNull = 0;       // 其中解析出具体类型的
    std::size_t anyReturnValues = 0;    // 值来自 AnyReturnType 函数
    std::size_t unresolved = 0;
    std::size_t mismatched = 0;
    std::size_t localTypeHints = 0;     // 局部变量读取与所在参数类型不一致（可能跨触发器传递）
    std::size_t unknownTypes = 0;       // 期望类型不在 TriggerTypes 中
    std::size_t missingDefinitions = 0; // ECA 定义缺失（不属于类型问题，单独计）
    std::map<std::string, std::size_t> problemsByFunction;
    // 块语句的子节点分布：“函数名 #分支号 类型” → 数量（用于核对各函数库块语句的分支约定）。
    std::map<std::string, std::size_t> blockBranches;
    std::vector<ArgumentTypeIssue> samples; // 最多 200 条
};

[[nodiscard]] ArgumentTypeAudit AuditArgumentTypes(const TriggerDocument& document, const UiCatalog& catalog);

} // namespace War3Map::Trigger
