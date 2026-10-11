#include "../OwnLua/LuaHost.h"
#include "../OwnLua/NativeCallX86.h"
#include "../OwnLua/NativeProvider.h"
#include "../OwnLua/NativeRegistry.h"

#include <windows.h>
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
using namespace Audastra::Runtime32;
using namespace Audastra::Runtime32::OwnLua;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Success(const LuaHostResult& result, const char* context) {
    if (!result.Succeeded()) throw std::runtime_error(std::string(context) + ": " + result.message);
}
std::uint32_t __cdecl FixtureMix(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return a + 10 * b + 100 * c;
}
std::uint32_t __cdecl FixtureReal(const float* a, const float* b) { return std::bit_cast<std::uint32_t>(*a + *b); }
std::uint32_t __cdecl FixtureNone() { return 0x12345678; }
std::uint32_t __cdecl FixtureException() { RaiseException(0xe0420042, 0, 0, nullptr); return 0; }

struct HandleLife { bool alive = true; std::size_t references = 0; };
class FixtureOwner final : public NativeHandleOwner {
public:
    explicit FixtureOwner(std::shared_ptr<HandleLife> value) : life_(std::move(value)) { ++life_->references; }
    ~FixtureOwner() override { --life_->references; }
    bool Alive() const noexcept override { return life_->alive; }
private:
    std::shared_ptr<HandleLife> life_;
};

// 原创类型与生命周期提供者。cdecl fixture 只验证调用桥，不表示已接通 Game.dll VM。
class FixtureProvider final : public NativeProvider {
public:
    std::uint64_t generation = 7;
    std::size_t calls = 0;
    bool throwLookup = false;
    std::shared_ptr<HandleLife> timer = std::make_shared<HandleLife>();
    std::shared_ptr<HandleLife> unit = std::make_shared<HandleLife>();
    const std::map<std::string, std::string, std::less<>> signatures{
        {"Integer", "(I)I"}, {"Real", "(RR)R"}, {"Boolean", "(B)B"}, {"String", "(S)S"},
        {"Handle", "()Hhandle;"}, {"Timer", "()Htimer;"}, {"Unit", "()Hunit;"},
        {"UseTimer", "(Htimer;)I"}, {"UseHandle", "(Hhandle;)I"},
        {"Callback", "(C)V"}, {"ReturnedCode", "()C"}, {"Void", "()V"},
        {"WrongResult", "()I"}, {"WrongHandle", "()Htimer;"}, {"StaleHandle", "()Htimer;"},
        {"BadReal", "()R"}, {"BadString", "()S"}, {"Throw", "(Htimer;)V"}, {"Fail", "()V"},
        {"Red", "()Hplayercolor;"}, {"ZeroRace", "()Hrace;"}, {"ColorValue", "(Hplayercolor;)I"}
    };
    std::uint64_t Generation() const noexcept override { return generation; }
    std::optional<NativeSignature> Lookup(std::string_view name) const override {
        if (throwLookup) throw std::runtime_error("fixture lookup failure");
        const auto found = signatures.find(name);
        return found == signatures.end() ? std::optional<NativeSignature>{} : ParseNativeSignature(found->second);
    }
    bool IsHandleType(std::string_view actual, std::string_view expected) const noexcept override {
        const bool known = actual == "timer" || actual == "unit" || actual == "handle" || actual == "playercolor" || actual == "race";
        return known && (actual == expected || expected == "handle");
    }
    NativeHandle Handle(std::uint32_t value, std::string type, const std::shared_ptr<HandleLife>& life) {
        return {value, generation, std::move(type), std::make_shared<FixtureOwner>(life)};
    }
    NativeCallResult Invoke(std::string_view name, std::span<const NativeValue> args) override {
        ++calls;
        if (name == "Integer" || name == "String" || name == "Boolean") return {true, args[0], {}};
        if (name == "Red" || name == "ZeroRace") {
            auto handle = Handle(0, name == "Red" ? "playercolor" : "race", unit);
            handle.kind = NativeHandleKind::Scalar;
            return {true, std::move(handle), {}};
        }
        if (name == "ColorValue") return {true, static_cast<std::int32_t>(std::get<NativeHandle>(args[0]).value), {}};
        if (name == "Real") {
            const auto a = std::get<float>(args[0]), b = std::get<float>(args[1]);
            const std::array words{reinterpret_cast<std::uint32_t>(&a), reinterpret_cast<std::uint32_t>(&b)};
            const auto called = InvokeNativeX86(reinterpret_cast<std::uintptr_t>(&FixtureReal), words);
            return {called.succeeded, std::bit_cast<float>(called.value), {}};
        }
        if (name == "Handle") return {true, Handle(42, "handle", timer), {}};
        if (name == "Timer") return {true, Handle(42, "timer", timer), {}};
        if (name == "Unit" || name == "WrongHandle") return {true, Handle(43, "unit", unit), {}};
        if (name == "StaleHandle") { auto value = Handle(44, "timer", timer); --value.generation; return {true, value, {}}; }
        if (name == "UseTimer" || name == "UseHandle") {
            return {true, static_cast<std::int32_t>(std::holds_alternative<std::monostate>(args[0]) ? 0 : std::get<NativeHandle>(args[0]).value), {}};
        }
        if (name == "Void") return {true, {}, {}};
        if (name == "WrongResult") return {true, std::string("not an integer"), {}};
        if (name == "BadReal") return {true, std::numeric_limits<float>::infinity(), {}};
        if (name == "BadString") return {true, std::string("a\0b", 3), {}};
        if (name == "Throw") throw std::runtime_error("fixture provider failure");
        if (name == "ReturnedCode") return {true, std::int32_t{1}, {}};
        return {false, {}, "fixture refused"};
    }
};

