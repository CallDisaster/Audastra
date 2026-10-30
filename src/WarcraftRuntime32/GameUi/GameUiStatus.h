#pragma once
#include "GameUiLibrary.h"
#include "audastra_japi.h"

namespace Audastra::Runtime32::GameUi {
[[nodiscard]] inline std::int32_t StatusOf(UiError error) noexcept {
    switch (error) {
    case UiError::None: return AJ_OK;
    case UiError::NotReady: return AJ_STATE;
    case UiError::WrongThread: return AJ_THREAD;
    case UiError::BadHandle:
    case UiError::NotFound:
    case UiError::BadArgument:
    case UiError::ParentCycle: return AJ_ARGUMENT;
    case UiError::StaleFrame: return AJ_STALE;
    case UiError::Unsupported:
    case UiError::UnsupportedParent: return AJ_UNSUPPORTED;
    case UiError::TableFull: return AJ_LIMIT;
    case UiError::NotOwned: return AJ_STATE;
    case UiError::Faulted: return AJ_INTERNAL;
    }
    return AJ_INTERNAL;
}
}
