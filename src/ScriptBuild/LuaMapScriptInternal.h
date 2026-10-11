#pragma once

#include "War3MapCore/Script/LuaMapScript.h"
#include "LuaTriggerEmitter.h"
#include "ScriptText.h"

#include <map>
#include <set>

namespace War3Map::Script::lua {

// writer与语法流水线共用；先从实际文本重算SHA并验证范围，再生成整图映射身份。
[[nodiscard]] Result<std::string> ComputeLuaMapSourceMapSha256(const LuaMapScriptResult& result);

// 只接收owned初始化事实；所有外部调用均经显式目标签名和常量目录。
class MapWriter {
public:
    MapWriter(const MapInitializationPlan& plan, const Trigger::TriggerDocument& document,
        const Trigger::UiCatalog& catalog, const LuaTargetProfile& target,
        std::span<const LuaCallbackBinding> callbacks, LuaTriggerBuildOptions options);
    LuaMapScriptResult Run();

private:
    [[noreturn]] void Fail(std::string code, std::string message) const;
    void Prepare();
    bool Assignable(std::string actual, std::string_view expected) const;
    Expression Api(std::string_view name, std::string_view result, std::initializer_list<Expression> arguments = {});
    Expression Constant(std::string_view name, std::string_view type);
    Expression Global(std::string_view name, std::string_view type);
    Expression Real(double value) const;
    Expression Rawcode(std::string_view value);
    Expression Player(std::int32_t number);
    static Expression Integer(std::int64_t value);
    static Expression Boolean(bool value);
    static Expression String(std::string_view value);
    std::string Resolve(std::string_view text) const;
    void Line(std::string_view value);
    void Call(std::string_view name, std::initializer_list<Expression> arguments = {});
    void Begin(std::string_view name);
    void End();
    std::size_t ObjectBegin(std::string_view symbol);
    void ObjectEnd(std::string_view symbol, std::size_t start);
    void Declarations();
    void Sounds();
    void Regions();
    void Cameras();
    void Destructables();
    void Items();
    void Units();
    void Unit(const MapInitUnit& value);
    void Upgrades();
    void TechTree();
    void Players();
    void Main();
    void Config();
    void Finish();

    const MapInitializationPlan& plan_;
    const Trigger::TriggerDocument& document_;
    const Trigger::UiCatalog& catalog_;
    const LuaTargetProfile& target_;
    std::span<const LuaCallbackBinding> callbacks_;
    LuaTriggerBuildOptions options_;
    LuaMapScriptResult result_;
    ScriptText out_;
    std::map<std::string, const LuaApiSignature*> apis_;
    std::map<std::string, std::string> constants_, parents_;
    std::map<std::string, MapInitSymbol> symbols_;
    std::set<std::string> requiredApis_, requiredGlobals_, requiredConstants_;
    std::vector<LuaMapObjectSpan> objectSpans_;
    OriginSpan currentOrigin_;
    std::string currentObject_;
};

} // namespace War3Map::Script::lua
