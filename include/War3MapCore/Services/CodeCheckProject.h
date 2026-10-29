#pragma once

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/Services/CodeCheck.h"
#include "War3MapCore/Script/ScriptToolchainProfile.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Trigger/TriggerRemovalAudit.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>

namespace War3Map::Services {

// 宿主在界面线程冻结结构副本；所有指针保活自己的不可变副本，不能借用实时工程。
// 保存资格、归档发布、脚本执行均不属于此只读检查快照。
struct CodeCheckLuaSources {
    std::shared_ptr<const Lua::ApiCatalog> catalog;
    std::map<std::string, std::string, std::less<>> modules; // require 名 → 正文
};

struct CodeCheckProjectSnapshot {
    std::string binding;
    std::uint64_t revision = 0;
    std::filesystem::path sourcePath;
    std::string sourceSha256;
    std::filesystem::path toolRoot, uiRoot, gameRoot;
    // 与F9同次冻结的配置来源；与JASS编辑/保存的工具和UI层独立。
    // 显式runtimeRoot为Flat配置；空时才使用归一后的runtimeYdweRoot Tree。
    std::filesystem::path luaRuntimeRoot, luaRuntimeYdweRoot;
    std::shared_ptr<const Trigger::TriggerDocument> triggers;
    std::shared_ptr<const Trigger::UiCatalog> catalog;
    std::optional<MapDocument> map;
    std::optional<Script::TerrainExtent> terrain;
    // 生成查询使用前三类；模板slk必须绑定本次七类物编的未保存覆盖。
    std::array<std::shared_ptr<const ObjectData::NativeObjectDocument>, 7> nativeObjects;
    std::array<std::shared_ptr<const ObjectData::LniObjectDocument>, 7> editableObjects;
    std::shared_ptr<const ObjectData::ObjectFieldSchema> objectSchema;
    std::vector<Script::ScriptProfileSourceEncoding> sourceEncodings;
    std::vector<Script::ScriptProfileIncludeAccess> includeAccess;
    // 只供 common.j / blizzard.j 最终来源选择，不放入大型导入资源。
    std::vector<MemberReplacement> baseReplacements;
    std::vector<std::string> baseRemovals;
    bool debug = false;
    bool scriptAuditOnly = false;
};

// 编辑器里执行触发器用（界面模拟）的整图：实际基础库与翻译后的整图 JASS（见 BuildProjectJassScript）。
struct ProjectJassScript {
    std::string common;
    std::string blizzard;
    std::string map;
};

// 一个代码视图拥有一份缓存；后台持有内部互斥量，同一缓存不并行运行。
// 缓存按整个snapshot对象及binding换代，不让草稿借用旧修订的基础库/物编/框架来源。
class CodeCheckProjectCache final {
public:
    CodeCheckProjectCache();
    ~CodeCheckProjectCache();
    CodeCheckProjectCache(const CodeCheckProjectCache&) = delete;
    CodeCheckProjectCache& operator=(const CodeCheckProjectCache&) = delete;
    // 释放已捕获的整图上下文（归档、地图、游戏数据、原版物编与工具链来源，大图约 1 GB）；
    // 有检查正在使用时不等待、返回假。下次检查按当时的 snapshot 重新捕获。
    [[nodiscard]] bool TryRelease() noexcept;
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    friend CodeCheckResult CheckProjectCode(const std::shared_ptr<const CodeCheckProjectSnapshot>&,
        CodeCheckProjectCache&, Trigger::CodeBlockTarget, std::string, CodeLanguage,
        std::uint64_t, std::stop_token, CodeCheckLuaSources);
    friend CodeCheckResult CheckProjectTriggers(const std::shared_ptr<const CodeCheckProjectSnapshot>&,
        CodeCheckProjectCache&, std::shared_ptr<const Trigger::TriggerDocument>, std::uint64_t, std::stop_token);
    friend Result<ProjectJassScript> BuildProjectJassScript(const std::shared_ptr<const CodeCheckProjectSnapshot>&,
        CodeCheckProjectCache&, std::shared_ptr<const Trigger::TriggerDocument>, std::stop_token);
    friend Trigger::RemovalPreprocessProvider MakeProjectRemovalPreprocessProvider(std::shared_ptr<const CodeCheckProjectSnapshot>);
};

[[nodiscard]] std::string CodeCheckTargetSourceName(Trigger::CodeBlockTarget target, CodeLanguage language = CodeLanguage::Jass);

// 只在后台调用：私有作者草稿→生产整图生成→实际owned基础库/框架预处理→共同CheckCode。
// 缺少或不支持完整来源时退回明确标记的正文语法检查；不授予保存通过结论。
// lua 只用于 Lua 代码块：编辑器自带 Lua 环境的接口目录与地图 Lua 文件，有目录时另做源码级 jass.* 用法检查。
[[nodiscard]] CodeCheckResult CheckProjectCode(
    const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckProjectCache& cache, Trigger::CodeBlockTarget target, std::string draft,
    CodeLanguage language, std::uint64_t generation, std::stop_token cancel = {}, CodeCheckLuaSources lua = {});

// 用给定触发器文档（通常是尚未发布的编辑候选）按保存链生成整图并做同一 JASS 检查。
// 落在 GUI 语句生成代码上的诊断附 trigger 位置（紧凑 T 行列）；不写地图、不执行代码。
// 没有完整整图上下文时不检查，明确返回 TRIGGER_CHECK_CONTEXT_INCOMPLETE。
[[nodiscard]] CodeCheckResult CheckProjectTriggers(const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckProjectCache& cache, std::shared_ptr<const Trigger::TriggerDocument> document,
    std::uint64_t generation, std::stop_token cancel = {});

// 界面模拟用：与 CheckProjectTriggers 同一条生产整图链（生成 → 注入/预处理 → 模板）展开，再经同一 vJass 翻译。
// 不检查、不写地图、不执行代码；上下文不完整或翻译不了时给出原因。
[[nodiscard]] Result<ProjectJassScript> BuildProjectJassScript(const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckProjectCache& cache, std::shared_ptr<const Trigger::TriggerDocument> document, std::stop_token cancel = {});
// 独立Lua文本使用与工程代码块相同的只读本机运行库捕获，不接收外部伪造库存事实。
// 不把文本加入地图/执行代码，不授予全图上下文或保存发布资格。lua 与 CheckProjectCode 相同。
[[nodiscard]] CodeCheckResult CheckProjectLuaSource(
    const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckRequest request, std::stop_token cancel = {}, CodeCheckLuaSources lua = {});

// UI/CLI/MCP共用宿主来源；闭包拥有缓存，只读且每次重新核对包含与档案身份。
[[nodiscard]] Trigger::RemovalPreprocessProvider MakeProjectRemovalPreprocessProvider(
    std::shared_ptr<const CodeCheckProjectSnapshot> snapshot);
} // namespace War3Map::Services
