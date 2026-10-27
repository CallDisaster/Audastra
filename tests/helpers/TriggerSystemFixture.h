#pragma once

#include "AudastraUiCatalog.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// 界面示例测试用：合成函数库加 AudastraUI 包上的空触发器文档，
// 按功能写法整批写进类别、按名字导出触发器文本，并用生产输出器生成整图脚本。
namespace AudastraUiTest {

inline void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
inline std::string First(const std::vector<War3Map::Diagnostic>& diagnostics) {
    return diagnostics.empty() ? std::string("（无诊断）") : diagnostics.front().code + " " + diagnostics.front().message;
}
inline bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

struct SystemFixture {
    Catalog files;
    War3Map::Trigger::UiCatalog catalog;
    War3Map::Trigger::TriggerDocument doc;

    explicit SystemFixture(std::vector<Catalog::Layer> layers) {
        files.extras = std::move(layers);
        const auto error = files.Load();
        Require(error.empty(), error);
        catalog = std::move(*files.catalog);
        doc = std::move(*files.document);
    }

    War3Map::Result<War3Map::Trigger::TriggerGraphEditReceipt> Apply(std::string_view category, const std::string& text) {
        using namespace War3Map::Trigger;
        const auto binding = CaptureTriggerGraphBinding(doc, catalog);
        Require(binding.Succeeded(), "capture binding");
        TriggerGraphEditBatch batch{*binding.value, {}};
        batch.operations.emplace_back(GraphApplyFeatureText{std::string(category), text, {}, std::nullopt});
        return ApplyTriggerGraphEditBatch(doc, catalog, batch);
    }

    [[nodiscard]] const War3Map::Trigger::TriggerRecord* Trigger(std::string_view name) const {
        const auto found = std::find_if(doc.triggers.begin(), doc.triggers.end(), [&](const auto& row) { return row.name.text == name; });
        return found == doc.triggers.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::string Text(std::string_view name) const {
        const auto* trigger = Trigger(name);
        Require(trigger != nullptr, "没有触发器 " + std::string(name));
        const auto text = War3Map::Trigger::RenderTriggerText(doc, catalog, trigger->id, 1u << 20, nullptr, nullptr);
        Require(text.Succeeded() && text.value, "导出 " + std::string(name) + "：" + First(text.diagnostics));
        return *text.value;
    }

    [[nodiscard]] std::string Script() const {
        War3Map::Script::MapScriptInputs inputs;
        inputs.triggers = &doc;
        inputs.catalog = &catalog;
        const auto generated = War3Map::Script::GenerateMapScript(inputs);
        Require(generated.Succeeded(), "整图脚本生成：" + First(generated.diagnostics));
        return generated.text;
    }
};

// 设了环境变量 AUDASTRA_GAME_SCRIPTS（放游戏 common.j 与 Blizzard.j 的目录）时，用游戏的真实声明检查生成的整图脚本；
// AudastraUI 的函数按函数库定义写成桩（只核对名字、参数个数与类型），不依赖它的实现。没设时返回 false（跳过），
// 检查不通过时抛出。游戏文件不随仓库提供。
inline bool CheckWithGameScripts(const SystemFixture& f, const std::string& script) {
    wchar_t buffer[1024] = {};
    const DWORD length = GetEnvironmentVariableW(L"AUDASTRA_GAME_SCRIPTS", buffer, 1024);
    if (length == 0 || length >= 1024) return false;
    const std::filesystem::path directory(buffer);
    const auto read = [&](const char* name) {
        std::ifstream stream(directory / name, std::ios::binary);
        Require(stream.good(), std::string("读不到 ") + name);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    };
    const std::string common = read("common.j");
    const std::string blizzard = read("Blizzard.j");

    using War3Map::Trigger::UiDefinitionKind;
    // 签名里的触发器类型换成 JASS 类型：Audastra 句柄为整数，其余按 TriggerTypes 第五列的基础类型（abilcode → integer）。
    const auto jassType = [&f](const std::string& type) {
        if (type.starts_with("Audastra")) return std::string("integer");
        const auto* definition = f.catalog.Find(UiDefinitionKind::Define, type, "TriggerTypes");
        if (definition == nullptr || definition->revisions.empty()) return type;
        for (const auto& [key, value] : definition->Active().fields) {
            if (key != "value") continue;
            std::size_t start = 0;
            for (int field = 0; field < 4 && start != std::string::npos; ++field) {
                start = value.find(',', start);
                if (start != std::string::npos) ++start;
            }
            if (start == std::string::npos) return type;
            const std::string base = value.substr(start, value.find(',', start) - start);
            return base.empty() ? type : base;
        }
        return type;
    };
    const auto fallback = [](const std::string& type) -> std::string {
        if (type == "integer") return "0";
        if (type == "real") return "0.0";
        if (type == "boolean") return "false";
        if (type == "string") return "\"\"";
        return "null";
    };
    std::string stubs;
    for (const auto kind : {UiDefinitionKind::Event, UiDefinitionKind::Action, UiDefinitionKind::Call}) {
        for (const auto* definition : f.catalog.Definitions(kind)) {
            const auto& active = definition->Active();
            // 函数库注入的函数（AudastraUI、合成层里名字以 ydwe 开头的 YDWE 函数）不在游戏脚本里，写成桩。
            if (active.layer != "AudastraUI" && !active.layer.starts_with("ydwe")) continue;
            std::string parameters;
            std::size_t index = 0;
            if (kind == UiDefinitionKind::Event) parameters = "trigger p" + std::to_string(index++);
            for (const auto& argument : active.arguments) {
                if (!parameters.empty()) parameters += ", ";
                parameters += jassType(argument.type) + " p" + std::to_string(index++);
            }
            std::string returns = "nothing";
            for (const auto& [key, value] : active.fields) {
                if (key == "returns") returns = jassType(value);
            }
            stubs += "function " + definition->name + " takes " + (parameters.empty() ? "nothing" : parameters) + " returns " + returns + "\n";
            if (returns != "nothing") stubs += "    return " + fallback(returns) + "\n";
            stubs += "endfunction\n";
        }
    }
    // 生成的脚本带 YDWE 预处理行（#define、#include）；这里换成空行（行号不变），所以只适用于不用预处理宏的脚本。
    std::string plain;
    for (std::size_t start = 0; start < script.size();) {
        const std::size_t end = std::min(script.find('\n', start), script.size());
        const std::string_view line(script.data() + start, end - start);
        if (!line.starts_with("#")) plain += line;
        plain += '\n';
        start = end + 1;
    }
    const std::array<War3Map::Jass::JassSource, 4> sources{War3Map::Jass::JassSource{"common.j", common},
        War3Map::Jass::JassSource{"Blizzard.j", blizzard}, War3Map::Jass::JassSource{"AudastraUI.j", stubs},
        War3Map::Jass::JassSource{"war3map.j", plain}};
    const auto checked = War3Map::Jass::CheckJass(sources);
    Require(checked.Succeeded() && checked.completed && checked.semanticChecked, "游戏声明下的 JASS 检查：" + First(checked.diagnostics));
    return true;
}

} // namespace AudastraUiTest
