#pragma once
#include "War3MapCore/Services/CodeCheckProject.h"

namespace War3Map::Services::Detail {
void CaptureLuaCheckContext(const std::shared_ptr<const CodeCheckProjectSnapshot>& snapshot,
    std::optional<Trigger::CodeBlockTarget> target, CodeCheckRequest& request, std::stop_token cancel);
}
