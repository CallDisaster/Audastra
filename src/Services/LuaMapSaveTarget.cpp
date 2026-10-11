#include "LuaMapSaveTarget.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/ClassicLuaMapSources.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"

#include <algorithm>
#include <array>

namespace War3Map::Services::LuaMapSave {
namespace {

bool SameMember(std::string_view a, std::string_view b) {
    const auto fold = [](unsigned char c) {
        if (c == '/') c = '\\';
        return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
    };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [&](unsigned char x, unsigned char y) { return fold(x) == fold(y); });
}

constexpr std::array<std::string_view, 4> kScriptMembers{
    "war3map.j", "scripts\\war3map.j", "war3map.lua", "scripts\\war3map.lua"};

bool IsScript(std::string_view name) {
    return std::any_of(kScriptMembers.begin(), kScriptMembers.end(),
        [&](auto script) { return SameMember(name, script); });
}

constexpr auto& kClassicMembers = kClassicLuaGeneratedMembers;

bool ClassicOwned(std::string_view name) {
    return std::any_of(kClassicMembers.begin() + 1, kClassicMembers.end(),
        [&](auto member) { return SameMember(name, member); });
}

Status Fail(std::string code, std::string message) {
    return Status::Failure("LuaMap.Save", std::move(code), std::move(message));
}

} // namespace

Result<SettingsInput> ReadSettings(const MapSaveTask& task) {
    if (!task.archive) return Result<SettingsInput>::Failure("LuaMap.Save",
        "LUA_MAP_ARCHIVE_MISSING", "Lua目标没有冻结源归档。");
    auto source = task.archive->ReadMember("war3map.w3i");
    if (!source.Succeeded() || !source.value) return {{}, std::move(source.diagnostics)};
    const MemberReplacement* selected = nullptr;
    for (const auto& member : task.replacements) {
        if (!SameMember(member.name, "war3map.w3i")) continue;
        if (selected) return Result<SettingsInput>::Failure("LuaMap.Save",
            "LUA_MAP_SETTINGS_CONFLICT", "最终W3I有多个版本，无法冻结语言选择。");
        selected = &member;
    }
    if (std::any_of(task.removals.begin(), task.removals.end(),
        [](const auto& name) { return SameMember(name, "war3map.w3i"); }))
        return Result<SettingsInput>::Failure("LuaMap.Save", "LUA_MAP_SETTINGS_REMOVED", "不能删除Lua目标的W3I。");
    SettingsInput input;
    input.source = std::move(*source.value);
    input.final = selected ? selected->data : input.source;
    return Result<SettingsInput>::Success(std::move(input));
}

Status VerifyTarget(const MapSaveTask& task) {
    if (!task.script || !task.script->luaTarget || !task.script->luaTarget->Identity()
        || !task.script->luaTarget->Profile() || task.scriptSkipped)
        return Fail("LUA_MAP_TARGET_UNCAPTURED", "Lua保存没有可执行的冻结目标。");
    const auto& frozen = *task.script;
    const auto& identity = *frozen.luaTarget->Identity();
    const bool classic = frozen.luaTarget->Profile()->dialect == Script::LuaMapSaveDialect::Classic127YDWE;
    if (identity.documentRevision != frozen.documentRevision
        || identity.targetRequestSha256 != frozen.luaTarget->Profile()->requestSha256
        || frozen.scriptMember != (classic ? "war3map.j" : "war3map.lua"))
        return Fail("LUA_MAP_TARGET_STALE", "Lua目标档案、文档修订或脚本成员与保存任务不一致。");
    const auto input = ReadSettings(task);
    if (!input.Succeeded() || !input.value) return {input.diagnostics};
    const auto source = Sha256(input.value->source), final = Sha256(input.value->final);
    const auto selected = Sha256(frozen.luaTarget->SelectedSettings());
    if (!source.Succeeded() || !final.Succeeded() || !selected.Succeeded()
        || !source.value || !final.value || !selected.value
        || *source.value != identity.sourceSettingsSha256 || *final.value != identity.inputSettingsSha256
        || *selected.value != identity.selectedSettingsSha256)
        return Fail("LUA_MAP_SETTINGS_STALE", "源、最终输入或选中W3I已改变；须重新冻结Lua目标。");
    if (std::any_of(task.replacements.begin(), task.replacements.end(),
        [&](const auto& member) { return IsScript(member.name) || (classic && ClassicOwned(member.name)); })
        || std::any_of(task.removals.begin(), task.removals.end(), [&](const auto& name) {
            return IsScript(name) || (classic && ClassicOwned(name)); }))
        return Fail("LUA_MAP_SCRIPT_CONFLICT", "Lua目标由同一重建任务提交唯一脚本，不能混入另一路脚本替换或删除。");
    return classic ? VerifyClassicLuaGeneratedSources(*task.archive, task.sourceSha256,
        ClassicLuaSourcePolicy::AllowAbsentForFirstSave) : Status::Ok();
}

