#pragma once

#include "GameUiAbi127.h"
#include <windows.h>

namespace Audastra::Runtime32::GameUi::Detail {

// 对象可能已经被游戏释放；读失败不向调用方泄露异常。
inline bool ReadWord(Address at, Address& value) noexcept {
    if (!at) return false;
    __try {
        value = *reinterpret_cast<const Address*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace Audastra::Runtime32::GameUi::Detail
