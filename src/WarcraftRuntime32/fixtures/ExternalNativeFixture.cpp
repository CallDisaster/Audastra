#include "War3MapCore/Runtime/TestNativeProviderAbi.h"

using Audastra::Runtime32::AddTestNative;
namespace {
std::uint32_t __cdecl AddSeven(std::uint32_t value) { return value + 7; }
std::uint32_t __cdecl EchoPlayer(std::uint32_t player) { return player; }
std::uint32_t __cdecl CallbackSentinel(std::uint32_t) { return 91; }
}

extern "C" BOOL WINAPI RegisterFixture(AddTestNative add, void* context) {
    return add(context, reinterpret_cast<std::uintptr_t>(&AddSeven), "AudastraFixtureAdd", "(I)I")
        && add(context, reinterpret_cast<std::uintptr_t>(&EchoPlayer), "AudastraFixturePlayer", "(Hplayer;)Hplayer;")
        && add(context, reinterpret_cast<std::uintptr_t>(&CallbackSentinel), "AudastraFixtureCallback", "(C)I");
}

extern "C" BOOL WINAPI RegisterFixtureWrongSignature(AddTestNative add, void* context) {
    return add(context, reinterpret_cast<std::uintptr_t>(&AddSeven), "AudastraFixtureAdd", "(I)B");
}

extern "C" BOOL WINAPI RegisterFixtureMissing(AddTestNative, void*) {
    return TRUE;
}
