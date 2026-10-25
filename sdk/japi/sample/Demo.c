#include "audastra_japi.h"
#include <limits.h>
#include <string.h>

static const AjHost* services;
static int32_t AJ_CALL Add(void* user, const AjCall* call, const AjArg* args, uint32_t count, AjResult* out) {
    int64_t sum;
    (void)user; (void)call;
    if (count != 2 || args[0].kind != AJ_INT || args[1].kind != AJ_INT) return AJ_ARGUMENT;
    sum = (int64_t)args[0].integer + args[1].integer;
    if (sum < INT32_MIN || sum > INT32_MAX) return AJ_LIMIT;
    out->kind = AJ_INT; out->integer = (int32_t)sum;
    return AJ_OK;
}
static int32_t AJ_CALL Echo(void* user, const AjCall* call, const AjArg* args, uint32_t count, AjResult* out) {
    (void)user; (void)call;
    if (count != 1 || args[0].kind != AJ_TEXT) return AJ_ARGUMENT;
    return AjSetText(out, args[0].text, args[0].textSize);
}
static int32_t AJ_CALL Notify(void* user, const AjCall* call, const AjArg* args, uint32_t count, AjResult* out) {
    (void)user; (void)call;
    if (count != 1 || args[0].kind != AJ_TEXT) return AJ_ARGUMENT;
    out->kind = AJ_VOID;
    return services->emit(services->context, AJ_LOCAL, "message", args[0].text, args[0].textSize);
}
static int32_t AJ_CALL Start(void* user) { (void)user; return AJ_OK; }
static void AJ_CALL Stop(void* user) { (void)user; }
static int32_t AJ_CALL Stopped(void* user) { (void)user; return 1; }
static void AJ_CALL Destroy(void* user) { (void)user; services = 0; }
static const AjCommand commands[] = {
    {"demo.add", "ii", AJ_INT, AJ_SYNC_COMMAND, Add, 0},
    {"demo.echo", "s", AJ_TEXT, AJ_LOCAL_COMMAND, Echo, 0},
    {"demo.notify", "s", AJ_VOID, AJ_LOCAL_COMMAND, Notify, 0}
};
static const AjModule module = {
    sizeof(AjModule), AJ_ABI, "demo", "2.0.0", commands, 3, 0, Start, Stop, Stopped, Destroy
};
AJ_EXPORT int32_t AJ_CALL AudastraJapiInitV2(const AjHost* host, const AjModule** out) {
    if (!host || !out) return AJ_ARGUMENT;
    if (host->structSize != sizeof(AjHost) || host->abi != AJ_ABI || !host->emit) return AJ_VERSION;
    services = host;
    *out = &module;
    return AJ_OK;
}
