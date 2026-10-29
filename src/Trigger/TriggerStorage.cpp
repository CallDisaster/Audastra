#include "War3MapCore/Trigger/TriggerStorage.h"
#include <array>
namespace War3Map::Trigger {
namespace {
// type → (Load/Save 后缀, Have/Remove 后缀, 是否句柄)。与 HTSet_Get.h 的 YDTRIGGER_<type>(SG)、
// Has_Clear.h 的 YDTRIGGER_HC_<type>(HC) 及（较新版本才有的）HTSystem.j 的 YDTRIGGER_SET_<type> 对应。
struct StorageType {
    std::string_view type;
    std::string_view value;   // Integer / Real / Boolean / Str / UnitHandle …
    std::string_view checked; // Integer / Real / Boolean / String / Handle
    bool handle;
};

constexpr std::array kStorageTypes = {
    StorageType{"integer", "Integer", "Integer", false},
    StorageType{"real", "Real", "Real", false},
    StorageType{"boolean", "Boolean", "Boolean", false},
    StorageType{"string", "Str", "String", false},
    StorageType{"StringExt", "Str", "String", false},
    StorageType{"unitcode", "Integer", "Integer", false},
    StorageType{"abilcode", "Integer", "Integer", false},
    StorageType{"itemcode", "Integer", "Integer", false},
    StorageType{"radian", "Real", "Real", false},
    StorageType{"degree", "Real", "Real", false},
    StorageType{"imagefile", "Str", "String", false},
    StorageType{"modelfile", "Str", "String", false},
    StorageType{"timer", "TimerHandle", "Handle", true},
    StorageType{"trigger", "TriggerHandle", "Handle", true},
    StorageType{"unit", "UnitHandle", "Handle", true},
    StorageType{"item", "ItemHandle", "Handle", true},
    StorageType{"group", "GroupHandle", "Handle", true},
    StorageType{"player", "PlayerHandle", "Handle", true},
    StorageType{"location", "LocationHandle", "Handle", true},
    StorageType{"destructable", "DestructableHandle", "Handle", true},
    StorageType{"force", "ForceHandle", "Handle", true},
    StorageType{"rect", "RectHandle", "Handle", true},
    StorageType{"region", "RegionHandle", "Handle", true},
    StorageType{"sound", "SoundHandle", "Handle", true},
    StorageType{"effect", "EffectHandle", "Handle", true},
    StorageType{"unitpool", "UnitPoolHandle", "Handle", true},
    StorageType{"itempool", "ItemPoolHandle", "Handle", true},
    StorageType{"quest", "QuestHandle", "Handle", true},
    StorageType{"questitem", "QuestItemHandle", "Handle", true},
    StorageType{"timerdialog", "TimerDialogHandle", "Handle", true},
    StorageType{"leaderboard", "LeaderboardHandle", "Handle", true},
    StorageType{"multiboard", "MultiboardHandle", "Handle", true},
    StorageType{"multiboarditem", "MultiboardItemHandle", "Handle", true},
    StorageType{"trackable", "TrackableHandle", "Handle", true},
    StorageType{"dialog", "DialogHandle", "Handle", true},
    StorageType{"button", "ButtonHandle", "Handle", true},
    StorageType{"texttag", "TextTagHandle", "Handle", true},
    StorageType{"lightning", "LightningHandle", "Handle", true},
    StorageType{"image", "ImageHandle", "Handle", true},
    StorageType{"fogstate", "FogStateHandle", "Handle", true},
    StorageType{"fogmodifier", "FogModifierHandle", "Handle", true},
};

const StorageType* FindType(std::string_view type) {
    for (const auto& entry : kStorageTypes) {
        if (entry.type == type) return &entry;
    }
    return nullptr;
}


}
StorageNatives StorageFor(std::string_view type, bool nullSafeHandleSet) {
    const std::string name(type);
    const auto* entry = FindType(type);
    if (entry == nullptr) {
        // 与 YDHashSet/YDHashGet 的展开一致：YDTRIGGER_<type>(SET|GET)，由函数库头文件按类型定义。
        return {"YDTRIGGER_" + name + "(GET)", "YDTRIGGER_" + name + "(SET)", "YDTRIGGER_HC_" + name + "(HAS)",
            "YDTRIGGER_HC_" + name + "(CLEAR)", false};
    }
    const std::string value(entry->value);
    const std::string checked(entry->checked);
    return {"Load" + value, entry->handle && nullSafeHandleSet ? "YDTRIGGER_SET_" + name : "Save" + value, "HaveSaved" + checked,
        "RemoveSaved" + checked, true};
}


}
