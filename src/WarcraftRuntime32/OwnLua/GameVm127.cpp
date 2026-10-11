#include "GameVm127.h"
#include "../RuntimeFiles.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <array>
#include <atomic>
#include <cstring>

namespace Audastra::Runtime32::OwnLua {
namespace {
constexpr char kVerifiedGameSha[] = "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a";
constexpr std::uintptr_t kGameGlobal = 0xbe4238;
constexpr std::uint32_t kFirstHandle = 0x100000;
constexpr std::size_t kTextLimit = 1024 * 1024;
struct CurrentIdentity { std::uintptr_t game = 0, state = 0, vm = 0; };
std::uintptr_t Word(std::uintptr_t address) noexcept { return *reinterpret_cast<const std::uint32_t*>(address); }
CurrentIdentity Identity(std::uintptr_t base) noexcept {
    CurrentIdentity current;
    __try {
        current.game = Word(base + kGameGlobal);
        if (!current.game) return {};
        current.state = Word(current.game + 0x1c);
        const auto getVm = reinterpret_cast<std::uintptr_t(__fastcall*)(std::uint32_t)>(base + 0x7e1100);
        current.vm = getVm(static_cast<std::uint32_t>(Word(current.game + 8)));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
    return current;
}
bool VerifiedImage(std::uintptr_t base) noexcept {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 1024 * 1024) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386
            && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && nt->OptionalHeader.SizeOfImage > kGameGlobal + 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct TextSpan { const char* data = nullptr; std::size_t size = 0; };
TextSpan BoundedText(std::uintptr_t object) noexcept {
    __try {
        const auto linked = object ? Word(object + 8) : 0;
        const auto* text = reinterpret_cast<const char*>(linked ? Word(linked + 0x1c) : 0);
        if (!text) return {};
        const auto* end = static_cast<const char*>(std::memchr(text, 0, kTextLimit + 1));
        return end ? TextSpan{text, static_cast<std::size_t>(end - text)} : TextSpan{};
    } __except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
}
bool CopyText(TextSpan input, char* output) noexcept {
    __try { std::memcpy(output, input.data, input.size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

struct GameVm127::Impl {
    Handle file;
    std::uintptr_t base = 0;
    CurrentIdentity identity;
    DWORD thread = 0;
    std::uint64_t generation = 0;
    mutable std::atomic<bool> active{true};
    Impl(HANDLE lease, std::uintptr_t image, CurrentIdentity current, std::uint64_t value)
        : file(lease), base(image), identity(current), thread(GetCurrentThreadId()), generation(value) {}
};
GameVm127::GameVm127(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GameVm127::~GameVm127() = default;
GameVm127::OpenResult GameVm127::OpenAtScriptEntry(HMODULE game, std::uint64_t generation) {
    if (!game || !generation) return {{}, "Game VM 附着需要明确的映像与非零地图代际。"};
    std::array<wchar_t, 4096> path{};
    const auto length = GetModuleFileNameW(game, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {{}, "无法核对 Game.dll 的实际映像来源。"};
    Handle lease(OpenLockedFile(path.data()));
    if (lease.value == INVALID_HANDLE_VALUE || Sha256(lease.value) != kVerifiedGameSha)
        return {{}, "Game.dll 不是已核验的经典 1.27a 映像；拒绝调用内部 ABI。"};
    const auto base = reinterpret_cast<std::uintptr_t>(game);
    if (!VerifiedImage(base)) return {{}, "Game.dll 的已映射 PE 结构无效。"};
    const auto current = Identity(base);
    if (!current.game || !current.state || !current.vm) return {{}, "当前线程没有已初始化的游戏脚本 VM。"};
    auto impl = std::make_unique<Impl>(lease.value, base, current, generation);
    lease.value = INVALID_HANDLE_VALUE;
    return {std::shared_ptr<GameVm127>(new GameVm127(std::move(impl))), {}};
}
bool GameVm127::Alive() const noexcept {
    const auto& self = *impl_;
    if (GetCurrentThreadId() != self.thread || !self.active.load()) return false;
    const auto current = Identity(self.base);
    const bool same = current.game == self.identity.game && current.state == self.identity.state && current.vm == self.identity.vm;
    if (!same) self.active = false; // 一旦变化即失效，即使内存地址稍后被复用也不能恢复。
    return same;
}
std::uint64_t GameVm127::Generation() const noexcept { return Alive() ? impl_->generation : 0; }
void GameVm127::Invalidate() noexcept { impl_->active = false; }
std::uintptr_t GameVm127::Base() const noexcept { return impl_->base; }
std::uintptr_t GameVm127::Vm() const noexcept { return Alive() ? impl_->identity.vm : 0; }

std::optional<std::uint32_t> GameVm127::InternString(std::string_view text) const {
    if (!Alive() || text.size() > kTextLimit || !LuaMailboxUtf8(text)) return {};
    const std::string terminated(text);
    using Intern = std::uint32_t(__thiscall*)(std::uintptr_t, const char*);
    // SEH 隔离保持在不含 C++ 析构对象的 lambda 内，异常后令整个会话过期。
    const auto call = [](std::uintptr_t function, std::uintptr_t vm, const char* value) noexcept -> std::uint32_t {
        __try { return reinterpret_cast<Intern>(function)(vm, value); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    };
    const auto id = call(impl_->base + 0x7f1540, impl_->identity.vm, terminated.c_str());
    if (!id) { impl_->active = false; return {}; }
    return id;
}
std::uintptr_t GameVm127::StringObject(std::uint32_t id) const noexcept {
    if (!Alive()) return 0;
    __try {
        const auto table = Word(impl_->identity.vm + 0x2874);
        if (!table || id >= Word(table + 4)) return 0;
        return Word(table + 8) + 16 * id;
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return 0; }
}
std::optional<std::string> GameVm127::ReadStringObject(std::uintptr_t object) {
    if (!object) return {};
    const auto text = BoundedText(object);
    if (!text.data) return {};
    std::string buffer(text.size, '\0');
    if (!CopyText(text, buffer.data())) return {};
    if (!LuaMailboxUtf8(buffer)) return {};
    return buffer;
}
std::optional<std::string> GameVm127::ReadString(std::uint32_t id) const { return ReadStringObject(StringObject(id)); }
void GameVm127::ReleaseString(std::uint32_t id) const noexcept {
    if (!id || !Alive()) return;
    __try {
        const auto object = StringObject(id);
        if (!object || !Word(object + 0xc)) { impl_->active = false; return; }
        reinterpret_cast<void(__thiscall*)(std::uintptr_t, std::uint32_t)>(impl_->base + 0x7ec9b0)(impl_->identity.vm, id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; }
}
std::uintptr_t GameVm127::HandleObject(std::uint32_t id) const noexcept {
    if (id < kFirstHandle || !Alive()) return 0;
    __try {
        const auto index = id - kFirstHandle;
        if (index >= Word(impl_->identity.state + 0x198) || index > 0x1000000) return 0;
        return Word(Word(impl_->identity.state + 0x19c) + 12 * index + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return 0; }
}
bool GameVm127::HandleAlive(std::uint32_t id, std::uintptr_t object) const noexcept {
    if (!object || HandleObject(id) != object) return false;
    __try {
        const auto weak = reinterpret_cast<std::uintptr_t(__fastcall*)(std::uint32_t, std::uint32_t)>(impl_->base + 0x037350)
            (static_cast<std::uint32_t>(Word(object + 12)), static_cast<std::uint32_t>(Word(object + 16)));
        return weak && Word(weak + 12) == 727803756 && !Word(weak + 32);
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return false; }
}
bool GameVm127::RetainHandle(std::uint32_t id) const noexcept {
    const auto object = HandleObject(id);
    if (!HandleAlive(id, object)) return false;
    __try {
        const auto callback = Word(impl_->identity.vm + 0x28a0);
        const auto context = Word(impl_->identity.vm + 0x28a4);
        if (callback != impl_->base + 0x2698b0 || context != impl_->identity.game + 0x1c) return false;
        reinterpret_cast<void(__fastcall*)(std::uint32_t, std::uint32_t, std::uintptr_t)>(callback)(id, 0, context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return false; }
}
void GameVm127::ReleaseHandle(std::uint32_t id, std::uintptr_t object) const noexcept {
    if (!object || HandleObject(id) != object) return;
    __try {
        const auto callback = Word(impl_->identity.vm + 0x28a0);
        const auto context = Word(impl_->identity.vm + 0x28a4);
        if (callback != impl_->base + 0x2698b0 || context != impl_->identity.game + 0x1c) { impl_->active = false; return; }
        reinterpret_cast<void(__fastcall*)(std::uint32_t, std::uint32_t, std::uintptr_t)>(callback)(id, 1, context);
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; }
}
std::optional<std::uint32_t> GameVm127::RegisterCode(std::uintptr_t token) const noexcept {
    if (!token || !Alive()) return {};
    __try {
        const auto id = reinterpret_cast<std::uint32_t(__thiscall*)(std::uintptr_t, std::uintptr_t)>
            (impl_->base + 0x7ece50)(impl_->identity.vm, token);
        return id ? std::optional<std::uint32_t>(id) : std::nullopt;
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return {}; }
}
void GameVm127::ReleaseCode(std::uint32_t id) const noexcept {
    if (!id || !Alive()) return;
    __try { reinterpret_cast<void(__thiscall*)(std::uintptr_t, std::uint32_t)>(impl_->base + 0x7f03d0)(impl_->identity.vm, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; }
}

} // namespace Audastra::Runtime32::OwnLua
