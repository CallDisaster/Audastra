#pragma once

#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/SourceMap.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 自研 vJass 翻译（替代 jasshelper 的常用子集），输出按 jasshelper 的可观察结果对齐（独立实现）：
//   - library / library_once、requires / needs / uses（含 optional）、initializer：按依赖分层、同层按名称排序输出，
//     初始化函数在 main 的 InitBlizzard 之后用 ExecuteFunc 调用；
//   - globals 合并到脚本开头，native 移到 globals 之后；private / public 名称改写为 库__名 / 库_名，
//     与地图全局变量同名的参数和局部变量改名为 l__名；非调试模式删除 debug 语句；
//   - struct：成员与静态成员、方法与静态方法、method operator（[]、[]=、名称、名称=）、create / allocate / destroy、
//     thistype、类型转换、extends（单层）与 onDestroy；
//   - 把 function 传给 boolexpr 参数时补 Condition(...)。
// 不支持的特性（textmacro、module、interface、hook、static if、带依赖或结构的 scope、zinc、//! 指令、调试模式等）
// 不会猜测翻译：结果的 unsupported 给出原因，由调用方改用 jasshelper。
namespace War3Map::Jass {

struct VJassOptions {
    bool debug = false;
    // 游戏脚本（common.j、blizzard.j）：提供原生函数签名（boolexpr 参数识别）；检查期间须保持有效。
    std::vector<JassSource> baseScripts;
    // jasshelper__initstructs 的数字后缀；0 表示按内容计算（同一输入得到同一输出）。
    std::uint32_t initSuffix = 0;
    // 与保存/代码检查相同的硬预算；翻译前端和函数体解析也受此限制。
    JassCheckOptions check;
};

// 每次实际发射的字节片段。原片段按行拆开，exact=true 时可逐字节回定位；
// 名称改写/模板/运算符生成仅有明确源码锚点，exact=false 不冒称精确位置。
struct VJassByteOrigin {
    std::uint32_t outputOffset = 0;
    std::uint32_t byteLength = 0;
    std::uint32_t sourceOffset = 0;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0; // 从1开始的原字节列；宿主可按实际编码转换显示列。
    bool exact = false;
};

struct VJassResult {
    std::string text;
    std::vector<Diagnostic> diagnostics;
    std::string unsupported; // 非空：含有尚未支持的特性（原因与位置），应改用 jasshelper
    std::size_t libraries = 0;
    std::size_t structs = 0;
    std::size_t functions = 0;
    bool completed = true;
    SourceMap sourceMap; // 实际输出片段 → 该次输入的原始行；可与预处理映射复合。
    std::vector<VJassByteOrigin> byteOrigins;
    std::size_t tokens = 0;
    std::size_t astNodes = 0;
    std::size_t maxAstDepthObserved = 0;

    [[nodiscard]] bool Succeeded() const noexcept;
    [[nodiscard]] std::optional<VJassByteOrigin> FindByteOrigin(std::size_t outputOffset) const noexcept;
};

[[nodiscard]] VJassResult TranslateVJass(std::string_view text, const std::string& name, const VJassOptions& options = {});

} // namespace War3Map::Jass
