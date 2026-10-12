#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace War3Map::Trigger {

class UiCatalog;

// 函数库 J 中的一条顶层 native/function 声明（只记录名称与参数类型）。
struct UiJassDeclaration {
    std::string name;
    std::vector<std::string> parameterTypes;
    std::filesystem::path source;
    std::uint32_t line = 0;
};

// 回调块动作：UI 定义比真实 JASS 声明正好少末尾一个 code 参数。
// 该参数不让作者填写，而由块内动作生成的独立函数传入（与“注册触发器”的回调一样）。
struct UiCallbackAction {
    std::size_t codeArgument = 0; // JASS 参数下标，等于 UI 参数个数。
    // 遍历类（标题含“做动作”“做多动作”或“遍历”）在调用时立即逐个执行回调，与“选取单位组做动作”一样
    // 共用外层触发器的局部变量；其余（创建模块、注册监听、设置回调等）之后才执行，局部变量独立。
    bool sharesLocals = false;
    std::filesystem::path source;
    std::uint32_t line = 0;
};

struct UiCallbackActionIndex {
    std::unordered_map<std::string, UiCallbackAction> actions; // 键为动作定义名。
    std::vector<Diagnostic> diagnostics; // 同名声明冲突、扫描预算等；不影响其他动作。
    bool complete = true; // false 表示扫描因预算或读取失败中止，未列出的动作不能视为普通动作的证明。
    [[nodiscard]] const UiCallbackAction* Find(std::string_view actionName) const;
};

// 行级词法扫描：去掉行尾 // 注释后识别 [修饰词] native|function 名 takes 参数 returns 类型。
// 不执行预处理，不解析函数体；最终调用是否匹配仍由保存时对完整展开脚本的 JASS 检查决定。
[[nodiscard]] std::vector<UiJassDeclaration> ScanUiJassDeclarations(std::string_view text,
    const std::filesystem::path& source);

// 规则只看 UI 动作定义与声明：脚本名唯一对应一组参数类型、个数为 UI 参数 + 1、末尾为 code。
// 同名声明的参数类型不一致时不认定，并记录诊断。
[[nodiscard]] UiCallbackActionIndex BuildUiCallbackActionIndex(const UiCatalog& catalog,
    std::span<const UiJassDeclaration> declarations);

// 函数库 J 的扫描范围：已启用各层的 <ui根>/<层>/jass；旧版 share/mpq 布局另含 <工具根>/jass。
// 递归读取 .j，不跟随符号链接；有文件数与字节预算，超出时 complete=false 并给出诊断。
[[nodiscard]] std::vector<std::filesystem::path> UiCallbackScanRoots(const std::filesystem::path& uiRoot,
    std::span<const std::string> layers);
struct UiJassDeclarationScan {
    std::vector<UiJassDeclaration> declarations;
    std::vector<Diagnostic> diagnostics;
    bool complete = true;
};
[[nodiscard]] UiJassDeclarationScan ScanUiJassRoots(std::span<const std::filesystem::path> roots);

} // namespace War3Map::Trigger
