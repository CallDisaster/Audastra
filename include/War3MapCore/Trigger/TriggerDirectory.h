#pragma once

#include "War3MapCore/Trigger/TriggerEditing.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace War3Map::Trigger {

// 触发器目录（原版“类别”）布局：类别表（含顺序与名称）与每个触发器的所属类别和先后顺序。
// 目录操作只改这两者，不动 ECA / 参数节点；撤销时整体恢复布局即可。
struct TriggerDirectoryLayout {
    std::vector<CategoryRecord> categories;
    // 按文档顺序：触发器稳定 ID 与所属 categoryId。
    std::vector<std::pair<StableId, std::int32_t>> triggers;
};

// 复制触发器的快照：触发器记录（名称、标志、注释、自定义文本）与各顶层 ECA 子树。
// 快照不引用文档，源触发器删除后仍可粘贴。
struct TriggerSnapshot {
    TriggerRecord trigger;
    std::vector<TriggerDocumentEditor::EcaSubtreeSnapshot> roots;
};

class TriggerDirectoryEditor {
public:
    [[nodiscard]] static TriggerDirectoryLayout CaptureLayout(const TriggerDocument& document);
    // 按布局恢复类别表与触发器顺序 / 类别。布局中没有的触发器保持相对顺序排在后面，
    // 文档中已不存在的触发器忽略（与删除触发器的撤销命令组合时两者先后都成立）。
    static void ApplyLayout(TriggerDocument& document, const TriggerDirectoryLayout& layout);

    // 原版命名方式：“<base> 001”，递增到不重名为止。
    [[nodiscard]] static std::string UniqueName(
        const std::vector<std::string>& existing, std::string_view base);
    // 同时保证脚本名（ScriptName）不与其他触发器冲突。
    [[nodiscard]] static std::string UniqueTriggerName(const TriggerDocument& document, std::string_view base);
    // 触发器在脚本中的标识符（gg_trg_ 之后的部分，WE 规则）：去掉结尾空格，非 [A-Za-z0-9_] 的每个字节
    // 换成 '_'，以 '_' 结尾时补 'u'。中文名只按字节数区分，因此不同名称可能得到相同标识符。
    [[nodiscard]] static std::string ScriptName(std::string_view name);
    // WE 按名字在 WTG 里实际存储的字节换算（规则同上）：GBK 存的汉字一个换两个 '_'，UTF-8 存的换三个。
    // 名字无法按原编码写出时退回按 UTF-8 文字换算。
    [[nodiscard]] static std::string ScriptName(const RawText& name);
    // 触发器的脚本全局变量名，也是 WE 写进 WTG 触发器类型参数的值：WE 生成脚本时把参数值原样写出，不再换算。
    [[nodiscard]] static std::string TriggerSymbol(const RawText& name);
    // value 是否引用该触发器：WE 写法（TriggerSymbol），以及本编辑器旧版本写过的按 UTF-8 文字换算、
    // 直接写原名两种形式。旧形式在 WE 里生成不出对应声明，只为读取和迁移保留。
    [[nodiscard]] static bool IsTriggerSymbolOf(std::string_view value, const RawText& triggerName);
    // 参数值指向的触发器（不含注释条目）：先按 WE 写法匹配，没有时再认旧形式；同一层有多个匹配时返回
    // nullptr 并置 ambiguous。
    [[nodiscard]] static const TriggerRecord* FindTriggerBySymbol(
        const TriggerDocument& document, std::string_view value, bool* ambiguous = nullptr);
    // 返回脚本名与 name 相同的其他触发器；没有冲突时返回 nullptr。
    [[nodiscard]] static const TriggerRecord* FindScriptNameConflict(
        const TriggerDocument& document, std::string_view name, StableId self = 0);
    [[nodiscard]] static std::string UniqueCategoryName(const TriggerDocument& document, std::string_view base);

    // 名称校验（界面层使用）：去掉首尾空白后不能为空、不能含 NUL、不能与其他触发器 / 类别同名；
    // 触发器还不能与其他触发器的脚本名相同（否则生成的函数与 gg_trg_ 变量重复定义）。
    [[nodiscard]] static Status ValidateTriggerName(
        const TriggerDocument& document, std::string_view name, StableId self = 0);
    [[nodiscard]] static Status ValidateCategoryName(
        const TriggerDocument& document, std::string_view name, std::optional<std::int32_t> self = std::nullopt);

    // 新建类别，插到 afterCategoryId 之后（缺省放末尾）；返回新的 categoryId。
    [[nodiscard]] static Result<std::int32_t> AddCategory(
        TriggerDocument& document, std::string name, std::optional<std::int32_t> afterCategoryId = std::nullopt);
    [[nodiscard]] static Status RenameCategory(TriggerDocument& document, std::int32_t categoryId, std::string name);
    // 只删除空类别；有触发器时由调用方先删除其中的触发器（各自可撤销）。
    [[nodiscard]] static Status RemoveEmptyCategory(TriggerDocument& document, std::int32_t categoryId);
    // 移动类别到 beforeCategoryId 之前（缺省放末尾）。
    [[nodiscard]] static Status MoveCategory(
        TriggerDocument& document, std::int32_t categoryId, std::optional<std::int32_t> beforeCategoryId);
    // 移动触发器到类别：beforeTriggerId 非 0 时插在它之前（须在目标类别内），否则放在类别末尾。
    [[nodiscard]] static Status MoveTrigger(
        TriggerDocument& document, StableId triggerId, std::int32_t categoryId, StableId beforeTriggerId = 0);

    [[nodiscard]] static Result<TriggerSnapshot> CopyTrigger(const TriggerDocument& document, StableId triggerId);
    // 粘贴为新触发器：全部节点换新稳定 ID；放在同类别的 afterTriggerId 之后，否则放在类别末尾。
    // 任一子树粘贴失败时整体回滚，文档不变。
    [[nodiscard]] static Result<StableId> PasteTrigger(
        TriggerDocument& document,
        const UiCatalog& catalog,
        const TriggerSnapshot& snapshot,
        std::int32_t categoryId,
        std::string name,
        StableId afterTriggerId = 0);
};

} // namespace War3Map::Trigger
