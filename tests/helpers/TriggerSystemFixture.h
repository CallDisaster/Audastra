#pragma once

#include "AudastraUiCatalog.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// 生成器（读条系统、团队框架）测试共用：合成函数库加 AudastraUI 包上的空触发器文档，
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

} // namespace AudastraUiTest
