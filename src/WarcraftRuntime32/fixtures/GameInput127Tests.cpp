#include "../GameUi/GameInput127.h"
#include "audastra_japi.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace {
using namespace Audastra::Runtime32::GameUi;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Object {
    std::array<Address, 0x400 / sizeof(Address)> words{};
    Address Self() { return reinterpret_cast<Address>(words.data()); }
    void Set(std::uint32_t offset, Address value) { words.at(offset / sizeof(Address)) = value; }
};
Object root, normal, target, other, unit;
std::array<Address, 16> unitVtable{};
Address rootValue = 0;
bool validUnit = true, unitClass = true, raiseFault = false, recurse = false;
unsigned resolveCount = 0, submitCount = 0, faultCount = 0;
int consumed = 1;
UiFault fault{};
GameInput127* currentInput = nullptr;
UnitClickResult nested;

Address __fastcall GetRoot(int, int) { return rootValue; }
Address __fastcall Resolve(std::uint32_t handle) {
    ++resolveCount;
    return validUnit && handle == 1234 ? unit.Self() : 0;
}
std::uint32_t __fastcall RuntimeType(Address value) {
    Check(value == unit.Self(), "type query receives resolved object");
    // 独立记录的CUnit类型槽返回值；较宽的父类标签不能冒充单位。
    return unitClass ? 0x2B773375 : 0x2B773373;
}
int __fastcall Derived(std::uint32_t actual, std::uint32_t expected) {
    Check(expected == 0x2B773375, "CUnit class discriminator matches the fixed-image type slot");
    return actual == expected;
}
int __fastcall Submit(Address self, int zero, Address object, int button, Address event) {
    ++submitCount;
    Check(self == target.Self() && object == unit.Self(), "native input receives live target mode and unit");
    Check(zero == 0 && button == 1 && event == 0, "native input uses unit click arguments, no fabricated event");
    if (raiseFault) RaiseException(0xE050CA11, 0, 0, nullptr);
    if (recurse) nested = currentInput->ClickUnit(1234, 7, 8);
    return consumed;
}
void Observe(const UiFault& value) noexcept { fault = value; ++faultCount; }
GameInputEntryPoints Entries() {
    return {&GetRoot, &Resolve, &Derived, &Submit, 0x10000, 0x20000};
}
void Reset() {
    root = {}; normal = {}; target = {}; other = {}; unit = {};
    rootValue = root.Self();
    normal.Set(0, Entries().selectVtable); target.Set(0, Entries().targetVtable); other.Set(0, 0x30000);
    root.Set(InputRva127::CurrentMode, normal.Self());
    root.Set(InputRva127::SelectMode, normal.Self()); root.Set(InputRva127::TargetMode, target.Self());
    target.Set(InputRva127::ReleaseFlags, 4);
    unitVtable.fill(0); unitVtable[InputRva127::RuntimeTypeSlot / sizeof(Address)] = reinterpret_cast<Address>(&RuntimeType);
    unit.Set(0, reinterpret_cast<Address>(unitVtable.data())); unit.Set(InputRva127::UnitTypeId, 0x6870616C);
    validUnit = unitClass = true; raiseFault = recurse = false;
    resolveCount = submitCount = faultCount = 0; consumed = 1; fault = {}; nested = {};
}

