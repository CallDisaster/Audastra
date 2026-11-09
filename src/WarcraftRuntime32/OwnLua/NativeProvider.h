#pragma once

#include "NativeSignature.h"
#include <memory>
#include <span>
#include <variant>

namespace Audastra::Runtime32::OwnLua {

// 生产拥有者持有真实VM代际；agent管理引用，Pool管理已观察到的生命期。
// Ordinal仅保留native返回的序号，不持有游戏对象，也不检测销毁或到期；失效地图不再调用native。
// 原创 fixture 可另作提供者，但不能将其存在当成真实游戏提供者已经实现。
class NativeHandleOwner {
public:
    virtual ~NativeHandleOwner() = default;
    [[nodiscard]] virtual bool Alive() const noexcept = 0;
};

enum class NativeHandleKind { Reference, Scalar, Pool, Ordinal };
[[nodiscard]] inline bool NativeOrdinalType(std::string_view type) noexcept {
    return type == "texttag" || type == "weathereffect";
}
[[nodiscard]] inline bool UsesWeatherEffect(const NativeSignature& signature) noexcept {
    if (signature.returns.kind == NativeKind::Handle && signature.returns.handleType == "weathereffect") return true;
    for (const auto& argument : signature.arguments)
        if (argument.kind == NativeKind::Handle && argument.handleType == "weathereffect") return true;
    return false;
}
inline constexpr char kTextTagNativeOnly[] = "texttag序号只能由本局native返回取得，不能用整数、nil或JASS全局原始值构造。";
[[nodiscard]] inline const char* NativeOrdinalOnlyDiagnostic(std::string_view type) noexcept {
    return type == "weathereffect"
        ? "weathereffect 序号只能由本局 AddWeatherEffect 返回取得，不能用整数、nil 或 JASS 全局原始值构造。"
        : kTextTagNativeOnly;
}
struct NativeHandle {
    std::uint32_t value = 0;
    std::uint64_t generation = 0;
    std::string type;
    std::shared_ptr<NativeHandleOwner> owner;
    // Scalar的枚举常量和Ordinal的序号都可能为0；不能当作agent/null或跨类型合并。
    NativeHandleKind kind = NativeHandleKind::Reference;
};

enum class NativeCallbackReturn { Nothing, Boolean };
struct NativeCallbackResult {
    bool succeeded = false;
    bool booleanValue = false;
    std::string error;
};
// 提供者必须按实际 timer/action/boolexpr 的生命期持有此对象。Lua registry 引用
// 随最后一个持有者释放；宿主关闭后残留的对象仍可查询，但不能重新进入 Lua。
class NativeCallback {
public:
    virtual ~NativeCallback() = default;
    [[nodiscard]] virtual NativeCallbackReturn Returns() const noexcept = 0;
    [[nodiscard]] virtual NativeCallbackResult Invoke() = 0;
};
using NativeValue = std::variant<std::monostate, std::int32_t, float, bool, std::string,
    NativeHandle, std::shared_ptr<NativeCallback>>;

struct NativeCallResult {
    bool succeeded = false;
    NativeValue value;
    std::string error;
};
struct NativeHandleDefinition {
    std::uint32_t reference = 0;
    std::optional<std::string> type;
};

class NativeProvider {
public:
    virtual ~NativeProvider() = default;
    [[nodiscard]] virtual std::uint64_t Generation() const noexcept = 0;
    [[nodiscard]] virtual std::optional<NativeSignature> Lookup(std::string_view name) const = 0;
    // 全局声明来自同一受信脚本来源；没有声明时不能仅凭 VM 中的数值猜类型。
    [[nodiscard]] virtual std::optional<NativeGlobalDeclaration> LookupGlobal(std::string_view) const { return {}; }
    [[nodiscard]] virtual NativeCallResult ReadGlobal(std::string_view) { return {false, {}, "全局读取尚未实现。"}; }
    [[nodiscard]] virtual NativeCallResult WriteGlobal(std::string_view, const NativeValue&) { return {false, {}, "全局写入尚未实现。"}; }
    [[nodiscard]] virtual std::optional<std::uint32_t> HandleMaximum() const { return {}; }
    [[nodiscard]] virtual std::optional<NativeHandleDefinition> HandleDefinition(std::uint32_t) const { return {}; }
    // 只能根据已取得的真实类型父链判断；未知子类型不能猜成 handle 兼容。
    [[nodiscard]] virtual bool IsHandleType(std::string_view actual, std::string_view expected) const noexcept = 0;
    // 原生 C 签名不包含 code 返回类型，必须由经过核验的调用行为提供，未知时拒绝。
    [[nodiscard]] virtual std::optional<NativeCallbackReturn> CallbackReturn(std::string_view, std::size_t) const { return {}; }
    [[nodiscard]] virtual NativeCallResult Invoke(std::string_view name, std::span<const NativeValue> arguments) = 0;
};

} // namespace Audastra::Runtime32::OwnLua
