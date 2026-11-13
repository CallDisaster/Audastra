#pragma once

#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"

namespace War3Map::Trigger {

enum class VariableReferenceKind { Declared, GeneratedTrigger, PreplacedObject };
struct VariableReference {
    VariableReferenceKind kind = VariableReferenceKind::Declared;
    std::string type;
    bool isArray = false;
    bool writable = false;
    std::string dependencySha256;
};

// 同一解析用于图插入、旧闭包guard和参数类型。没有真实来源时不猜gg_前缀。
[[nodiscard]] Result<VariableReference> ResolveVariableReference(
    const TriggerDocument& document, std::string_view name,
    const MapObjectReferenceSnapshot* mapObjects = nullptr);

class TriggerTypeSystem;

struct GlobalReferenceCandidate {
    VariableReferenceKind kind = VariableReferenceKind::Declared;
    std::string value; // 参数里实际存的引用：变量原名或 gg_trg_ 生成名
    std::string name;  // 显示名：变量名或触发器名
    std::string type;
};
struct GlobalReferenceCandidates {
    std::vector<GlobalReferenceCandidate> items; // 先变量（声明顺序），后触发器（目录顺序）
    std::size_t arrays = 0;           // 需要下标、未列出的数组变量
    std::size_t clashingTriggers = 0; // 生成名重复、不能引用的触发器
};
// 参数“变量”来源可直接选的全局引用；取值面板、参数对话框共用，资格与 ResolveVariableReference 一致。
// anyGlobal（AnyGlobal 槽）不按类型过滤；writableTarget 只列可赋值的用户变量，不列生成的触发器。
// 预放对象需要密封快照，由调用方另行列出。
[[nodiscard]] GlobalReferenceCandidates CollectGlobalReferenceCandidates(const TriggerDocument& document,
    const TriggerTypeSystem& types, std::string_view expectedType, bool anyGlobal, bool writableTarget);

} // namespace War3Map::Trigger
