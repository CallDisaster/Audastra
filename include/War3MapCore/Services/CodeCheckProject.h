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
    std::array<std::shared_ptr<const ObjectData::NativeObjectDocument>, 3> nativeObjects;
    std::array<std::shared_ptr<const ObjectData::LniObjectDocument>, 3> editableObjects;
    std::shared_ptr<const ObjectData::ObjectFieldSchema> objectSchema;
    std::vector<Script::ScriptProfileSourceEncoding> sourceEncodings;
    std::vector<Script::ScriptProfileIncludeAccess> includeAccess;
    // 只供 common.j / blizzard.j 最终来源选择，不放入大型导入资源。
    std::vector<MemberReplacement> baseReplacements;
    std::vector<std::string> baseRemovals;
    bool debug = false;
};

// 一个代码视图拥有一份缓存；后台持有内部互斥量，同一缓存不并行运行。
// 缓存按整个snapshot对象及binding换代，不让草稿借用旧修订的基础库/物编/框架来源。
class CodeCheckProjectCache final {
public:
    CodeCheckProjectCache();
    ~CodeCheckProjectCache();
    CodeCheckProjectCache(const CodeCheckProjectCache&) = delete;
    CodeCheckProjectCache& operator=(const CodeCheckProjectCache&) = delete;
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    friend CodeCheckResult CheckProjectCode(const std::shared_ptr<const CodeCheckProjectSnapshot>&,
        CodeCheckProjectCache&, Trigger::CodeBlockTarget, std::string, CodeLanguage,
        std::uint64_t, std::stop_token, CodeCheckLuaSources);
};

[[nodiscard]] std::string CodeCheckTargetSourceName(Trigger::CodeBlockTarget target, CodeLanguage language = CodeLanguage::Jass);

// 只在后台调用：私有作者草稿→生产整图生成→实际owned基础库/框架预处理→共同CheckCode。
// 缺少或不支持完整来源时退回明确标记的正文语法检查；不授予保存通过结论。
// lua 只用于 Lua 代码块：编辑器自带 Lua 环境的接口目录与地图 Lua 文件，有目录时另做源码级 jass.* 用法检查。
[[nodiscard]] CodeCheckResult CheckProjectCode(
    const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckProjectCache& cache, Trigger::CodeBlockTarget target, std::string draft,
    CodeLanguage language, std::uint64_t generation, std::stop_token cancel = {}, CodeCheckLuaSources lua = {});

// 独立Lua文本使用与工程代码块相同的只读本机运行库捕获，不接收外部伪造库存事实。
// 不把文本加入地图/执行代码，不授予全图上下文或保存发布资格。lua 与 CheckProjectCode 相同。
[[nodiscard]] CodeCheckResult CheckProjectLuaSource(
    const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    CodeCheckRequest request, std::stop_token cancel = {}, CodeCheckLuaSources lua = {});

} // namespace War3Map::Services
