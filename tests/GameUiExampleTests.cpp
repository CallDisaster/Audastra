#include "ActionBarLayer.h"
#include "CastSpellLayer.h"
#include "MeterLayer.h"
#include "PartyLayer.h"
#include "TalentLayer.h"
#include "TargetLayer.h"
#include "TriggerSystemFixture.h"
#include "War3MapCore/GameUi/UiDescription.h"
#include "War3MapCore/GameUi/UiTriggers.h"

#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

// 界面示例（examples/game-ui）：每个示例的描述能读入、能生成界面触发器，示例触发器文本能整批写进触发器文档，
// 同一生产输出器能生成整图脚本；设了 AUDASTRA_GAME_SCRIPTS 时再用游戏的真实声明检查。示例不随编辑器提供生成器，
// 这里防止示例跟函数库或描述格式脱节。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;

using AudastraUiTest::First;
using AudastraUiTest::Require;

struct Fixture : AudastraUiTest::SystemFixture {
    Fixture()
        : SystemFixture({AudastraUiTest::SpellLayer(), AudastraUiTest::PartyLayer(), AudastraUiTest::TalentLayer(), AudastraUiTest::TargetLayer(),
              AudastraUiTest::MeterLayer(), AudastraUiTest::YdweDamageLayer(), AudastraUiTest::ActionBarLayer(),
              AudastraUiTest::YdweAbilityStateLayer()}) {}
};

std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "读不到 " + path.string());
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void CheckExample(const std::filesystem::path& folder) {
    const std::string name = folder.filename().string();
    const auto description = ParseUiDescription(Read(folder / (name + ".json")), name);
    Require(description.Succeeded(), name + " 描述：" + First(description.diagnostics));
    Require(description.value->id == name, name + " 描述编号与目录同名");

    Fixture f;
    const auto ui = GenerateUiTriggers(*description.value);
    Require(ui.Succeeded(), name + " 界面触发器：" + First(ui.diagnostics));
    const auto uiApplied = f.Apply(kUiTriggerCategory, ui.value->featureText);
    Require(uiApplied.Succeeded(), name + " 界面触发器写入：" + First(uiApplied.diagnostics));
    const auto feature = Read(folder / "triggers.txt");
    // 检查完整的静态图标路径；颜色贴图会拼接编号与扩展名，不是完整路径字面量。
    for (std::size_t at = 0; (at = feature.find("\"ReplaceableTextures\\\\CommandButtons\\\\", at)) != std::string::npos;) {
        const auto end = feature.find('"', at + 1);
        Require(end != std::string::npos, name + " 贴图路径未结束");
        const std::string_view path(feature.data() + at + 1, end - at - 1);
        Require(path.ends_with(".blp") || path.ends_with(".tga"), name + " 框架贴图缺少扩展名：" + std::string(path));
        at = end + 1;
    }
    const auto applied = f.Apply("示例", feature);
    Require(applied.Succeeded(), name + " 示例触发器写入：" + First(applied.diagnostics));
    // 读条示例登记的效果触发器随示例一起导入，空白图直接导入不缺引用。
    if (name == "castbar")
        for (const auto* effect : {"效果 读条 Bolt", "效果 引导 Ritual"})
            Require(std::any_of(f.doc.triggers.begin(), f.doc.triggers.end(), [&](const auto& row) { return row.name.text == effect; }),
                std::string("读条示例缺少效果触发器 ") + effect);
    const std::string script = f.Script();
    // 读条示例用逆天局部变量，脚本依赖 YDWE 预处理宏，游戏声明检查不适用。
    if (name != "castbar" && AudastraUiTest::CheckWithGameScripts(f, script)) std::cout << name << " script checked against game scripts\n";
    std::cout << name << " example ok\n";
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        const std::filesystem::path root(AUDASTRA_UI_EXAMPLES_DIR);
        int count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory()) continue;
            CheckExample(entry.path());
            ++count;
        }
        Require(count == 6, "示例数量应为 6，实际 " + std::to_string(count));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "game ui example tests passed\n";
    return 0;
}
