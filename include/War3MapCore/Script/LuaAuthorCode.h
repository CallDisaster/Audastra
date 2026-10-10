#pragma once

#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"

namespace War3Map::Script {

struct LuaAuthorCodeIdentity {
    std::string sourceSettingsSha256;
    std::string sourceWtgSha256, sourceWctSha256, catalogSha256;
    std::string currentWtgSha256, currentWctSha256, graphSha256;
};

struct LuaAuthorTriggerCode {
    StableId triggerId = 0;
    std::string text, variable, initializer;
};

// 只有实际解析的原生 Lua W3I 和可信 WTG/WCT 文档能建立此凭据。
// 持有者不能改写正文、源语言或图身份；空对象不能放行自定义代码。
class PreparedLuaAuthorCode {
public:
    [[nodiscard]] const LuaAuthorCodeIdentity* Identity() const noexcept;
    [[nodiscard]] std::string_view Header() const noexcept;
    [[nodiscard]] std::span<const LuaAuthorTriggerCode> Triggers() const noexcept;
    [[nodiscard]] Status Verify(const Trigger::TriggerDocument& document,
        const Trigger::UiCatalog& catalog) const;
private:
    struct State;
    std::shared_ptr<const State> state_;
    friend Result<PreparedLuaAuthorCode> CaptureNativeLuaAuthorCode(std::span<const std::byte>,
        const Trigger::TriggerDocument&, const Trigger::UiCatalog&, const Lua::SyntaxOptions&);
};

// 保存前捕获：只编译活动 Lua 作者块，不执行；禁用正文仍纳入身份并保留在 WCT。
// initializer 采用原版 InitTrig_<WE ScriptName> 合同，不按 StableId 猜作者函数名。
[[nodiscard]] Result<PreparedLuaAuthorCode> CaptureNativeLuaAuthorCode(
    std::span<const std::byte> sourceSettings, const Trigger::TriggerDocument& document,
    const Trigger::UiCatalog& catalog, const Lua::SyntaxOptions& options = {});

// 编辑资格仍来自实际源 W3I；允许暂存不完整代码，语法门禁由实时检查和保存承担。
// 计划、Apply、Replay、撤销均复用 TriggerCodeEditor，没有第二套变更路径。
class LuaAuthorCodeEditor {
public:
    [[nodiscard]] static Result<Trigger::TriggerCodeEditPlan> PrepareText(
        std::span<const std::byte> sourceSettings, const Trigger::TriggerDocument& document,
        const Trigger::UiCatalog& catalog, Trigger::CodeBlockTarget target,
        std::string_view normalizedUtf8,
        std::size_t maxEditedBytes = Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes);
    [[nodiscard]] static Result<Trigger::TriggerCodeEditPlan> PrepareNewCustom(
        std::span<const std::byte> sourceSettings, const Trigger::TriggerDocument& document,
        const Trigger::UiCatalog& catalog, std::int32_t categoryId, std::string name,
        std::string_view normalizedUtf8 = {},
        std::size_t maxEditedBytes = Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes,
        std::string newCategoryName = {});
};

} // namespace War3Map::Script
