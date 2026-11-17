#include "War3MapCore/Trigger/TriggerBlocks.h"

namespace War3Map::Trigger {

bool IsIfThenElseBlock(std::string_view name) noexcept {
    return name.find("IfThenElse") != std::string_view::npos && name.ends_with("Multiple");
}

const std::vector<TriggerBlockBranch>* FindTriggerBlockBranches(std::string_view name, const UiCatalog* catalog) {
    static const std::vector<TriggerBlockBranch> ifBlock{
        {0, 1, "如果 - 条件", "if"}, {1, 2, "则 - 动作", "then"}, {2, 2, "否则 - 动作", "else"}};
    static const std::vector<TriggerBlockBranch> conditions{{0, 1, "条件", "conditions"}};
    static const std::vector<TriggerBlockBranch> loop{{0, 2, "循环 - 动作", "actions"}};
    static const std::vector<TriggerBlockBranch> actions{{0, 2, "动作", "actions"}};
    static const std::vector<TriggerBlockBranch> timer{{0, 2, "参数", "params"}, {1, 2, "动作", "actions"}};
    static const std::vector<TriggerBlockBranch> registerTrigger{
        {0, 0, "事件", "events"}, {1, 2, "参数", "params"}, {2, 2, "动作", "actions"}};
    static const std::vector<TriggerBlockBranch> parameters{{0, 2, "参数", "params"}};
    // KKWE 运动器“应用到…”：参数、刷新、到达三块（块名取自 KKWE）。
    static const std::vector<TriggerBlockBranch> moverApply{
        {0, 2, "参数", "params"}, {1, 2, "刷新", "update"}, {2, 2, "到达", "reach"}};
    if (IsIfThenElseBlock(name)) return &ifBlock;
    if (name == "AndMultiple" || name == "OrMultiple") return &conditions;
    if (name == "YDWETimerStartMultiple") return &timer;
    if (name == "MHMover_SetOnUpdate" || name == "MHMover_SetOnReach") return &timer;
    if (name == "MHMover_ApplyUnit" || name == "MHMover_ApplyItem" || name == "MHMover_ApplyDest"
        || name == "MHMover_ApplyEffect" || name == "MHMover_ApplyDoodad")
        return &moverApply;
    if (name == "MHModule_Evaluate") return &parameters;
    if (name == "YDWERegisterTriggerMultiple") return &registerTrigger;
    if (name == "YDWEExecuteTriggerMultiple") return &parameters;
    if (name == "YDWEEnumUnitsInRangeMultiple" || name == "YDWERegionMultiple") return &actions;
    // Dz 异步注册（多项）：块内是回调动作，不是循环。
    if ((name.starts_with("DzTriggerRegister") || name.starts_with("DzFrameSet")) && name.ends_with("Multiple")) return &actions;
    if (name.ends_with("Multiple")) return &loop;
    if (catalog != nullptr && catalog->CallbackActions().Find(name) != nullptr) return &actions;
    return nullptr;
}

} // namespace War3Map::Trigger
