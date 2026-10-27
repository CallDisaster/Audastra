#include "War3MapCore/Text/TextUtil.h"
#include "ObjectClassification.h"

#include "ObjectModels.h"

#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <QCoreApplication>

#include <algorithm>
#include <cctype>

namespace War3Map::Modules::ObjectDetail {
namespace {

using ObjectData::ObjectType;

QString Tr(const char* text) {
    return QCoreApplication::translate("ObjectClassification", text);
}



bool Flag(const std::string& value) {
    return value == "1" || Text::LowerCType(value) == "true";
}

// 单位种族：五个可玩种族与娜迦单独成文件夹，其余按中立敌对 / 中立被动（hostilePal）。
QString UnitRace(const std::string& race, bool hostile) {
    const std::string value = Text::LowerCType(race);
    if (value == "human") return Tr("人族");
    if (value == "orc") return Tr("兽族");
    if (value == "undead") return Tr("不死族");
    if (value == "nightelf") return Tr("暗夜精灵族");
    if (value == "naga") return Tr("中立 - 娜迦");
    return hostile ? Tr("中立敌对") : Tr("中立被动");
}

// 技能 / 魔法效果 / 科技的种族：creeps 为中立敌对，critters / commoner 为中立被动，其余为特殊。
QString OtherRace(const std::string& race) {
    const std::string value = Text::LowerCType(race);
    if (value == "human") return Tr("人族");
    if (value == "orc") return Tr("兽族");
    if (value == "undead") return Tr("不死族");
    if (value == "nightelf") return Tr("暗夜精灵族");
    if (value == "naga") return Tr("中立 - 娜迦");
    if (value == "creeps") return Tr("中立敌对");
    if (value == "critters" || value == "commoner") return Tr("中立被动");
    return Tr("特殊");
}

QString ItemClass(const std::string& value) {
    const std::string lower = Text::LowerCType(value);
    if (lower == "permanent") return Tr("永久的");
    if (lower == "charged") return Tr("可充的");
    if (lower == "powerup") return Tr("力量提升");
    if (lower == "artifact") return Tr("人造物品");
    if (lower == "purchasable") return Tr("可购买的");
    if (lower == "campaign") return Tr("战役");
    if (lower == "miscellaneous") return Tr("混杂的");
    return Tr("未知");
}

QString DoodadCategory(const std::string& value) {
    switch (value.empty() ? '\0' : static_cast<char>(std::toupper(static_cast<unsigned char>(value.front())))) {
    case 'O': return Tr("道具");
    case 'S': return Tr("建筑");
    case 'W': return Tr("水");
    case 'C': return Tr("悬崖/地形");
    case 'E': return Tr("环境");
    case 'Z': return Tr("电影");
    default: return Tr("其他");
    }
}

QString DestructableCategory(const std::string& value) {
    switch (value.empty() ? '\0' : static_cast<char>(std::toupper(static_cast<unsigned char>(value.front())))) {
    case 'D': return Tr("树/可破坏");
    case 'P': return Tr("路径阻断器");
    case 'B': return Tr("桥/斜坡");
    default: return Tr("其他");
    }
}

} // namespace

QString RootLabel(ObjectType type, bool custom) {
    static const char* const kNames[] = {"单位", "技能", "物品", "魔法效果", "科技", "装饰物", "可破坏物"};
    const auto index = static_cast<std::size_t>(type);
    const QString name = index < std::size(kNames) ? Tr(kNames[index]) : QString{};
    return (custom ? Tr("自定义") : Tr("标准")) + name;
}

QStringList Classify(ObjectType type, const std::string& rawcode, const ClassificationLookup& get) {
    switch (type) {
    case ObjectType::Unit: {
        const QString race = UnitRace(get("race", "urac"), Flag(get("hostilepal", "uhos")));
        const QString melee = Flag(get("campaign", "ucam")) ? Tr("战役") : Tr("对战");
        QString kind = Tr("单位");
        if (Flag(get("special", "uspe"))) kind = Tr("特殊");
        else if (Flag(get("isbldg", "ubdg"))) kind = Tr("建筑");
        else if (!rawcode.empty() && std::isupper(static_cast<unsigned char>(rawcode.front()))) kind = Tr("英雄");
        return {race, melee, kind};
    }
    case ObjectType::Ability:
        if (Flag(get("item", "aite"))) return {Tr("物品")};
        return {OtherRace(get("race", "arac")), Flag(get("hero", "aher")) ? Tr("英雄") : Tr("单位")};
    case ObjectType::Buff:
        return {OtherRace(get("race", "frac")), Flag(get("iseffect", "feff")) ? Tr("效果") : Tr("魔法效果")};
    case ObjectType::Upgrade:
        return {OtherRace(get("race", "grac"))};
    case ObjectType::Item:
        return {ItemClass(get("class", "icla"))};
    case ObjectType::Doodad:
        return {DoodadCategory(get("category", "dcat"))};
    case ObjectType::Destructable:
        return {DestructableCategory(get("category", "bcat"))};
    }
    return {};
}

QStringList ClassifyStandard(const ObjectData::StandardObjectEntry& entry) {
    return Classify(entry.key.type, entry.key.rawcode, [&entry](std::string_view slk, std::string_view) {
        const auto found = entry.fields.find(std::string(slk));
        return found == entry.fields.end() ? std::string{} : found->second;
    });
}

QStringList ClassifyCustom(const ObjectInheritanceChain& chain, ObjectType type, const std::string& rawcode) {
    return Classify(type, rawcode, CustomLookup(chain));
}

std::string CategoryCode(ObjectType type, const ClassificationLookup& get) {
    if (type == ObjectType::Doodad) return get("category", "dcat");
    if (type == ObjectType::Destructable) return get("category", "bcat");
    return {};
}

ClassificationLookup CustomLookup(const ObjectInheritanceChain& chain) {
    return [&chain](std::string_view slk, std::string_view native) {
        // 自身与地图父链上的修改优先（原生字段 ID；LNI 文档也可能用 SLK 名）。
        for (const auto* record : chain.records) {
            for (const auto& modification : record->modifications) {
                const std::string id = Text::LowerCType(modification.key.fieldId);
                if (id == native || id == slk) return ObjectData::ValueToDisplayString(modification.value);
            }
        }
        if (chain.standardBase != nullptr) {
            const auto found = chain.standardBase->fields.find(std::string(slk));
            if (found != chain.standardBase->fields.end()) return found->second;
        }
        return std::string{};
    };
}

int FolderRank(const QString& label, int depth) {
    // 单位的第三层（单位 / 建筑 / 英雄 / 特殊）：“特殊”排在最后，与种族层的“特殊”位置不同。
    static const QStringList kUnitKinds = {Tr("单位"), Tr("建筑"), Tr("英雄"), Tr("特殊")};
    if (depth >= 2 && kUnitKinds.contains(label)) return 100 + static_cast<int>(kUnitKinds.indexOf(label));
    static const QStringList kOrder = {
        Tr("人族"), Tr("兽族"), Tr("不死族"), Tr("暗夜精灵族"), Tr("中立 - 娜迦"), Tr("中立敌对"), Tr("中立被动"),
        Tr("物品"), Tr("特殊"),
        Tr("对战"), Tr("战役"),
        Tr("单位"), Tr("建筑"), Tr("英雄"),
        Tr("魔法效果"), Tr("效果"),
        Tr("永久的"), Tr("可充的"), Tr("力量提升"), Tr("人造物品"), Tr("可购买的"), Tr("混杂的"),
        Tr("道具"), Tr("水"), Tr("悬崖/地形"), Tr("环境"), Tr("电影"),
        // 可破坏物与原版目录同序：桥/斜坡、路径阻断器、树/可破坏。
        Tr("桥/斜坡"), Tr("路径阻断器"), Tr("树/可破坏"),
    };
    const int index = static_cast<int>(kOrder.indexOf(label));
    return index < 0 ? 1000 : index;
}

} // namespace War3Map::Modules::ObjectDetail
