#include "War3MapCore/ApplicationPaths.h"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <iterator>
#include <string_view>

namespace War3Map {
std::filesystem::path ReadEditorExternalDependenciesSetting() {
    // Qt NativeFormat 使用当前用户 Software 下的产品键。
    std::wstring product = L"Software\\";
    product.append(std::begin(kEditorSettingsOrganization), std::end(kEditorSettingsOrganization) - 1);
    product += L'\\';
    product.append(std::begin(kEditorSettingsApplication), std::end(kEditorSettingsApplication) - 1);
    std::wstring key(std::begin(kExternalDependenciesSettingsKey), std::end(kExternalDependenciesSettingsKey) - 1);
    std::replace(key.begin(), key.end(), L'/', L'\\');
    const auto slash = key.rfind(L'\\');
    product += L'\\' + key.substr(0, slash);
    std::array<wchar_t, 4096> value{};
    DWORD bytes = static_cast<DWORD>(sizeof(value));
    if (RegGetValueW(HKEY_CURRENT_USER, product.c_str(), key.c_str() + slash + 1,
        RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    std::wstring_view text(value.data());
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    return std::filesystem::path(text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1));
}

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
