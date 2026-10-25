// Original test module. Its worker only emits local test events; no game/file/network access.
#include "audastra_japi.h"
#include <windows.h>
#include <atomic>

namespace {
const AjHost* host = nullptr;
HANDLE thread = nullptr, beginEvent = nullptr, stopEvent = nullptr;
std::atomic<int32_t> attempts{0};
DWORD WINAPI Run(void*) {
    HANDLE waiting[]{stopEvent, beginEvent};
    if (WaitForMultipleObjects(2, waiting, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        for (int i = 0; i < 1000 && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0; ++i) {
            host->emit(host->context, AJ_ASYNC, "worker", "original test", 13);
            attempts.fetch_add(1);
        }
        WaitForSingleObject(stopEvent, INFINITE);
    }
    return 0;
}
int32_t AJ_CALL Start(void*) {
    attempts = 0;
    beginEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!beginEvent || !stopEvent) return AJ_INTERNAL;
    thread = CreateThread(nullptr, 0, Run, nullptr, 0, nullptr);
    return thread ? AJ_OK : AJ_INTERNAL;
}
void AJ_CALL Stop(void*) { if (stopEvent) SetEvent(stopEvent); }
int32_t AJ_CALL Stopped(void*) {
    if (!thread) return 1;
    if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) return 0;
    CloseHandle(thread); thread = nullptr;
    return 1;
}
void AJ_CALL Destroy(void*) {
    if (beginEvent) CloseHandle(beginEvent);
    if (stopEvent) CloseHandle(stopEvent);
    beginEvent = stopEvent = nullptr;
    host = nullptr;
}
int32_t AJ_CALL Begin(void*, const AjCall*, const AjArg*, uint32_t n, AjResult* out) {
    if (n) return AJ_ARGUMENT;
    out->kind = AJ_VOID;
    return SetEvent(beginEvent) ? AJ_OK : AJ_INTERNAL;
}
int32_t AJ_CALL Count(void*, const AjCall*, const AjArg*, uint32_t n, AjResult* out) {
    if (n) return AJ_ARGUMENT;
    out->kind = AJ_INT; out->integer = attempts.load(); return AJ_OK;
}
const AjCommand commands[] = {{"worker.begin", "", AJ_VOID, AJ_LOCAL_COMMAND, Begin, nullptr},
    {"worker.count", "", AJ_INT, AJ_LOCAL_COMMAND, Count, nullptr}};
const AjModule descriptor{sizeof(AjModule), AJ_ABI, "worker", "2.0.0", commands, 2, nullptr, Start, Stop, Stopped, Destroy};
}
extern "C" AJ_EXPORT int32_t AJ_CALL AudastraJapiInitV2(const AjHost* services, const AjModule** out) {
    if (!services || !out) return AJ_ARGUMENT;
    if (services->abi != AJ_ABI || services->structSize != sizeof(AjHost)) return AJ_VERSION;
    host = services; *out = &descriptor; return AJ_OK;
}
