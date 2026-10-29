#include <windows.h>
using Next = void(__cdecl*)();
using Record = void(__cdecl*)(int);
Next next = nullptr;
Record record = nullptr;
#pragma section(".chain", execute, read)
__declspec(allocate(".chain")) unsigned char gates[10][32] = {{0x90}};
unsigned char nonExecutable[32]{};
extern "C" void __cdecl ChainProvider() { if (record) record(20); next(); if (record) record(21); }
extern "C" void __cdecl ChainSetNext(Next value, Record recorder) { next = value; record = recorder; }
extern "C" void* __cdecl ChainGate(int i) { return i >= 0 && i < 10 ? gates[i] : nullptr; }
extern "C" void* __cdecl ChainData() { return nonExecutable; }
