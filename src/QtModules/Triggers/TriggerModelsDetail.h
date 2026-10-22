#pragma once


#include "Icons/GeometricIcons.h"
#include "Icons/IconProvider.h"
#include "War3MapCore/Trigger/TriggerBlocks.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QModelIndex>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QString>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <utility>


// 触发器目录 / 逻辑模型共用的句柄编码与显示工具（仅模块内部使用）。
namespace TriggerModelsDetail {

inline constexpr quintptr kTriggerIndexTag =
    quintptr{1} << (std::numeric_limits<quintptr>::digits - 1);
inline constexpr quintptr kRoleHeaderTag =
    quintptr{1} << (std::numeric_limits<quintptr>::digits - 2);
inline constexpr quintptr kRoleSlotMask = 0xF; // 角色槽位 4 位（每节点最多 16 个分支组）

// ECA 树句柄编码（quintptr）：
//   0                        = 无效/根；
//   bit63=1, bit62=0         = 顶层分组头（低位为分组 0/1/2）；
//   bit63=1, bit62=1         = 函数体角色分组头，低位 = (parentNodeIndex << 4) | roleSlot；
//   其余                     = ECA 节点，句柄 = nodeIndex + 1。
inline bool IsTopGroupHandle(const QModelIndex& index) {
    return index.isValid() && (index.internalId() & kTriggerIndexTag) != 0 &&
        (index.internalId() & kRoleHeaderTag) == 0;
}
inline bool IsRoleHeaderHandle(const QModelIndex& index) {
    return index.isValid() && (index.internalId() & kTriggerIndexTag) != 0 &&
        (index.internalId() & kRoleHeaderTag) != 0;
}
inline int GroupOfHandle(const QModelIndex& index) {
    return static_cast<int>(index.internalId() & ~(kTriggerIndexTag | kRoleHeaderTag));
}
inline std::uint32_t RoleParentOfHandle(const QModelIndex& index) {
    return static_cast<std::uint32_t>(
        (index.internalId() & ~(kTriggerIndexTag | kRoleHeaderTag)) >> 4);
}
inline int RoleSlotOfHandle(const QModelIndex& index) {
    return static_cast<int>(index.internalId() & kRoleSlotMask);
}
inline quintptr MakeRoleHeaderHandle(std::uint32_t parentNodeIndex, int slot) {
    return kTriggerIndexTag | kRoleHeaderTag |
        ((static_cast<quintptr>(parentNodeIndex) << 4) |
         (static_cast<quintptr>(slot) & kRoleSlotMask));
}

inline QString Utf8(const std::string& value) {
    return QString::fromUtf8(value);
}

inline QString EcaKindName(std::int32_t type) {
    switch (type) {
    case 0: return QObject::tr("事件");
    case 1: return QObject::tr("条件");
    case 2: return QObject::tr("动作");
    case 3: return QObject::tr("调用");
    default: return QObject::tr("未知类型 %1").arg(type);
    }
}

// 顶层 ECA 类型→分组：0 事件 / 1 条件 / 其余归入动作。
inline constexpr std::uint8_t kNoGroup = 0xFF;
inline int TypeToGroup(std::int32_t type) {
    switch (type) {
    case 0: return 0;
    case 1: return 1;
    default: return 2;
    }
}

inline QString GroupName(int group) {
    switch (group) {
    case 0: return QObject::tr("事件");
    case 1: return QObject::tr("条件");
    case 2: return QObject::tr("动作");
    default: return QObject::tr("其它");
    }
}

// 注释动作（WE/YDWE 的 CommentString 系）：整行绿字，与 WE 观感一致。
inline bool IsCommentEca(const War3Map::Trigger::EcaNode& eca) {
    return eca.name.text.starts_with("CommentString");
}

// 嵌套子节点的角色标签（对应 WE 的 If/Then/Else、循环体、条件分支）。
// 依据子节点类型 + 分支号（childId）判定，WTG 格式保证可靠：
//   条件(type1)→条件（If 判断 / And / Or 子条件）；
//   动作(type2)：分支1→则（Then）、分支2→否则（Else）、分支0→动作（循环/枚举体）。
inline QString NestedRoleLabel(std::int32_t branch, std::int32_t type) {
    switch (type) {
    case 0: return QObject::tr("事件");
    case 1: return QObject::tr("条件");
    case 2:
        switch (branch) {
        case 1: return QObject::tr("则");
        case 2: return QObject::tr("否则");
        default: return QObject::tr("动作");
        }
    case 3: return QObject::tr("调用");
    default: return EcaKindName(type);
    }
}

// 块语句的分支表在核心层（War3MapCore/Trigger/TriggerBlocks.h），界面与紧凑 T 写法共用。
// 声明的分支即使为空也显示，这样可以直接往空的“否则”“参数”里添加语句。
using BranchSpec = War3Map::Trigger::TriggerBlockBranch;

inline bool IsIfBlock(std::string_view name) {
    return War3Map::Trigger::IsIfThenElseBlock(name);
}

inline const std::vector<BranchSpec>* KnownBlock(std::string_view name, const War3Map::Trigger::UiCatalog* catalog = nullptr) {
    return War3Map::Trigger::FindTriggerBlockBranches(name, catalog);
}

inline std::vector<BranchSpec> DeclaredBranches(std::string_view name, const War3Map::Trigger::UiCatalog* catalog = nullptr) {
    const auto* block = KnownBlock(name, catalog);
    return block == nullptr ? std::vector<BranchSpec>{} : *block;
}

inline QString BranchLabel(std::string_view parentName, std::int32_t branch, std::int32_t type,
    const War3Map::Trigger::UiCatalog* catalog = nullptr) {
    if (const auto* block = KnownBlock(parentName, catalog)) {
        for (const BranchSpec& spec : *block) {
            if (spec.branch == branch && spec.label != nullptr) return QString::fromUtf8(spec.label);
        }
    }
    return NestedRoleLabel(branch, type);
}

// 程序化回退图标：未配置魔兽路径或无真实图标时按事件/条件/动作/调用区分。
// 命运2 极简科幻风格：细线条几何构型（角状脉冲/菱形门/双箭头/圆+三角）。
inline QIcon FallbackEcaIcon(std::int32_t type) {
    switch (type) {
    case 0: return GeometricIcons::EcaEvent();
    case 1: return GeometricIcons::EcaCondition();
    case 2: return GeometricIcons::EcaAction();
    case 3: return GeometricIcons::EcaCall();
    default: return GeometricIcons::EcaComment();
    }
}

// 禁用节点图标：断裂圆环 + 斜线（命运2 式失效标记）。
inline QIcon DisabledGearIcon() {
    return GeometricIcons::EcaDisabled();
}

} // namespace TriggerModelsDetail