void NativeCalls() {
    const std::array<std::uint32_t, 3> words{1, 2, 3};
    const auto ordered = InvokeNativeX86(reinterpret_cast<std::uintptr_t>(&FixtureMix), words);
    Check(ordered.succeeded && ordered.value == 321, "cdecl argument order and EAX return");
    Check(InvokeNativeX86(reinterpret_cast<std::uintptr_t>(&FixtureNone), {}).value == 0x12345678, "zero args cdecl");
    const auto failed = InvokeNativeX86(reinterpret_cast<std::uintptr_t>(&FixtureException), {});
    Check(!failed.succeeded && failed.exception == 0xe0420042, "SEH exception produces explicit failed result");
    const std::array<std::uint32_t, 33> tooMany{};
    Check(!InvokeNativeX86(reinterpret_cast<std::uintptr_t>(&FixtureNone), tooMany).succeeded, "argument cap");
    Check(!InvokeNativeX86(0, {}).succeeded, "null function refused");

    NativeRegistry registry;
    Check(registry.Observe("Mix", "(III)I", reinterpret_cast<std::uintptr_t>(&FixtureMix)), "registration");
    Check(registry.Observe("Mix", "(III)I", reinterpret_cast<std::uintptr_t>(&FixtureMix)), "identical registration");
    Check(registry.Size() == 1 && registry.Find("Mix") && !registry.Find("Missing"), "lookup");
    Check(!registry.Observe("Mix", "()I", reinterpret_cast<std::uintptr_t>(&FixtureNone)), "changed registration refused");
    Check(!registry.Valid() && !registry.Error().empty() && !registry.Find("Mix"), "registry failure latches");
    NativeRegistry invalid;
    Check(!invalid.Observe("Invalid", "(V)V", 1), "invalid signature refuses registry");
}

