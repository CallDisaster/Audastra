#pragma once

#include <array>

namespace War3Map::MapData {

struct ClassicProfileInput { const char* path; bool alwaysLoad; };
// 本编辑器采用的完整经典固定初始化链；不声称是任意运行中编辑器的最终profile。
inline constexpr std::array<ClassicProfileInput, 56> kClassicProfiles{{
    {"Units/MiscData.txt", false}, {"Units/MiscGame.txt", false},
    {"UI/MiscData.txt", true}, {"UI/WorldEditData.txt", true},
    {"UI/WorldEditStrings.txt", false}, {"UI/WorldEditGameStrings.txt", true},
    {"UI/UnitEditorData.txt", false}, {"UI/AIEditorData.txt", false},
    {"UI/WorldEditLayout.txt", false}, {"Units/CommandFunc.txt", false},
    {"Units/CommandStrings.txt", false}, {"UI/TriggerData.txt", false},
    {"UI/TriggerStrings.txt", false}, {"Units/HumanUnitStrings.txt", false},
    {"Units/NeutralUnitStrings.txt", false}, {"Units/NightElfUnitStrings.txt", false},
    {"Units/OrcUnitStrings.txt", false}, {"Units/UndeadUnitStrings.txt", false},
    {"Units/CampaignUnitStrings.txt", false}, {"Units/UnitGlobalStrings.txt", false},
    {"Units/HumanUnitFunc.txt", false}, {"Units/NeutralUnitFunc.txt", false},
    {"Units/NightElfUnitFunc.txt", false}, {"Units/OrcUnitFunc.txt", false},
    {"Units/UndeadUnitFunc.txt", false}, {"Units/CampaignUnitFunc.txt", false},
    {"Units/ItemStrings.txt", false}, {"Units/ItemFunc.txt", false},
    {"Units/HumanAbilityFunc.txt", false}, {"Units/NeutralAbilityFunc.txt", false},
    {"Units/NightElfAbilityFunc.txt", false}, {"Units/OrcAbilityFunc.txt", false},
    {"Units/UndeadAbilityFunc.txt", false}, {"Units/CommonAbilityFunc.txt", false},
    {"Units/ItemAbilityFunc.txt", false}, {"Units/CampaignAbilityFunc.txt", false},
    {"Units/HumanAbilityStrings.txt", false}, {"Units/NeutralAbilityStrings.txt", false},
    {"Units/NightElfAbilityStrings.txt", false}, {"Units/OrcAbilityStrings.txt", false},
    {"Units/UndeadAbilityStrings.txt", false}, {"Units/CommonAbilityStrings.txt", false},
    {"Units/ItemAbilityStrings.txt", false}, {"Units/CampaignAbilityStrings.txt", false},
    {"Units/HumanUpgradeFunc.txt", false}, {"Units/OrcUpgradeFunc.txt", false},
    {"Units/UndeadUpgradeFunc.txt", false}, {"Units/NightElfUpgradeFunc.txt", false},
    {"Units/CampaignUpgradeFunc.txt", false}, {"Units/NeutralUpgradeFunc.txt", false},
    {"Units/HumanUpgradeStrings.txt", false}, {"Units/OrcUpgradeStrings.txt", false},
    {"Units/UndeadUpgradeStrings.txt", false}, {"Units/NightElfUpgradeStrings.txt", false},
    {"Units/CampaignUpgradeStrings.txt", false}, {"Units/NeutralUpgradeStrings.txt", false},
}};

} // namespace War3Map::MapData
