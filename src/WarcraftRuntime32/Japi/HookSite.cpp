#include "HookSite.h"
#include <detours.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace Audastra::Runtime32::Japi {
namespace {
bool Copy(std::uintptr_t address, void* output, std::size_t count) noexcept {
    __try { std::memcpy(output, reinterpret_cast<const void*>(address), count); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::uintptr_t Jump(std::uintptr_t address, const unsigned char* code) noexcept {
    std::int32_t relative = 0; std::memcpy(&relative, code + 1, 4);
    return static_cast<std::uint32_t>(address + 5 + relative); // PE32 rel32按32位环绕。
}
bool Executable(HMODULE module, std::uintptr_t address, std::size_t count) noexcept {
    __try {
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!module || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 1024 * 1024) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->FileHeader.NumberOfSections > 96
            || nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32)
            || address < base || address - base >= nt->OptionalHeader.SizeOfImage
            || count > nt->OptionalHeader.SizeOfImage - (address - base)) return false;
        bool code = false;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& s = sections[i]; const auto rva = address - base;
            if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= s.VirtualAddress && rva - s.VirtualAddress < s.Misc.VirtualSize
                && count <= s.Misc.VirtualSize - (rva - s.VirtualAddress)) code = true;
        }
        MEMORY_BASIC_INFORMATION page{};
        if (!code || !VirtualQuery(reinterpret_cast<const void*>(address), &page, sizeof(page))
            || page.AllocationBase != module || page.State != MEM_COMMIT || page.Type != MEM_IMAGE
            || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            || count > page.RegionSize - (address - reinterpret_cast<std::uintptr_t>(page.BaseAddress))) return false;
        const auto access = page.Protect & 0xff;
        return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ || access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::shared_ptr<void> Retain(std::uintptr_t address, std::size_t count) {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), &owner)) return {};
    std::shared_ptr<void> lease(owner, [](void* p) { FreeLibrary(static_cast<HMODULE>(p)); });
    return Executable(owner, address, count) ? lease : std::shared_ptr<void>{};
}
std::string Owner(std::uintptr_t address) {
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &owner)) return "<not-loaded-image>";
    std::array<wchar_t, 32768> path{};
    const auto n = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) return "<unnamed-image>";
    const auto begin = std::wstring_view(path.data(), n).find_last_of(L"\\/");
    const auto file = std::wstring_view(path.data(), n).substr(begin == std::wstring_view::npos ? 0 : begin + 1);
    std::string name(file.size() * 4, 0);
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, file.data(), static_cast<int>(file.size()), name.data(), static_cast<int>(name.size()), nullptr, nullptr);
    if (bytes <= 0) return "<invalid-name>"; name.resize(bytes);
    char offset[32]{}; std::snprintf(offset, sizeof(offset), "+0x%X", static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(owner)));
    return name + offset;
}
}
HookSite::HookSite(std::uintptr_t address, std::span<const unsigned char> original, bool allow) : address_(address) {
    owners_.reserve(9);
    if (original.size() < 16 || original.size() > 32) { reason_ = "invalid_original_window"; return; }
    auto owner = Retain(address, original.size());
    if (!owner) { reason_ = "site_not_executable_image"; return; }
    const auto rootOwner = owner.get(); owners_.push_back(std::move(owner));
    auto& root = snapshots_[count_++]; root.address = address; root.count = original.size();
    if (!Copy(address, root.bytes.data(), root.count)) { reason_ = "unreadable_site"; return; }
    if (std::equal(original.begin(), original.end(), root.bytes.begin())) { approved_ = true; reason_ = "original"; return; }
    if (!allow || original[0] != 0xe8 || root.bytes[0] != 0xe9
        || !std::equal(original.begin() + 5, original.end(), root.bytes.begin() + 5)) { reason_ = "unrecognized_site_bytes"; return; }
    auto target = Jump(address, root.bytes.data());
    for (;;) {
        if (count_ == snapshots_.size()) { reason_ = "chain_depth_exceeded"; return; }
        for (std::size_t i = 0; i < count_; ++i) if (snapshots_[i].address == target) { reason_ = "chain_cycle"; return; }
        auto targetOwner = Retain(target, 16);
        if (!targetOwner || targetOwner.get() == rootOwner) { reason_ = "target_not_external_executable_image"; return; }
        owners_.push_back(std::move(targetOwner));
        auto& next = snapshots_[count_++]; next.address = target; next.count = 16;
        if (!Copy(target, next.bytes.data(), next.count)) { reason_ = "unreadable_target"; return; }
        if (next.bytes[0] == 0xe9) { target = Jump(target, next.bytes.data()); continue; }
        if (next.bytes[0] == 0xeb || next.bytes[0] == 0xea || (next.bytes[0] == 0xff && (next.bytes[1] & 0x30) == 0x20)) {
            reason_ = "unrecognized_target_jump"; return;
        }
        approved_ = chained_ = true; reason_ = "verified_rel32_chain"; return;
    }
}
bool HookSite::StillMatches() const noexcept {
    if (!approved_) return false;
    std::array<unsigned char, 32> current{};
    for (std::size_t i = 0; i < count_; ++i)
        if (!Copy(snapshots_[i].address, current.data(), snapshots_[i].count)
            || !std::equal(current.begin(), current.begin() + snapshots_[i].count, snapshots_[i].bytes.begin())) return false;
    return true;
}
LONG AttachQualifiedHook(const HookSite& site, PVOID* next, PVOID callback) noexcept {
    if (!site.Approved() || !site.StillMatches() || !next || *next != reinterpret_cast<void*>(site.Address())) return ERROR_INVALID_DATA;
    PVOID realTarget = nullptr;
    const auto status = DetourAttachEx(next, callback, nullptr, &realTarget, nullptr);
    return status != NO_ERROR ? status : realTarget == reinterpret_cast<void*>(site.Address()) ? NO_ERROR : ERROR_INVALID_DATA;
}
bool HookIsHead(std::uintptr_t site, const void* callback) noexcept {
    std::array<unsigned char, 5> bytes{};
    return Copy(site, bytes.data(), bytes.size()) && bytes[0] == 0xe9 && Jump(site, bytes.data()) == reinterpret_cast<std::uintptr_t>(callback);
}
std::string DescribeHookSite(std::uintptr_t site) {
    std::array<unsigned char, 32> code{};
    std::size_t count = code.size();
    if (!Copy(site, code.data(), count)) { count = 16; if (!Copy(site, code.data(), count)) return "owner=" + Owner(site) + " bytes=<unreadable>"; }
    std::string hex; hex.reserve(64);
    for (std::size_t i = 0; i < count; ++i) { const auto c = code[i]; hex += "0123456789abcdef"[c >> 4]; hex += "0123456789abcdef"[c & 15]; }
    auto text = "owner=" + Owner(site) + " bytes=" + hex;
    if (code[0] == 0xe9) text += " jump_owner=" + Owner(Jump(site, code.data()));
    return text;
}
}
