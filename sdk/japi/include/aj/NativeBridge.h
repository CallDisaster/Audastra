#pragma once
#include "Host.h"

namespace audastra::japi {
// Integration seam for an independently verified VM adapter. This class does not
// discover addresses, install hooks, register natives or launch the game.
// The adapter must copy out into VM-owned string storage before returning to JASS.
inline bool HandleLocalizedString(Host& host, std::string_view input, std::string& out) {
    auto reply = host.Dispatch(input);
    if (!reply.consumed) return false; // forward unchanged to the original native
    out = EncodeResponse(reply);
    return true;
}
}
