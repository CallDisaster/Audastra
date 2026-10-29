#include "MapRuntimeSave.h"
#include "MapRuntimeJson.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/MapRuntimeBoot.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace War3Map::Services {
namespace {
using Json = nlohmann::json;
Status Fail(std::string code, std::string message) { return Status::Failure("MapRuntime", std::move(code), std::move(message)); }
std::string Key(std::string_view name) {
    auto key = Resources::CanonicalImportPath(name);
    return key.Succeeded() ? *key.value : std::string{};
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data()); return {first, first + text.size()};
}
std::string_view Text(std::span<const std::byte> bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
using Member = Result<std::optional<std::vector<std::byte>>>;
std::size_t MemberBudget(std::string_view name) {
    if (Key(name) == Key(kMapRuntimeSettingsMember)) return 4096;
    if (Key(name) == Key(kMapRuntimeInstallationMember)) return 32 * 1024;
    return 64 * 1024 * 1024;
}
Member SourceMember(const MapSaveTask& task, std::string_view name) {
    const auto budget = MemberBudget(name);
    if (task.archive->HasMember(name)) {
        auto read = task.archive->ReadMember(name);
        if (!read.Succeeded()) { Member result; result.diagnostics = std::move(read.diagnostics); return result; }
        if (read.value->size() > budget)
            return Member::Failure("MapRuntime", Key(name) == Key(kMapRuntimeInstallationMember)
                ? "RUNTIME_INSTALLATION_INVALID" : "RUNTIME_MEMBER_BUDGET", "源成员超过运行库核验预算：" + std::string(name));
        return Member::Success(std::move(*read.value));
    }
    if (!task.archive->HasOpaqueMembers()) return Member::Success(std::nullopt);
    ArchiveExactMemberLimits limits;
    limits.memberCount = 1; limits.memberBytes = budget; limits.totalBytes = budget;
    auto exact = task.archive->CaptureExactMembers(std::vector<std::string>{std::string(name)}, limits);
    if (!exact.Succeeded()) { Member result; result.diagnostics = std::move(exact.diagnostics); return result; }
    auto& member = exact.value->front();
    return member.present ? Member::Success(std::move(member.bytes)) : Member::Success(std::nullopt);
}
Member FinalMember(const MapSaveTask& task, std::string_view name) {
    const auto key = Key(name); const MemberReplacement* found = nullptr; bool removed = false;
    for (const auto& item : task.replacements) if (Key(item.name) == key) {
        if (found) return Member::Failure("MapRuntime", "RUNTIME_MEMBER_CONFLICT", "最终成员重复：" + std::string(name));
        found = &item;
    }
    for (const auto& item : task.removals) if (Key(item) == key) removed = true;
    if (found && removed) return Member::Failure("MapRuntime", "RUNTIME_MEMBER_CONFLICT", "成员同时替换和删除：" + std::string(name));
    if (removed) return Member::Success(std::nullopt);
    if (found) {
        if (found->data.size() > MemberBudget(name))
            return Member::Failure("MapRuntime", "RUNTIME_MEMBER_BUDGET", "最终成员超过运行库核验预算：" + std::string(name));
        return Member::Success(found->data);
    }
    return SourceMember(task, name);
}
Status Set(MapSaveTask& task, MemberReplacement member) {
    const auto key = Key(member.name);
    for (const auto& item : task.removals) if (Key(item) == key) return Fail("RUNTIME_MEMBER_CONFLICT", "受管成员被其他保存域删除：" + member.name);
    for (const auto& item : task.replacements) if (Key(item.name) == key) {
        if (item.data == member.data) return Status::Ok();
        return Fail("RUNTIME_MEMBER_CONFLICT", "受管成员被模板或其他保存域覆盖：" + member.name);
    }
    task.replacements.push_back(std::move(member)); return Status::Ok();
}
Result<std::shared_ptr<const MapRuntimePackage>> Package(const MapSaveTask& task, std::string_view identity,
    std::optional<MapRuntimeSavePurpose> purpose = {}) {
    if (!task.runtimeCatalogDiagnostics.empty()) return {{}, task.runtimeCatalogDiagnostics};
    const MapRuntimePackageSource* selected = nullptr;
    for (const auto& source : task.runtimePackageSources) if (source.manifestSha256 == identity) {
        if (selected) return Result<std::shared_ptr<const MapRuntimePackage>>::Failure("MapRuntime", "RUNTIME_PACKAGE_CONFLICT", "可信包目录存在重复内容身份。");
        selected = &source;
    }
    if (!selected) return Result<std::shared_ptr<const MapRuntimePackage>>::Failure("MapRuntime", "RUNTIME_PACKAGE_MISSING", "本机可信目录缺少地图锁定的运行库包；未改用最新版。");
    return MapRuntimePackage::Capture(*selected, purpose.value_or(task.runtimePurpose));
}
}

