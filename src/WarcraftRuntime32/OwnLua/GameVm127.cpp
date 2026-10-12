#include "GameVm127.h"
#include "GameImage127.h"
#include "NativeSignature.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"

#include <array>
#include <atomic>
#include <cstring>

namespace Audastra::Runtime32::OwnLua {
namespace {
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
    std::shared_ptr<GameImage127> image;
    std::uintptr_t base = 0;
    CurrentIdentity identity;
    DWORD thread = 0;
    std::uint64_t generation = 0;
    mutable std::atomic<bool> active{true};
    Impl(std::shared_ptr<GameImage127> mapped, CurrentIdentity current, std::uint64_t value)
        : image(std::move(mapped)), base(image->Base()), identity(current), thread(GetCurrentThreadId()), generation(value) {}
};
GameVm127::GameVm127(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GameVm127::~GameVm127() = default;
GameVm127::OpenResult GameVm127::OpenAtScriptEntry(HMODULE game, std::uint64_t generation) {
    if (!game || !generation) return {{}, "Game VM 附着需要明确的映像与非零地图代际。"};
    auto loaded = GameImage127::Open(game);
    if (!loaded.image) return {{}, std::move(loaded.error)};
    return OpenAtScriptEntry(std::move(loaded.image), generation);
}
GameVm127::OpenResult GameVm127::OpenAtScriptEntry(std::shared_ptr<GameImage127> image, std::uint64_t generation) {
    if (!image || !generation) return {{}, "Game VM 附着需要明确的映像与非零地图代际。"};
    const auto current = Identity(image->Base());
    if (!current.game || !current.state || !current.vm) return {{}, "当前线程没有已初始化的游戏脚本 VM。"};
    auto impl = std::make_unique<Impl>(std::move(image), current, generation);
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
bool GameVm127::IsMainVm(std::uintptr_t vm) const noexcept { return vm && vm == impl_->identity.vm && impl_->thread == GetCurrentThreadId(); }

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
std::uintptr_t GameVm127::ResolveCode(std::uint32_t vmIndex, std::uint32_t id) const noexcept {
    if (!id || !Alive()) return 0;
    __try {
        const auto selected = reinterpret_cast<std::uintptr_t(__fastcall*)(std::uint32_t)>(impl_->base + 0x7e1100)(vmIndex);
        if (!selected) return 0;
        const auto table = Word(selected + 0x2888);
        // 1.27a 创建子 VM 时共享 code 表；不接受其它映射碰巧复用的数字 ID。
        if (!table || table != Word(impl_->identity.vm + 0x2888) || id >= Word(table + 4)) return 0;
        return Word(Word(table + 8) + 4 * id);
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return 0; }
}
std::optional<NativeCodeOwner> GameVm127::CodeOwner(std::uint32_t handle) const noexcept {
    const auto object = HandleObject(handle);
    if (!object || !HandleAlive(handle, object)) return {};
    __try { return NativeCodeOwner{object, static_cast<std::uint32_t>(Word(object + 12)), static_cast<std::uint32_t>(Word(object + 16))}; }
    __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return {}; }
}
bool GameVm127::CodeOwnerAlive(const NativeCodeOwner& owner) const noexcept {
    if (!owner.object || !Alive()) return false;
    __try {
        const auto weak = reinterpret_cast<std::uintptr_t(__fastcall*)(std::uint32_t, std::uint32_t)>(impl_->base + 0x037350)
            (owner.tokenA, owner.tokenB);
        // 不解引用可能已释放的旧对象地址；两个弱身份由游戏分配器生成。
        return weak && Word(weak + 12) == 727803756 && !Word(weak + 32);
    } __except (EXCEPTION_EXECUTE_HANDLER) { impl_->active = false; return false; }
}
std::optional<GameVm127::GlobalValue> GameVm127::ReadGlobal(std::string_view name) const {
    if (!Alive() || !NativeIdentifier(name)) return {};
    const std::string terminated(name);
    const auto read = [](std::uintptr_t base, std::uintptr_t vm, const char* key,
        GlobalValue& output, bool& fault) noexcept {
        __try {
            const auto table = Word(vm + 0x285c);
            if (!table) return false;
            const auto record = reinterpret_cast<std::uintptr_t(__thiscall*)(std::uintptr_t, const char*)>
                (base + 0x7e5820)(table, key);
            if (!record) return false;
            output = {static_cast<std::uint32_t>(Word(record + 0x18)),
                static_cast<std::uint32_t>(Word(record + 0x1c)), static_cast<std::uint32_t>(Word(record + 0x20))};
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { fault = true; return false; }
    };
    GlobalValue value; bool fault = false;
    const bool found = read(impl_->base, impl_->identity.vm, terminated.c_str(), value, fault);
    if (fault) impl_->active = false;
    return found && Alive() ? std::optional<GlobalValue>(value) : std::nullopt;
}

} // namespace Audastra::Runtime32::OwnLua
