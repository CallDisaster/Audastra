#pragma once
#include "Host.h"
#include <filesystem>
#include <span>

namespace audastra::japi {
struct ModulePin {
    ModuleContract contract;
    std::string sha256; // trusted installation policy; never accepted from map content
};
// Windows game deployment is deliberately x86-only. No development-mode policy bypass.
class PinnedDllLoader final : public Loader {
public:
    PinnedDllLoader(std::filesystem::path trustedDirectory, std::vector<ModulePin> pins,
        std::filesystem::path stagingParent = {});
    LoadResult Open(std::string_view name) override;
private:
    std::filesystem::path directory_, staging_;
    std::vector<ModulePin> pins_;
};
std::string Sha256(std::span<const uint8_t> bytes);
bool InspectPe32(std::span<const uint8_t> bytes);
}
