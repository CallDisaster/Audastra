#include <windows.h>
using Record = void(__cdecl*)(int);
volatile LONG calls = 0;
Record record = nullptr;
extern "C" void __cdecl ChainOriginalCall() { InterlockedIncrement(&calls); if (record) record(30); }
extern "C" __declspec(naked) void __cdecl ChainEntry() {
    __asm {
        call ChainOriginalCall
        ret
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
    }
}
extern "C" __declspec(naked) int __cdecl ChainAbsolute() {
    __asm {
        mov eax, dword ptr [calls]
        ret
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
    }
}
extern "C" void __cdecl ChainSetRecord(Record value) { record = value; }
extern "C" int __cdecl ChainCount() { return static_cast<int>(calls); }
