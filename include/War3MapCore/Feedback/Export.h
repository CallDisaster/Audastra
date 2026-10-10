#pragma once

#include "War3MapCore/Feedback/Transport.h"

namespace War3Map::Feedback {

struct ExportResult {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    std::string sha256;
};

} // namespace War3Map::Feedback
