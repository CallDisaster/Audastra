/* Audastra JAPI 2.0 — C ABI. This is a trusted in-process SDK, not a sandbox. */
#ifndef AUDASTRA_JAPI_H
#define AUDASTRA_JAPI_H
#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#define AJ_CALL __cdecl
#define AJ_EXPORT __declspec(dllexport)
#else
#define AJ_CALL
#define AJ_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define AJ_ABI 0x00020000u
#define AJ_MAX_FRAME 512u
#define AJ_MAX_ARGUMENTS 16u
#define AJ_MAX_INPUT_TEXT 384u /* encoded bytes */
#define AJ_MAX_OUTPUT_TEXT 416u /* encoded bytes; storage includes terminator */
#define AJ_MAX_EVENT_TEXT 256u /* encoded bytes */
#define AJ_MAX_COMMANDS 256u
#define AJ_INIT_EXPORT "AudastraJapiInitV2"

typedef enum AjStatus {
    AJ_OK = 0, AJ_FRAME = 1, AJ_VERSION = 2, AJ_LIMIT = 3,
    AJ_ENCODING = 4, AJ_ARGUMENT = 5, AJ_UNKNOWN = 6, AJ_CARRIER = 7,
    AJ_STATE = 8, AJ_THREAD = 9, AJ_BUSY = 10, AJ_REENTRANT = 11,
    AJ_UNSUPPORTED = 12, AJ_BACKEND = 13, AJ_POLICY = 14,
    AJ_STALE = 15, AJ_DISPATCH = 16, AJ_INTERNAL = 17
} AjStatus;
typedef enum AjKind {
    AJ_VOID = 0, AJ_INT = 1, AJ_REAL = 2, AJ_TEXT = 3, AJ_BOOL = 4, AJ_HANDLE = 5
} AjKind;
typedef enum AjEventClass { AJ_SYNC = 0, AJ_LOCAL = 1, AJ_ASYNC = 2 } AjEventClass;
typedef enum AjDomain { AJ_LOCAL_COMMAND = 0, AJ_SYNC_COMMAND = 1 } AjDomain;

/* No STL, exceptions, allocator ownership or borrowed result strings cross the ABI. */
#pragma pack(push, 8)
typedef struct AjArg {
    int32_t kind;
    int32_t integer;
    float real;
    const char* text; /* borrowed for this invocation only; UTF-8, already decoded */
    uint32_t textSize;
} AjArg;
typedef struct AjResult {
    int32_t kind;
    int32_t integer;
    float real;
    uint32_t textSize;
    char text[AJ_MAX_OUTPUT_TEXT + 1u]; /* caller-owned; no pointer replacement */
} AjResult;
typedef struct AjCall {
    uint32_t structSize;
    uint32_t epoch;
    uint32_t instance;
    uint32_t request;
} AjCall;
typedef int32_t(AJ_CALL* AjInvoke)(void*, const AjCall*, const AjArg*, uint32_t, AjResult*);
typedef struct AjCommand {
    const char* name;      /* module.function, lowercase ASCII */
    const char* arguments; /* i/r/s/b/d; at most 16 */
    int32_t resultKind;
    int32_t domain;        /* local or deterministic/gameplay; not automatic network sync */
    AjInvoke invoke;
    void* user;
} AjCommand;
typedef struct AjHost {
    uint32_t structSize;
    uint32_t abi;
    uint32_t epoch;
    uint32_t instance;
    void* context;
    /* The only worker-thread service. Strings are copied before return.
       SYNC is game-thread-only and succeeds only after acknowledgement AND callback return. */
    int32_t(AJ_CALL* emit)(void*, int32_t eventClass, const char* type,
        const char* payload, uint32_t payloadSize);
} AjHost;
typedef struct AjModule {
    uint32_t structSize;
    uint32_t abi;
    const char* name;
    const char* version;
    const AjCommand* commands;
    uint32_t commandCount;
    void* user;
    /* Init only supplies static metadata. Allocate/spawn in start, not Init/DllMain.
       All lifecycle callbacks run on the game thread without a host mutex held. */
    int32_t(AJ_CALL* start)(void*);
    void(AJ_CALL* requestStop)(void*); /* nonblocking; exactly once */
    int32_t(AJ_CALL* pollStopped)(void*); /* 0 busy; 1 all workers joined; other values fail closed */
    void(AJ_CALL* destroy)(void*); /* only after pollStopped == 1 */
} AjModule;
typedef int32_t(AJ_CALL* AjInit)(const AjHost*, const AjModule**);
#pragma pack(pop)

/* Copies while the author's stack storage is alive. Host validates UTF-8 and encoded size. */
static inline int32_t AjSetText(AjResult* out, const char* value, uint32_t size) {
    uint32_t i;
    if (!out || (!value && size)) return AJ_ARGUMENT;
    if (size > AJ_MAX_OUTPUT_TEXT) return AJ_LIMIT;
    out->kind = AJ_TEXT;
    out->textSize = size;
    for (i = 0; i < size; ++i) out->text[i] = value[i];
    out->text[size] = 0;
    return AJ_OK;
}
#ifdef __cplusplus
}
static_assert(sizeof(AjResult) == 436, "ABI packing changed");
static_assert(sizeof(AjCall) == 16, "ABI packing changed");
static_assert(sizeof(AjArg) == (sizeof(void*) == 4 ? 20 : 32), "ABI packing changed");
static_assert(sizeof(AjCommand) == (sizeof(void*) == 4 ? 24 : 40), "ABI packing changed");
static_assert(sizeof(AjHost) == (sizeof(void*) == 4 ? 24 : 32), "ABI packing changed");
static_assert(sizeof(AjModule) == (sizeof(void*) == 4 ? 44 : 80), "ABI packing changed");
#endif
#endif
