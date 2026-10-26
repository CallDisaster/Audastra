#include "War3MapCore/ApplicationPaths.h"
#include <Windows.h>
#include <array>

namespace War3Map {
std::filesystem::path InstallationRootFromExecutableDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(directory, error);
    if (error) return {};
    if (canonical.filename() == L"bin" && std::filesystem::is_regular_file(canonical.parent_path() / "manifest.json", error) && !error)
        return canonical.parent_path();
    return canonical;
}
std::filesystem::path CurrentInstallationRoot() {
    std::array<wchar_t, 32768> buffer{};
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) return {};
    return InstallationRootFromExecutableDirectory(std::filesystem::path(std::wstring(buffer.data(), size)).parent_path());
}
} // namespace War3Map
