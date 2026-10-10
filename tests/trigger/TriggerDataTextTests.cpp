#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <windows.h>

// TriggerData 文本统一读取：CSV 切列、预设、分类前缀与 WESTRING 本地化。夹具为原创合成的 UI 定义，
// 按公开写法覆盖字面值与 WESTRING 键两种分类名、标志位、多层同名分类与右引号后缺逗号的预设。
namespace {

using namespace War3Map::Trigger;

void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error("trigger data text: " + std::string(message));
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::filesystem::path CreateUi() {
    const auto root = std::filesystem::temp_directory_path() /
        ("War3MapEditor64-TriggerDataText-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::remove_all(root);
    const auto ui = root / "ui";
    WriteText(ui / "config", "base\r\nextra\r\n");
    WriteText(ui / "base" / "define.txt",
        "[TriggerCategories]\r\n"
        "TC_UNIT=WESTRING_TRIGCAT_UNIT,ReplaceableTextures\\WorldEditUI\\Actions-Unit\r\n"
        "TC_LOGIC=WESTRING_TRIGCAT_LOGIC,ReplaceableTextures\\WorldEditUI\\Actions-Logical,1\r\n"
        "TC_LIBRARY=[示例库],icons\\\\Sample.blp\r\n"
        "TC_MISSING=WESTRING_TRIGCAT_NOT_DEFINED,ReplaceableTextures\\WorldEditUI\\Actions-Nothing\r\n"
        "[TriggerTypes]\r\n"
        "unit=1,1,1,WESTRING_TRIGTYPE_UNIT\r\nflag=0,0,0,标志,integer\r\nplain=0,0,0,WESTRING_TRIGTYPE_ABSENT\r\n"
        "[TriggerParams]\r\n"
        "FlagA=0      ,flag,\"FLAG_A\",标志甲\r\n"
        "FlagB=0,flag,\"FLAG_B\"标志乙\r\n"
        "FlagC=0,flag,FLAG_C,WESTRING_FLAG_C\r\n"
        "FlagD=0,flag,FLAG_D,WESTRING_FLAG_ABSENT\r\n");
    WriteText(ui / "base" / "action.txt",
        "[CreateUnit]\r\ntitle=\"创建单位\"\r\ndescription=\"创建 ${单位}\"\r\ncategory=TC_UNIT\r\n[[.args]]\r\ntype=unit\r\n"
        "[IfThen]\r\ntitle=\"如果\"\r\ndescription=\"如果\"\r\ncategory=TC_LOGIC\r\n"
        "[LibraryCall]\r\ntitle=\"库函数\"\r\ndescription=\"库函数\"\r\ncategory = TC_LIBRARY\r\n"
        "[CaseCategory]\r\ntitle=\"大小写\"\r\ndescription=\"大小写\"\r\ncategory=TC_Unit\r\n"
        "[Orphan]\r\ntitle=\"无分类声明\"\r\ndescription=\"无分类声明\"\r\ncategory=TC_UNDECLARED\r\n"
        "[Untitled]\r\ndescription=\"无标题\"\r\n"
        "[Dangling]\r\ntitle=\"键不存在\"\r\ndescription=\"键不存在\"\r\ncategory=TC_MISSING\r\n");
    // 上层同名分类覆盖下层（显示名与图标都换成上层的）。
    WriteText(ui / "extra" / "define.txt",
        "[TriggerCategories]\r\nTC_LIBRARY=[示例库·新],ui\\\\extra\\\\icons\\\\new.blp\r\n");
    return root;
}

void TestCsvSplitting() {
    const auto plain = SplitTriggerDataCsv("0      ,flag,\"FLAG_A\",标志甲");
    Check(plain.size() == 4 && plain[0] == "0" && plain[2] == "FLAG_A" && plain[3] == "标志甲", "padding and quotes");
    const auto glued = SplitTriggerDataCsv("0,flag,\"FLAG_B\"标志乙");
    Check(glued.size() == 4 && glued[2] == "FLAG_B" && glued[3] == "标志乙", "missing comma after closing quote");
    const auto spaced = SplitTriggerDataCsv("0,flag,\"FLAG_B\"  ,标志");
    Check(spaced.size() == 4 && spaced[2] == "FLAG_B" && spaced[3] == "标志", "space before comma is not a column");
    const auto escaped = SplitTriggerDataCsv("0,string,\"a\\\"b,c\",'单,引号'");
    Check(escaped.size() == 4 && escaped[2] == "a\"b,c" && escaped[3] == "单,引号", "escapes and single quotes");
    Check(SplitTriggerDataCsv("").size() == 1, "empty value is one empty column");
}

void TestCatalogText(const UiCatalog& catalog) {
    const TriggerCategoryTable categories(catalog);
    Check(categories.Categories().size() == 4, "four categories");
    const auto* unit = categories.Find("TC_UNIT");
    Check(unit != nullptr && unit->display == "单位" && unit->displayResolved && !unit->hidePrefix, "WESTRING category");
    Check(categories.Find("TC_LOGIC")->hidePrefix, "flag 1 hides prefix");
    const auto* library = categories.Find("TC_LIBRARY");
    Check(library->display == "[示例库·新]" && library->layer == "extra" && library->icon == "ui\\\\extra\\\\icons\\\\new.blp",
        "upper layer overrides category");
    const auto* missing = categories.Find("TC_MISSING");
    Check(!missing->displayResolved && missing->display == "MISSING", "unresolved key falls back to id");
    Check(categories.DisplayName("TC_UNDECLARED") == "UNDECLARED" && categories.DisplayName("") .empty(), "undeclared names");
    Check(categories.Find("tc_unit") == unit && categories.CanonicalId("Tc_Unit") == "TC_UNIT"
        && categories.CanonicalId("TC_UNDECLARED") == "TC_UNDECLARED", "category ids are case-insensitive like WE INI keys");

    const auto title = [&](std::string_view name) {
        return PrefixedTitle(catalog, categories, *catalog.Find(UiDefinitionKind::Action, name));
    };
    Check(title("CreateUnit") == "单位 - 创建单位", "WE-style prefix");
    Check(title("IfThen") == "如果", "no prefix for flagged category");
    Check(title("CaseCategory") == "单位 - 大小写", "category case differs from declaration");
    Check(title("LibraryCall") == "[示例库·新] - 库函数", "literal library category");
    Check(title("Orphan") == "无分类声明", "undeclared category has no prefix");
    Check(title("Untitled") == "Untitled", "missing title falls back to function name");
    Check(title("Dangling") == "MISSING - 键不存在", "unresolved category key still prefixes with id");

    const auto a = FindPreset(catalog, "FlagA");
    Check(a && a->type == "flag" && a->script == "FLAG_A" && a->display == "标志甲" && a->displayResolved, "plain preset");
    const auto b = FindPreset(catalog, "FlagB");
    Check(b && b->script == "FLAG_B" && b->display == "标志乙", "glued preset split into script and display");
    Check(FindPreset(catalog, "FlagC")->display == "标志丙", "WESTRING preset localized");
    const auto d = FindPreset(catalog, "FlagD");
    Check(d && !d->displayResolved && d->display == "FLAG_D", "unresolved preset shows script value");
    Check(!FindPreset(catalog, "Absent").has_value(), "unknown preset");
    Check(PresetsOfType(catalog, "flag").size() == 4 && PresetsOfType(catalog, "unit").empty(), "presets by type");

    const TriggerTypeSystem types(catalog);
    Check(types.DisplayName("unit") == "单位", "WESTRING type name localized");
    Check(types.DisplayName("flag") == "标志", "literal type name");
    Check(types.DisplayName("plain") == "plain", "unresolved type name falls back to id");
    Check(types.PresetType("FlagB") == std::optional<std::string>("flag"), "glued preset keeps type");
}

} // namespace

void TestTriggerDataText() {
    TestCsvSplitting();
    const auto root = CreateUi();
    auto loaded = UiCatalog::Load(root);
    Check(loaded.Succeeded(), "synthetic catalog loads");
    auto catalog = std::move(*loaded.value);
    // 没有字符串表时：键原样保留，分类退回 id。
    Check(TriggerCategoryTable(catalog).Find("TC_UNIT")->display == "UNIT", "no strings: id fallback");
    Check(FindPreset(catalog, "FlagC")->display == "FLAG_C", "no strings: preset shows script value");
    auto strings = std::make_shared<War3Map::Game::WorldEditStrings>();
    strings->AddText("test",
        "[WorldEditStrings]\r\nWESTRING_TRIGCAT_UNIT=单位\r\nWESTRING_TRIGCAT_LOGIC=逻辑\r\n"
        "WESTRING_TRIGTYPE_UNIT=单位\r\nWESTRING_FLAG_C=标志丙\r\n");
    catalog.SetStrings(strings);
    Check(catalog.Localize("WESTRING_TRIGCAT_LOGIC") == "逻辑" && catalog.Localize("原文") == "原文", "catalog localize");
    TestCatalogText(catalog);
    std::filesystem::remove_all(root);
}