Status MergeCompletedScript(MapSaveTask& task, std::vector<MemberReplacement>& scripts) {
    const auto verified = VerifyTarget(task);
    if (!verified.Succeeded()) return verified;
    const bool classic = task.script->luaTarget->Profile()->dialect == Script::LuaMapSaveDialect::Classic127YDWE;
    if (classic) {
        if (scripts.size() != kClassicMembers.size())
            return Fail("CLASSIC_LUA_SCRIPT_MEMBER", "经典Lua保存缺少完整入口、模块或清单。");
        for (std::size_t index = 0; index < scripts.size(); ++index)
            if (scripts[index].name != kClassicMembers[index])
                return Fail("CLASSIC_LUA_SCRIPT_MEMBER", "经典Lua生成成员名称或顺序不一致。");
    } else if (scripts.size() != 1 || scripts.front().name != "war3map.lua")
        return Fail("LUA_MAP_SCRIPT_MEMBER", "完整Lua保存只接受唯一根war3map.lua。");
    const std::vector<std::string> oldNames = classic
        ? std::vector<std::string>{"scripts\\war3map.j", "war3map.lua", "scripts\\war3map.lua"}
        : std::vector<std::string>{"war3map.j", "scripts\\war3map.j", "scripts\\war3map.lua"};
    // 精确查询也覆盖没有可读listfile的源归档；只取得存在性和身份，不猜不存在。
    const auto old = task.archive->DescribeExactMemberIdentities(task.sourceSha256, oldNames);
    if (!old.Succeeded() || !old.value) return {old.diagnostics};
    if (classic && std::any_of(old.value->members.begin() + 1, old.value->members.end(), [](const auto& m) { return m.present; }))
        return Fail("CLASSIC_LUA_SCRIPT_CONFLICT", "经典源含独立Lua入口，拒绝静默删除或替换。");
    auto removals = task.removals;
    std::vector<std::string> generatedScripts;
    generatedScripts.reserve(scripts.size()+old.value->members.size());
    for (const auto& member:scripts) generatedScripts.push_back(member.name);
    for (const auto& member : old.value->members)
        if (member.present) { removals.push_back(member.name); generatedScripts.push_back(member.name); }
    const auto selected = task.script->luaTarget->SelectedSettings();
    MemberReplacement settings{"war3map.w3i", {selected.begin(), selected.end()}};
    const auto existing = std::find_if(task.replacements.begin(), task.replacements.end(),
        [](const auto& member) { return SameMember(member.name, "war3map.w3i"); });
    const auto slot = static_cast<std::size_t>(existing - task.replacements.begin());
    const bool appendSettings = existing == task.replacements.end();
    task.replacements.reserve(task.replacements.size() + scripts.size() + 1);
    const auto merged = classic ? Script::MergeClassicLuaSaveMembers(task.replacements, removals, scripts)
        : Script::MergeScriptSaveMembers(task.replacements, removals, scripts);
    if (!merged.Succeeded()) return merged;
    // 分配、来源检查和合并冲突均已完成；以下只移动owned成员，不会留下半份语言选择。
    static_assert(std::is_nothrow_move_assignable_v<MemberReplacement>);
    if (appendSettings) task.replacements.push_back(std::move(settings));
    else task.replacements[slot] = std::move(settings);
    task.removals.swap(removals);
    task.generatedScriptMembers.swap(generatedScripts);
    return Status::Ok();
}

} // namespace War3Map::Services::LuaMapSave