void ConversionAndLifetimes() {
    const auto provider = std::make_shared<FixtureProvider>();
    {
        LuaHost host({});
        Check(!host.BindNatives(provider).Succeeded(), "bind before initialization refused");
        Success(host.Initialize(), "initialize");
        Success(host.BindNatives(provider), "bind original provider");
        Check(!host.BindNatives(provider).Succeeded(), "duplicate provider refused");
        Success(host.Execute("j=require('jass.common'); assert(j.Integer(-2147483648)==-2147483648); assert(j.Integer(2147483647)==2147483647); assert(j.Integer(2.0)==2); assert(j.Real(1.25,2.5)==3.75); assert(j.Boolean(false)==false); assert(j.String('中文')=='中文'); assert(j.String(nil)==nil); assert(select('#',j.Void())==0)", "@types.lua"), "primitive conversion");
        for (const auto source : {"j.Integer(2147483648)", "j.Integer(-2147483649)", "j.Integer(1.5)", "j.Integer('1')",
            "j.Boolean(1)", "j.Real(0/0,1)", "j.Real(math.huge,1)", "j.Real(1e40,1)", "j.Real('1',1)",
            "j.String(1)", "j.String('a'..string.char(0)..'b')", "j.String(string.char(255))", "j.Integer()", "j.Integer(1,2)",
            "j.Callback(function() return true end)", "j.UseTimer(42)", "j.Unknown()"}) {
            const auto before = provider->calls;
            const auto result = host.Execute(source, "@invalid.lua");
            Check(result.code == LuaReloadResult::RuntimeError && result.message.find("invalid.lua") != std::string::npos,
                "invalid argument has source location");
            Check(provider->calls == before, "invalid argument does not call native");
        }
        Success(host.Execute("generic=j.Handle(); keys={[generic]='present'}; timer=j.Timer(); assert(rawequal(generic,timer)); assert(keys[timer]=='present'); assert(j.UseTimer(generic)==42); assert(j.UseHandle(timer)==42); assert(j.UseTimer(nil)==0); unit=j.Unit(); assert(j.UseHandle(unit)==43); assert(not rawequal(unit,timer)); assert(getmetatable(timer)=='native handle')", "@handles.lua"), "typed handle identity and upgrade");
        Check(provider->timer->references == 1 && provider->unit->references == 1, "canonical userdata owns only one reference");
        const auto beforeWrong = provider->calls;
        Check(host.Execute("j.UseTimer(unit)", "@wrong-type.lua").code == LuaReloadResult::RuntimeError, "unrelated handle rejected");
        Check(provider->calls == beforeWrong, "wrong handle does not reach provider");
        for (const auto source : {"j.WrongResult()", "j.WrongHandle()", "j.StaleHandle()", "j.BadReal()", "j.BadString()", "j.ReturnedCode()", "j.Fail()", "j.Throw(timer)"}) {
            Check(host.Execute(source, "@provider.lua").code == LuaReloadResult::RuntimeError, "bad provider result refused");
        }
        Check(provider->timer->references == 1, "C++ unwinding releases temporary handle arguments on error");
        provider->throwLookup = true;
        Check(host.Execute("j.Integer(1)", "@lookup.lua").message.find("fixture lookup failure") != std::string::npos, "lookup exception reported");
        provider->throwLookup = false;
        provider->timer->alive = false;
        const auto beforeDead = provider->calls;
        Check(host.Execute("j.UseTimer(timer)", "@dead.lua").code == LuaReloadResult::RuntimeError && provider->calls == beforeDead, "destroyed handle refused");
        const auto previous = provider->timer;
        provider->timer = std::make_shared<HandleLife>();
        Success(host.Execute("replacement=j.Timer(); assert(not rawequal(timer,replacement)); assert(keys[replacement]==nil); assert(j.UseTimer(replacement)==42)", "@reuse.lua"), "reused handle ID does not revive stale Lua identity");
        Check(previous->references == 1 && provider->timer->references == 1, "both retained identities release independently");
        Success(host.Execute("red=j.Red(); racezero=j.ZeroRace(); assert(red~=nil and j.ColorValue(red)==0); assert(rawequal(red,j.Red())); assert(red~=racezero); assert(not rawequal(red,racezero)); enumkeys={[red]='red',[racezero]='race'}; assert(enumkeys[j.Red()]=='red')", "@scalar-handles.lua"), "zero-valued native enums preserve type and Lua table identity");
        Check(!host.Execute("j.ColorValue(racezero)", "@wrong-enum.lua").Succeeded(), "different zero-valued native enum types do not interchange");
        Success(host.Execute("cached=j.Integer", "@cache.lua"), "cache native closure");
        ++provider->generation;
        const auto beforeStale = provider->calls;
        Check(host.Execute("cached(1)", "@generation.lua").code == LuaReloadResult::RuntimeError && provider->calls == beforeStale, "cached closure refuses expired generation");
    }
    Check(provider->timer->references == 0 && provider->unit->references == 0, "lua_close releases native handle owners");
    LuaHost wrongThread({}); Success(wrongThread.Initialize(), "thread initialize");
    bool refused = false;
    std::thread worker([&] { refused = !wrongThread.BindNatives(provider).Succeeded(); }); worker.join();
    Check(refused, "only initialize thread may bind provider");
}
}

int main() {
    try {
        NativeCalls(); ConversionAndLifetimes();
        std::cout << "Own Lua native conversions passed; original cdecl/handle fixture only; game_evidence=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
