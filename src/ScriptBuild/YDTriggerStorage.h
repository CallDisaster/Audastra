#pragma once
#include "War3MapCore/Trigger/TriggerStorage.h"

#include <span>
#include <string>
#include <string_view>
#include <utility>

// YDTrigger 存储宏（YDLocal* / YDUserData* / YDHash*，见 YDWE compiler/include/YDTrigger/Hash.h 等）
// 在 1.24+ 哈希表版本下的最终展开形式。生成器直接写出这些原生调用，不再依赖 Wave 展开宏、
// 模板求 StringHash，也不依赖 YDWE 每次保存时改写的 YDTrigger/Import.h。
// 展开结果与 Wave + 模板求值后的文本逐记号一致（只研究其行为，独立实现）。
namespace War3Map::Script::detail {

// 局部变量表：YDLOC；自定义值（UserData）表：YDHT。均由 YDTriggerSaveLoadSystem 库声明。
inline constexpr std::string_view kLocalTable = "YDLOC";
inline constexpr std::string_view kUserDataTable = "YDHT";

// 某个存储类型（YDTrigger 的 type 参数，如 integer / unit / StringExt）对应的原生哈希表函数。
using Trigger::StorageNatives;
using Trigger::StorageFor;

// 常量键：StringHash(name) 的编译期结果，写成 0x%08X（name 中的反斜杠转义与模板求值一致）。
[[nodiscard]] std::string StorageKey(std::string_view name);

// 局部变量页（YDLOCAL_1 … YDLOCAL_5）的表达式。
[[nodiscard]] std::string LocalPage(int page);
// 以句柄为页（YDLocalSet(page, …)）：GetHandleId(handle)。
[[nodiscard]] std::string HandlePage(std::string_view handle);

// YDHashAny2I(type, value) 的前后缀：值写在两者之间。known=false 时同 StorageNatives。
struct TableWrap {
    std::string prefix;
    std::string suffix;
    bool known = true;
};
[[nodiscard]] TableWrap UserDataTableWrap(std::string_view type);

// YDLocalInitialize()：触发器动作开头为本次执行分配局部变量页（四行，不含缩进）。
[[nodiscard]] std::span<const std::string_view> LocalInitializeLines();
// YDLocalReset()：等待之后恢复本次执行的页号。
[[nodiscard]] std::string LocalResetCall();
// YDLocalExecuteTrigger(trigger)：为被执行的触发器预分配页号。
[[nodiscard]] std::string LocalExecuteTrigger(std::string_view trigger);

// YDWEGetObjectProperty* 的物体类型常量（YDWE_OBJECT_TYPE_UNIT …）对应的 slk 表名；未知时为空。
[[nodiscard]] std::string_view ObjectPropertyTable(std::string_view objectType);

// 把一行代码中的标识符 name（不在字符串、四字符码、注释内）替换为 value：
// 等价于只有一个参数的宏替换，用于展开“任意玩家”事件的 16 次注册。
[[nodiscard]] std::string ReplaceIdentifier(std::string_view line, std::string_view name, std::string_view value);

} // namespace War3Map::Script::detail