void ModesAndUnits() {
    Reset(); GameInput127 input(Entries()); currentInput = &input;
    auto result = input.ClickUnit(1234, 1, 1);
    Check(result.status == AJ_OK && result.value == 0 && submitCount == 0, "ordinary select is not consumed");
    root.Set(InputRva127::CurrentMode, target.Self());
    result = input.ClickUnit(1234, 1, 2);
    Check(result.status == AJ_OK && result.value == 1 && submitCount == 1, "unit target enters the original input chain once");
    target.Set(InputRva127::ReleaseFlags, 6 | 0x2000000);
    Check(input.ClickUnit(1234, 1, 3).value == 1 && submitCount == 2, "target flags are left to the original input chain");
    for (const auto kind : {2u, 3u, 5u, 0x100u}) {
        target.Set(InputRva127::ReleaseFlags, kind);
        result = input.ClickUnit(1234, 1, 4);
        Check(result.status == AJ_OK && result.value == 3 && submitCount == 2, "point and other target modes are not converted");
    }
    root.Set(InputRva127::CurrentMode, other.Self());
    Check(input.ClickUnit(1234, 1, 5).value == 3 && submitCount == 2, "other mode preserves selection and input");
    root.Set(InputRva127::CurrentMode, normal.Self());
    Check(input.ClickUnit(0, 1, 6).status == AJ_ARGUMENT, "null unit refused");
    Check(input.ClickUnit(-1, 1, 7).status == AJ_ARGUMENT, "negative integer is not an address");
    validUnit = false;
    Check(input.ClickUnit(1234, 1, 8).status == AJ_ARGUMENT, "removed or unknown handle refused");
    validUnit = true; unitClass = false;
    Check(input.ClickUnit(1234, 1, 9).status == AJ_ARGUMENT, "nonunit agent refused");
    unitClass = true; unit.Set(InputRva127::UnitTypeId, 0);
    Check(input.ClickUnit(1234, 1, 10).status == AJ_ARGUMENT, "missing unit type refused");
    Check(submitCount == 2, "bad inputs never reach the original click function");
}

void LifecycleAndFaults() {
    Reset(); GameInput127 unavailable({});
    Check(unavailable.ClickUnit(1234, 1, 1).status == AJ_UNSUPPORTED && resolveCount == 0, "missing ABI refuses before game calls");
    GameInput127 input(Entries(), &Observe); currentInput = &input;
    std::int32_t wrongThread = AJ_OK;
    std::thread worker([&] { wrongThread = input.ClickUnit(1234, 1, 2).status; }); worker.join();
    Check(wrongThread == AJ_THREAD && resolveCount == 0, "wrong thread refused before memory and game calls");
    rootValue = 0;
    Check(input.ClickUnit(1234, 1, 3).status == AJ_STATE, "missing root is not a selection state");
    rootValue = root.Self(); root.Set(InputRva127::CurrentMode, 1);
    Check(input.ClickUnit(1234, 1, 4).status == AJ_UNSUPPORTED, "unreadable mode refuses without native input");
    root.Set(InputRva127::CurrentMode, target.Self()); recurse = true;
    Check(input.ClickUnit(1234, 1, 5).value == 1 && nested.status == AJ_REENTRANT && submitCount == 1,
        "synchronous callbacks cannot replay the click");
    recurse = false; consumed = 0;
    Check(input.ClickUnit(1234, 1, 6).status == AJ_UNSUPPORTED && submitCount == 2,
        "unconsumed native request is not success or ordinary selection");
    consumed = 1; raiseFault = true;
    Check(input.ClickUnit(1234, 7, 88).status == AJ_INTERNAL, "structured exception is bounded to input capability");
    Check(faultCount == 1 && fault.code == 0xE050CA11 && fault.epoch == 7 && fault.request == 88
        && std::string_view(fault.entry) == "audastraui.clickunit", "fault records actual code, location, session and request");
    Check(input.ClickUnit(1234, 7, 89).status == AJ_INTERNAL && submitCount == 3, "faulted operation is never retried");
    GameInput127 next(Entries()); next.Stop();
    Check(next.ClickUnit(1234, 8, 1).status == AJ_STALE && submitCount == 3, "stopped session cannot submit input");
}

void ResolverFailures() {
    GameInputEntryPoints out = Entries(); std::string reason;
    Check(!ResolveGameInputEntryPoints127(0, {}, out, reason) && !out.clickTargetWidget && !reason.empty(),
        "unverified image clears all input entries");
    Check(!ResolveGameInputEntryPoints127(0x10000000, [](Address, std::size_t) { return false; }, out, reason),
        "nonexecutable entry rejected");
    Check(!ResolveGameInputEntryPoints127(0xFFFFFFFF, [](Address, std::size_t) { return true; }, out, reason),
        "address overflow rejected");
}
} // namespace

int main() {
    try {
        ModesAndUnits(); LifecycleAndFaults(); ResolverFailures();
        std::cout << "PASS input modes, units, lifecycle, reentrancy, fault isolation and resolver gates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