Status PrepareMapRuntimeSave(MapSaveTask& task) {
    if (task.runtimePurpose != MapRuntimeSavePurpose::Normal && task.runtimePurpose != MapRuntimeSavePurpose::InternalValidation
        && task.runtimePurpose != MapRuntimeSavePurpose::TestRun)
        return Fail("RUNTIME_SAVE_PURPOSE_INVALID", "未知保存用途不能授予内部包使用资格。");
    auto state = std::make_shared<MapRuntimeSaveState>();
    state->purpose = task.runtimePurpose;
    state->approvedPackageSha256 = task.runtimeApprovedPackageSha256;
    auto settings = FinalMember(task, kMapRuntimeSettingsMember);
    if (!settings.Succeeded()) return {settings.diagnostics};
    if (*settings.value) {
        const auto parsed = ParseMapRuntimeSettings(**settings.value);
        if (!parsed.Succeeded()) return {parsed.diagnostics};
        state->settings = *parsed.value;
    }
    state->settingsBytes = *settings.value;
    state->scriptMember = task.script ? task.script->scriptMember : Script::MapScriptMemberName(*task.archive);
    // 安装所有权只从冻结源核对；保存输入不得伪造或重写安装回执。
    for (const auto& item : task.replacements) if (Key(item.name) == Key(kMapRuntimeInstallationMember))
        return Fail("RUNTIME_MEMBER_CONFLICT", "安装回执由保存服务管理，不能从资源或模板替换。");
    for (const auto& item : task.removals) if (Key(item) == Key(kMapRuntimeInstallationMember))
        return Fail("RUNTIME_MEMBER_CONFLICT", "不能从资源队列删除运行库安装回执。");
    const auto installed = SourceMember(task, kMapRuntimeInstallationMember);
    if (!installed.Succeeded()) return {installed.diagnostics};
    if (*installed.value) {
        try {
            const auto json = detail::ParseRuntimeJson(**installed.value, 32 * 1024);
            const bool module = json.is_object() && json.contains("schema") && json.at("schema") == 2;
            if (!json.is_object() || json.size() != (module ? 7 : 5) || !json.at("schema").is_number_unsigned()
                || json.at("schema") != (module ? 2 : 1) || !json.at("bootRule").is_number_unsigned()
                || json.at("bootRule") != (module ? 2 : 1))
                throw std::runtime_error("schema");
            if (module) {
                const auto loaders = task.script && task.script->toolchain.frameworkInputs
                    ? task.script->toolchain.frameworkInputs->profile.moduleLoaders : Script::DefaultScriptModuleLoaders();
                for (const auto& loader : loaders) {
                    const auto hash = Script::ScriptModuleLoaderIdentity(loader);
                    if (hash.Succeeded() && json.at("moduleLoaderId") == loader.id && json.at("moduleLoaderSha256") == *hash.value) {
                        if (state->moduleLoader) throw std::runtime_error("duplicate loader");
                        state->moduleLoader = loader;
                    }
                }
                if (!state->moduleLoader) throw std::runtime_error("loader unavailable");
            }
            state->previousIdentity = json.at("packageSha256").get<std::string>();
            state->previousScriptSha256 = json.at("scriptSha256").get<std::string>();
            if (state->previousScriptSha256.size() != 64
                || state->previousScriptSha256.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("script identity");
            // 只读验证旧所有权不等于把旧内部包再分发；关闭后普通保存可以移除它。
            const auto package = Package(task, state->previousIdentity, MapRuntimeSavePurpose::InternalValidation);
            if (!package.Succeeded() || !package.value) return {package.diagnostics};
            const auto& files = json.at("files");
            if (module && (*package.value)->ModulePayloadSha256().empty()) throw std::runtime_error("module payload missing");
            if (!files.is_array() || files.size() != (*package.value)->Members().size()) throw std::runtime_error("files");
            std::set<std::string> seen;
            for (const auto& file : files) {
                if (!file.is_object() || file.size() != 3) throw std::runtime_error("file");
                const auto name = file.at("name").get<std::string>();
                if (!seen.insert(Key(name)).second) throw std::runtime_error("duplicate");
                const auto expected = std::find_if((*package.value)->Members().begin(), (*package.value)->Members().end(), [&](const auto& member) { return member.name == name; });
                if (expected == (*package.value)->Members().end()) throw std::runtime_error("member");
                const auto hash = Sha256(expected->data);
                if (!hash.Succeeded() || file.at("sha256") != *hash.value || !file.at("size").is_number_unsigned()
                    || file.at("size") != expected->data.size()) throw std::runtime_error("identity");
                auto actual = SourceMember(task, name);
                if (!actual.Succeeded() || !*actual.value || **actual.value != expected->data)
                    return Fail("RUNTIME_MEMBER_CONFLICT", "已安装运行库成员被修改或缺失：" + name);
                state->previousMembers.push_back(*expected);
            }
        } catch (const std::exception&) { return Fail("RUNTIME_INSTALLATION_INVALID", "运行库安装回执与可信包不符，未接管其成员。"); }
    }
    const bool approved = !state->settings.packageSha256.empty() && state->settings.mode != MapRuntimeMode::Disabled;
    if (task.script && task.script->luaTarget) {
        const auto sourceName = Script::MapScriptMemberName(*task.archive);
        const auto source = FinalMember(task, sourceName);
        if (!source.Succeeded()) return {source.diagnostics};
        if (*source.value) {
            const auto usage = Script::AnalyzeMapRuntimeUsage(Text(**source.value), sourceName.ends_with(".lua")
                ? Script::MapRuntimeScriptLanguage::Lua : Script::MapRuntimeScriptLanguage::Jass);
            if (!usage.Succeeded()) return {usage.diagnostics};
            if (usage.value->required) return Fail("RUNTIME_LUA_UNSUPPORTED", "原主脚本有本运行库依赖，不能通过转换Lua丢掉启动。");
        }
        if (!state->previousIdentity.empty() || state->settings.mode == MapRuntimeMode::Enabled)
            return Fail("RUNTIME_LUA_UNSUPPORTED", "原生界面运行库及AI引导仅支持经典1.27a JASS，不能通过删除启动转换Lua。");
    }
    if (task.script && !task.script->luaTarget && (approved || !state->previousIdentity.empty())) {
        auto& boot = task.script->toolchain.mapRuntimeBoot.emplace();
        boot.testRunDiagnostics = task.runtimePurpose == MapRuntimeSavePurpose::TestRun;
        boot.enabled = approved; boot.ownsExisting = !state->previousIdentity.empty();
        boot.ownedModuleLoader = state->moduleLoader;
        boot.onlyIfUsed = approved && state->settings.mode == MapRuntimeMode::Automatic;
        boot.resolvePayloadIdentity = [state, sources = task.runtimePackageSources, purpose = task.runtimePurpose]() -> Result<std::string> {
            if (state->approvedPackageSha256 != state->settings.packageSha256)
                return Result<std::string>::Failure("MapRuntime", "RUNTIME_CONSENT_REQUIRED", "地图记录了包身份，但本工程尚无对应的明确启用批准；未从陌生地图继承授权。");
            const MapRuntimePackageSource* selected = nullptr;
            for (const auto& source : sources) if (source.manifestSha256 == state->settings.packageSha256) {
                if (selected) return Result<std::string>::Failure("MapRuntime", "RUNTIME_PACKAGE_CONFLICT", "可信目录中重复包身份。");
                selected = &source;
            }
            if (!selected) return Result<std::string>::Failure("MapRuntime", "RUNTIME_PACKAGE_MISSING", "本机缺少已锁定运行库包。");
            auto captured = MapRuntimePackage::Capture(*selected, purpose);
            if (!captured.Succeeded()) { Result<std::string> failed; failed.diagnostics = captured.diagnostics; return failed; }
            state->package = *captured.value;
            state->moduleLoader.reset();
            return Result<std::string>::Success(state->package->PayloadSha256());
        };
        boot.resolveModulePayloadIdentity = [state, capture = boot.resolvePayloadIdentity](const Script::ScriptProfileModuleLoader& loader) -> Result<std::string> {
            auto captured = capture();
            if (!captured.Succeeded()) return captured;
            if (state->package->ModulePayloadSha256().empty())
                return Result<std::string>::Failure("MapRuntime", "RUNTIME_MODULE_PAYLOAD_MISSING", "当前锁定包不含模块启动载荷；请明确选择配套的新包，未自动升级。");
            state->moduleLoader = loader;
            return Result<std::string>::Success(state->package->ModulePayloadSha256());
        };
        boot.verifyScriptInputs = [state](std::span<const std::string> hashes) {
            if (!state->package) return Fail("RUNTIME_PACKAGE_MISSING", "脚本输入校验缺少已捕获包。");
            return state->package->VerifyScriptInputs(hashes);
        };
        // 已安装地图的关闭/更新都要重建；不能被WTG/WCT未变的旧脚本分支吞掉。
        if (!state->previousIdentity.empty() || state->settings.mode == MapRuntimeMode::Enabled) task.preserveUnaffectedScript = false;
    }
    task.runtimeState = std::move(state);
    return Status::Ok();
}

Status FinalizeMapRuntimeSave(MapSaveTask& task) {
    auto& state = *task.runtimeState;
    const auto script = FinalMember(task, state.scriptMember);
    if (!script.Succeeded()) return {script.diagnostics};
    const bool lua = state.scriptMember.ends_with(".lua");
    auto used = *script.value ? Script::AnalyzeMapRuntimeUsage(Text(**script.value), lua
        ? Script::MapRuntimeScriptLanguage::Lua : Script::MapRuntimeScriptLanguage::Jass) : Result<Script::MapRuntimeUsage>::Success({});
    if (!used.Succeeded()) return {used.diagnostics};
    // 经典Lua的主入口仍为JASS；其实际生成的Lua成员也必须经过同一依赖判定。
    if (task.script && task.script->luaTarget) for (const auto& member : task.replacements) {
        if (!member.name.ends_with(".lua") || Key(member.name) == Key(state.scriptMember)) continue;
        const auto usage = Script::AnalyzeMapRuntimeUsage(Text(member.data), Script::MapRuntimeScriptLanguage::Lua);
        if (!usage.Succeeded()) return {usage.diagnostics};
        used.value->required = used.value->required || usage.value->required;
        used.value->unresolvedDynamicCalls = used.value->unresolvedDynamicCalls || usage.value->unresolvedDynamicCalls;
    }
    const bool required = used.value->required || state.settings.mode == MapRuntimeMode::Enabled;
    if ((lua || (task.script && task.script->luaTarget)) && (required || !state.previousIdentity.empty()))
        return Fail("RUNTIME_LUA_UNSUPPORTED", "本图需要本原生运行库，当前仅支持经典1.27a JASS；普通无依赖Lua地图不受此限制。");
    if (!required && !state.previousIdentity.empty() && used.value->unresolvedDynamicCalls)
        return Fail("RUNTIME_USAGE_UNCERTAIN", "主脚本仍有动态执行入口，不能凭静态未检出移除已有运行库；请先明确依赖。");
    // 只对写了运行库设置的地图提示：没有设置的普通地图本来就不会带运行库，动态执行入口又很常见。
    if (!required && used.value->unresolvedDynamicCalls && state.settingsBytes)
        task.diagnostics.push_back({DiagnosticSeverity::Warning,"MapRuntime","RUNTIME_USAGE_PARTIAL",
            "运行库静态检测未发现确定依赖，但有动态执行入口；未自动加入运行库，需要时请明确启用。"});
    if (required && state.settings.mode == MapRuntimeMode::Disabled) return Fail("RUNTIME_DISABLED_WITH_DEPENDENCIES", "地图仍调用界面/JAPI函数；请调整依赖或重新启用运行库。");
    if (required && (state.settings.packageSha256.empty() || state.approvedPackageSha256 != state.settings.packageSha256))
        return Fail("RUNTIME_CONSENT_REQUIRED", "本图需要原生运行库，请在生成触发器时明确启用并锁定可信包；地图内声明不作为本工程的批准，保存未弹重复确认。");
    if (required && state.previousIdentity.empty() && !task.scriptRebuilt)
        return Fail("RUNTIME_SCRIPT_REBUILD_REQUIRED", "首次安装必须经过共用脚本构建和类型检查，不能接管已有手工启动。");
    state.active = required;
    if (!required && state.previousIdentity.empty()) return Status::Ok();
    if (!*script.value || state.scriptMember.ends_with(".lua")) return Fail("RUNTIME_LUA_UNSUPPORTED", "运行库需要完整经典1.27a JASS主脚本。");
    const auto boot = Script::VerifyMapRuntimeBoot(Text(**script.value), required, state.moduleLoader);
    if (!boot.Succeeded()) return boot;
    const auto diagnostic = Script::VerifyMapRuntimeTestDiagnostics(Text(**script.value), required && state.purpose == MapRuntimeSavePurpose::TestRun);
    if (!diagnostic.Succeeded()) return diagnostic;
    const auto scriptHash = Sha256(**script.value);
    if (!scriptHash.Succeeded()) return {scriptHash.diagnostics};
    state.scriptSha256 = *scriptHash.value;
    auto tableBytes = FinalMember(task, "war3map.imp");
    if (!tableBytes.Succeeded()) return {tableBytes.diagnostics};
    auto parsed = *tableBytes.value ? Resources::ImportTable::Parse(**tableBytes.value)
        : Result<Resources::ImportTable>::Success(Resources::ImportTable::CreateEmpty());
    if (!parsed.Succeeded()) return {parsed.diagnostics};
    auto& table = *parsed.value;
    Status existenceStatus;
    const auto exists = [&](std::string_view member) {
        if (std::any_of(state.previousMembers.begin(), state.previousMembers.end(), [&](const auto& m) { return Key(m.name) == Key(member); })) return true;
        if (state.package && std::any_of(state.package->Members().begin(), state.package->Members().end(), [&](const auto& m) { return Key(m.name) == Key(member); })) return true;
        const auto item = FinalMember(task, member);
        if (!item.Succeeded()) { existenceStatus.diagnostics = item.diagnostics; return false; }
        return item.value->has_value();
    };
    if (state.settingsBytes) {
        const auto registered = table.Upsert(std::string(kMapRuntimeSettingsMember), exists);
        if (!registered.Succeeded()) return registered;
        if (!existenceStatus.Succeeded()) return existenceStatus;
    }
    if (required) {
        auto package = Package(task, state.settings.packageSha256);
        if (!package.Succeeded()) return {package.diagnostics};
        state.package = std::move(*package.value);
        if (state.package->InternalOnly() && (!task.testCopyOnly || task.inPlace
            || (!task.target.empty() && task.target.lexically_normal() == task.sourcePath.lexically_normal())))
            return Fail("RUNTIME_INTERNAL_COPY_REQUIRED", "内部黑盒包只能写入宿主明确标记的独立测试/验收副本，不能写回作者地图。");
        const auto identity = Script::VerifyMapRuntimeIdentity(Text(**script.value), state.moduleLoader
            ? state.package->ModulePayloadSha256() : state.package->PayloadSha256());
        if (!identity.Succeeded()) return identity;
        if (task.scriptRebuilt) {
            const auto inputs = state.package->VerifyScriptInputs(task.scriptInjectedSourceSha256);
            if (!inputs.Succeeded()) return inputs;
        } else if (state.previousIdentity != state.package->Identity() || state.previousScriptSha256 != state.scriptSha256) {
            return Fail("RUNTIME_SCRIPT_REBUILD_REQUIRED", "运行库包或主脚本已变化，必须使用该包配套客户端经过共用脚本构建，不能复用旧注入身份。");
        }
        for (const auto& member : state.package->Members()) {
            const bool owned = std::any_of(state.previousMembers.begin(), state.previousMembers.end(), [&](const auto& m) { return Key(m.name) == Key(member.name); });
            if (!owned) {
                const auto source = SourceMember(task, member.name);
                if (!source.Succeeded()) return {source.diagnostics};
                const bool queued = std::any_of(task.replacements.begin(), task.replacements.end(), [&](const auto& item) { return Key(item.name) == Key(member.name); });
                if (*source.value || queued)
                    return Fail("RUNTIME_MEMBER_CONFLICT", "同名源成员或待导入成员不是本工具拥有，即使内容相同也拒绝接管：" + member.name);
                for (const auto& record : table.Entries()) {
                    const auto direct = Key(record.path);
                    const auto prefixed = Key("war3mapImported/" + record.path);
                    if (direct.empty() || prefixed.empty()) return Fail("RUNTIME_MEMBER_CONFLICT", "导入表含不安全路径，不能核实运行库成员所有权。");
                    if (direct == Key(member.name) || ((record.flags != 10 && record.flags != 13) && prefixed == Key(member.name)))
                        return Fail("RUNTIME_MEMBER_CONFLICT", "已有导入记录不是本工具拥有，拒绝接管：" + member.name);
                }
            }
            const auto installed = Set(task, member); if (!installed.Succeeded()) return installed;
            const auto added = table.Upsert(member.name, exists); if (!added.Succeeded()) return added;
            if (!existenceStatus.Succeeded()) return existenceStatus;
            state.expectedMembers.push_back(member);
        }
    }
    for (const auto& old : state.previousMembers) {
        if (required && std::any_of(state.package->Members().begin(), state.package->Members().end(), [&](const auto& m) { return Key(m.name) == Key(old.name); })) continue;
        for (const auto& item : task.replacements) if (Key(item.name) == Key(old.name)) return Fail("RUNTIME_MEMBER_CONFLICT", "待卸载成员同时被其他保存域修改：" + old.name);
        if (std::find(task.removals.begin(), task.removals.end(), old.name) == task.removals.end()) task.removals.push_back(old.name);
        state.removedMembers.push_back(old.name);
        const auto removed = table.Remove(old.name, exists); if (!removed.Succeeded()) return removed;
        if (!existenceStatus.Succeeded()) return existenceStatus;
    }
    if (required) {
        Json metadata{{"schema",1},{"bootRule",1},{"packageSha256",state.package->Identity()},
            {"scriptSha256",state.scriptSha256},{"files",Json::array()}};
        if (state.moduleLoader) {
            metadata["schema"] = 2; metadata["bootRule"] = 2;
            metadata["moduleLoaderId"] = state.moduleLoader->id;
            const auto hash = Script::ScriptModuleLoaderIdentity(*state.moduleLoader);
            if (!hash.Succeeded()) return {hash.diagnostics};
            metadata["moduleLoaderSha256"] = *hash.value;
        }
        for (const auto& member : state.expectedMembers) {
            const auto hash = Sha256(member.data); if (!hash.Succeeded()) return {hash.diagnostics};
            metadata["files"].push_back({{"name",member.name},{"size",member.data.size()},{"sha256",*hash.value}});
        }
        MemberReplacement receipt{std::string(kMapRuntimeInstallationMember),Bytes(metadata.dump())};
        const auto installed = Set(task, receipt); if (!installed.Succeeded()) return installed;
        state.expectedMembers.push_back(std::move(receipt));
    } else { task.removals.push_back(std::string(kMapRuntimeInstallationMember)); state.removedMembers.push_back(std::string(kMapRuntimeInstallationMember)); }
    const auto encoded = table.Serialize(); if (!encoded.Succeeded()) return {encoded.diagnostics};
    // 导入表是共用派生产物，已验证其现有内容后替换同一条记录。
    bool replaced = false;
    for (auto& item : task.replacements) if (Key(item.name) == Key("war3map.imp")) { item.data = *encoded.value; replaced = true; }
    if (!replaced) task.replacements.push_back({"war3map.imp",*encoded.value});
    state.expectedMembers.push_back({"war3map.imp",*encoded.value});
    task.diagnostics.push_back({DiagnosticSeverity::Info,"MapRuntime",required ? "RUNTIME_INSTALLED" : "RUNTIME_REMOVED",
        required ? "已计划带入运行库 " + state.package->Id() + " " + state.package->Version() + "；仅经典1.27a；包写入不代表游戏验收通过。" : "已移除身份未变的受管运行库成员和启动。"});
    return Status::Ok();
}

Status VerifyMapRuntimeSave(const MapSaveTask& task) {
    if (!task.runtimeState) return Fail("RUNTIME_PLAN_MISSING", "保存缺少运行库策略快照。");
    const auto& state = *task.runtimeState;
    if (task.runtimePurpose != state.purpose)
        return Fail("RUNTIME_SOURCE_CHANGED", "运行库保存用途在准备之后变化，未发布旧策略产物。");
    if (task.runtimeApprovedPackageSha256 != state.approvedPackageSha256)
        return Fail("RUNTIME_SOURCE_CHANGED", "运行库批准在保存期间改变，未发布旧授权快照。");
    const auto settings = FinalMember(task, kMapRuntimeSettingsMember);
    if (!settings.Succeeded()) return {settings.diagnostics};
    if (*settings.value != state.settingsBytes) return Fail("RUNTIME_SOURCE_CHANGED", "地图运行库设置在保存派生阶段改变。");
    if (state.package) { const auto current = state.package->VerifySource(); if (!current.Succeeded()) return current; }
    for (const auto& expected : state.expectedMembers) {
        const auto actual = FinalMember(task, expected.name);
        if (!actual.Succeeded()) return {actual.diagnostics};
        if (!*actual.value || **actual.value != expected.data) return Fail("RUNTIME_MEMBER_CONFLICT", "发布前受管成员被其他派生产物改变：" + expected.name);
    }
    for (const auto& removed : state.removedMembers) {
        const auto actual = FinalMember(task, removed);
        if (!actual.Succeeded()) return {actual.diagnostics};
        if (*actual.value) return Fail("RUNTIME_MEMBER_CONFLICT", "已卸载受管成员在发布前重新出现：" + removed);
    }
    if (!state.active && state.previousIdentity.empty()) return Status::Ok();
    const auto script = FinalMember(task, state.scriptMember);
    if (!script.Succeeded() || !*script.value) return Fail("RUNTIME_BOOT_INVALID", "发布前主脚本缺失。");
    const auto hash = Sha256(**script.value);
    if (!hash.Succeeded() || *hash.value != state.scriptSha256) return Fail("RUNTIME_SOURCE_CHANGED", "主脚本在运行库终检之后变化。");
    const auto boot = Script::VerifyMapRuntimeBoot(Text(**script.value), state.active, state.moduleLoader);
    if (!boot.Succeeded() || !state.active) return boot;
    const auto diagnostic = Script::VerifyMapRuntimeTestDiagnostics(Text(**script.value), state.purpose == MapRuntimeSavePurpose::TestRun);
    if (!diagnostic.Succeeded()) return diagnostic;
    return Script::VerifyMapRuntimeIdentity(Text(**script.value), state.moduleLoader
        ? state.package->ModulePayloadSha256() : state.package->PayloadSha256());
}
}
