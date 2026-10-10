#include "War3MapCore/Runtime/RuntimeLuaApiInventory.h"
#include "RuntimeLuaApiPe.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Jass/JassDeclarationIndex.h"
#include "War3MapCore/ProjectSaveControl.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace War3Map::Runtime {
namespace {
using detail::Pe32;
void Require(bool value, std::string_view why) {
    if (!value) throw std::runtime_error(std::string(why));
}
void CheckCancel(std::stop_token cancel, std::chrono::steady_clock::time_point deadline) {
    if (cancel.stop_requested()) throw std::runtime_error("接口清单读取已取消");
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("接口清单读取超过本轮时间预算");
}
bool Name(std::string_view text) {
    return !text.empty() && text.size() <= 128 && std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
    });
}
class Cursor {
public:
    Cursor(const Pe32& input, std::size_t start) : pe(input), at(start) {}
    void Bytes(std::initializer_list<std::uint8_t> values) {
        for (const auto expected : values) Require(pe.Byte(at++) == expected, "模块注册 ABI 编码不匹配");
    }
    std::uint32_t Pointer() {
        Require(pe.HasRelocation(at), "注册关系缺少真实 HIGHLOW 重定位");
        const auto value = pe.Dword(at); at += 4; return value;
    }
    void Import(std::string_view name) {
        Bytes({0xff, 0x15});
        Require(pe.IsImport(Pointer(), "lua53.dll", name), "模块注册未调用实际 Lua import");
    }
    std::uint32_t DirectCall() {
        Bytes({0xe8});
        const auto relative = static_cast<std::int32_t>(pe.Dword(at));
        const auto target = static_cast<std::int64_t>(pe.Rva(at)) + 4 + relative; at += 4;
        Require(target > 0 && target <= UINT32_MAX && pe.Executable(static_cast<std::uint32_t>(target)),
            "模块注册调用目标不是有效代码");
        return static_cast<std::uint32_t>(target);
    }
    const Pe32& pe;
    std::size_t at;
};
bool PreloadHelper(const Pe32& pe, std::size_t at) {
    try {
        Cursor cursor(pe, at);
        cursor.Bytes({0x55, 0x8b, 0xec, 0x56, 0x57, 0x68});
        Require(pe.String(cursor.Pointer()) == "_PRELOAD", "注册 key 不是 _PRELOAD");
        cursor.Bytes({0x8b, 0xf9, 0x8b, 0xf2, 0x68});
        Require(pe.Dword(cursor.at) == 0xfff0b9d8u, "注册表索引不是 Lua 5.3 registry"); cursor.at += 4;
        cursor.Bytes({0x57}); cursor.Import("lua_getfield");
        cursor.Bytes({0x6a, 0, 0xff, 0x75, 8, 0x57}); cursor.Import("lua_pushcclosure");
        cursor.Bytes({0x56, 0x6a, 0xfe, 0x57}); cursor.Import("lua_setfield");
        cursor.Bytes({0x6a, 0xfe, 0x57}); cursor.Import("lua_settop");
        cursor.Bytes({0x83, 0xc4, 0x2c, 0x5f, 0x5e, 0x5d, 0xc3});
        (void)pe.Raw(pe.Rva(at), cursor.at - at);
        return true;
    } catch (const std::exception&) { return false; }
}
std::optional<RuntimeApiModule> Registration(const Pe32& pe, std::size_t at, unsigned variant,
    const std::set<std::uint32_t>& helpers) {
    try {
        Cursor cursor(pe, at);
        std::uint32_t name = 0, function = 0;
        if (variant == 0) {
            cursor.Bytes({0xba}); name = cursor.Pointer();
            cursor.Bytes({0x68}); function = cursor.Pointer();
            cursor.Bytes({0x89, 0x75}); cursor.at += 1;
        } else if (variant == 1) {
            cursor.Bytes({0xba}); name = cursor.Pointer();
            cursor.Bytes({0xc7, 0x04, 0x24}); function = cursor.Pointer();
            cursor.Bytes({0x8b, 0xce});
        } else {
            cursor.Bytes({0x68}); function = cursor.Pointer();
            cursor.Bytes({0xba}); name = cursor.Pointer();
            cursor.Bytes({0x8b, 0xcb});
        }
        Require(helpers.contains(cursor.DirectCall()), "不是已验证 _PRELOAD helper");
        Require(function >= pe.imageBase && pe.Executable(function - pe.imageBase), "模块 open 指针不在代码节区");
        auto label = pe.String(name);
        Require(label.starts_with("jass.") && Name(std::string_view(label).substr(5)), "模块名称无效");
        RuntimeApiModule module;
        module.name = std::move(label);
        module.registrationRva = pe.Rva(at);
        module.openFunctionRva = function - pe.imageBase;
        (void)pe.Raw(module.registrationRva, cursor.at - at);
        return module;
    } catch (const std::exception&) { return {}; }
}
struct Value {
    enum class Kind { Unknown, Constant, Frame, Import } kind = Kind::Unknown;
    std::uint32_t number = 0;
};
std::vector<RuntimeApiField> Table(const Pe32& pe, Value pointer, const std::map<int, Value>& frame,
    std::uint32_t callRva) {
    std::vector<RuntimeApiField> result;
    for (unsigned i = 0; i < 256; ++i) {
        Value label, function;
        if (pointer.kind == Value::Kind::Constant) {
            Require(pointer.number >= pe.imageBase, "luaL_Reg 指针超界");
            const auto at = pe.Raw(pointer.number - pe.imageBase + i * 8, 8);
            label = {Value::Kind::Constant, pe.Dword(at)};
            function = {Value::Kind::Constant, pe.Dword(at + 4)};
            if (label.number || function.number)
                Require(pe.HasRelocation(at) && pe.HasRelocation(at + 4), "luaL_Reg 指针缺少重定位");
        } else if (pointer.kind == Value::Kind::Frame) {
            const auto offset = static_cast<int>(static_cast<std::int32_t>(pointer.number)) + static_cast<int>(i * 8);
            const auto nameIt = frame.find(offset), functionIt = frame.find(offset + 4);
            Require(nameIt != frame.end() && functionIt != frame.end(), "栈 luaL_Reg 不是完整冻结对");
            label = nameIt->second; function = functionIt->second;
        } else throw std::runtime_error("luaL_Reg 指针未知");
        Require(label.kind == Value::Kind::Constant && function.kind == Value::Kind::Constant,
            "luaL_Reg 值未知");
        if (!label.number && !function.number) return result;
        Require(label.number && function.number >= pe.imageBase && pe.Executable(function.number - pe.imageBase),
            "luaL_Reg 函数指针无效");
        auto name = pe.String(label.number);
        Require(Name(name), "luaL_Reg 名称无效");
        result.push_back({std::move(name), {}, false, callRva, function.number - pe.imageBase});
    }
    throw std::runtime_error("luaL_Reg 缺少有界 NULL 终结");
}

// 仅读取 open 入口的已解码直线前缀；遇条件分支即停，不假装执行可达性或完整表枚举。
void Fields(const Pe32& pe, RuntimeApiModule& module) {
    std::array<Value, 8> registers{};
    std::map<int, Value> frame;
    std::vector<Value> pushes;
    std::vector<std::uint32_t> pendingIndices;
    std::optional<std::string> pushedName;
    std::optional<std::uint32_t> pushedFunction;
    auto at = pe.Raw(module.openFunctionRva);
    for (unsigned count = 0; count < 1024; ++count) {
        const auto instruction = detail::Decode32(pe, at);
        const auto op = instruction.opcode;
        if ((op >= 0x70 && op <= 0x7f) || op == 0xe9 || op == 0xeb ||
            (instruction.twoByte && instruction.second >= 0x80 && instruction.second <= 0x8f)) break;
        if (op == 0xc3 || op == 0xc2) break;
        const auto immediate = [&] { return pe.Dword(instruction.immediateAt); };
        bool handled = true;
        if (op >= 0xb8 && op <= 0xbf && instruction.immediateSize == 4)
            registers[op - 0xb8] = {Value::Kind::Constant, immediate()};
        else if ((op == 0x31 || op == 0x33) && (instruction.modrm >> 6) == 3 &&
            (instruction.modrm & 7u) == ((instruction.modrm >> 3) & 7u))
            registers[instruction.modrm & 7u] = {Value::Kind::Constant, 0};
        else if ((op == 0x31 || op == 0x33) && (instruction.modrm >> 6) == 3)
            registers[op == 0x33 ? (instruction.modrm >> 3) & 7u : instruction.modrm & 7u] = {};
        else if (op == 0x8b && (instruction.modrm & 0xc7u) == 5) {
            const auto pointer = pe.Dword(instruction.modrmAt + 1);
            registers[(instruction.modrm >> 3) & 7u] = pe.imports.contains(pointer) ?
                Value{Value::Kind::Import, pointer} : Value{};
        } else if ((op == 0x8b || op == 0x89) && (instruction.modrm >> 6) == 3) {
            const auto destination = op == 0x8b ? (instruction.modrm >> 3) & 7u : instruction.modrm & 7u;
            const auto source = op == 0x8b ? instruction.modrm & 7u : (instruction.modrm >> 3) & 7u;
            registers[destination] = registers[source];
        } else if (op == 0x8b) registers[(instruction.modrm >> 3) & 7u] = {};
        else if (op == 0x8d) {
            const auto destination = (instruction.modrm >> 3) & 7u;
            if ((instruction.modrm & 0xc7u) == 0x45) {
                const auto offset = static_cast<std::int8_t>(pe.Byte(instruction.modrmAt + 1));
                registers[destination] = {Value::Kind::Frame, static_cast<std::uint32_t>(static_cast<std::int32_t>(offset))};
            } else registers[destination] = {};
        } else if (op == 0xc7 && instruction.modrm == 0x45 && instruction.immediateSize == 4) {
            const auto offset = static_cast<std::int8_t>(pe.Byte(instruction.modrmAt + 1));
            const auto value = immediate();
            if (value) Require(pe.HasRelocation(instruction.immediateAt), "栈注册指针缺少重定位");
            frame[offset] = {Value::Kind::Constant, value};
        } else if (op == 0x89 && (instruction.modrm & 0xc7u) == 0x45) {
            const auto offset = static_cast<std::int8_t>(pe.Byte(instruction.modrmAt + 1));
            frame[offset] = registers[(instruction.modrm >> 3) & 7u];
        } else if (op == 0x83 && instruction.modrm == 0x65 && pe.Byte(instruction.immediateAt) == 0) {
            frame[static_cast<std::int8_t>(pe.Byte(instruction.modrmAt + 1))] = {Value::Kind::Constant, 0};
        } else if (op == 0x68) pushes.push_back({Value::Kind::Constant, immediate()});
        else if (op == 0x6a) pushes.push_back({Value::Kind::Constant,
            static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(pe.Byte(instruction.immediateAt))))});
        else if (op >= 0x50 && op <= 0x57) pushes.push_back(registers[op - 0x50]);
        else if (op >= 0x58 && op <= 0x5f) {
            registers[op - 0x58] = {}; if (!pushes.empty()) pushes.pop_back();
        } else if (op == 0xff && ((instruction.modrm >> 3) & 7u) == 6) pushes.push_back({});
        else if (op == 0xa1) registers[0] = {};
        else if (op == 0x83 && (instruction.modrm == 0xc4 || instruction.modrm == 0xe4 || instruction.modrm == 0xec)) {}
        else if (op == 0xe8 || (op == 0xff && ((instruction.modrm >> 3) & 7u) == 2)) {}
        else handled = false;

        if (!handled) break; // 未覆盖的写入/副作用不保留过期抽象值，不产生猜测字段。

        std::optional<detail::PeImport> imported;
        const bool indirectCall = op == 0xff && ((instruction.modrm >> 3) & 7u) == 2;
        if (indirectCall) {
            std::uint32_t pointer = 0;
            if (instruction.modrm == 0x15) pointer = pe.Dword(instruction.modrmAt + 1);
            else if ((instruction.modrm >> 6) == 3 && registers[instruction.modrm & 7u].kind == Value::Kind::Import)
                pointer = registers[instruction.modrm & 7u].number;
            const auto found = pe.imports.find(pointer);
            if (found != pe.imports.end()) imported = found->second;
        }
        if (imported && imported->library == "lua53.dll") {
            const auto& name = imported->name;
            if (name == "luaL_setfuncs" && pushes.size() >= 3) {
                try {
                    auto fields = Table(pe, pushes[pushes.size() - 2], frame, pe.Rva(at));
                    for (auto& field : fields) if (!field.name.starts_with("__"))
                        module.fields.push_back(std::move(field));
                } catch (const std::exception&) { /* 部分/未知表不当成字段缺失。 */ }
            }
            if (name == "lua_pushstring" && pushes.size() >= 2) {
                const auto value = pushes[pushes.size() - 2];
                if (value.kind == Value::Kind::Constant) {
                    try { pushedName = pe.String(value.number); } catch (const std::exception&) { pushedName.reset(); }
                } else pushedName.reset();
            } else if (name == "lua_pushcclosure" && pushes.size() >= 3) {
                const auto function = pushes[pushes.size() - 2];
                if (function.kind == Value::Kind::Constant && function.number >= pe.imageBase &&
                    pe.Executable(function.number - pe.imageBase)) pushedFunction = function.number - pe.imageBase;
                else pushedFunction.reset();
            } else if (name == "lua_rawset" && pushedName && pushedFunction) {
                if (*pushedName == "__index") pendingIndices.push_back(*pushedFunction);
                else if (Name(*pushedName) && !pushedName->starts_with("__"))
                    module.fields.push_back({*pushedName, {}, false, pe.Rva(at), *pushedFunction});
                pushedName.reset(); pushedFunction.reset();
            }
        }
        if (indirectCall || op == 0xe8) {
            registers[0] = {}; registers[1] = {}; registers[2] = {}; pushes.clear();
            if (!imported || imported->library != "lua53.dll") frame.clear();
        }
        at += instruction.size;
    }
    if (module.name == "jass.common") {
        for (const auto function : pendingIndices) {
            bool nativeLookup = false, validated = false, closure = false;
            auto raw = pe.Raw(function);
            for (unsigned i = 0; i < 256; ++i) {
                const auto instruction = detail::Decode32(pe, raw);
                if (instruction.opcode == 0xc3 || instruction.opcode == 0xc2) break;
                if (instruction.opcode == 0xff && instruction.modrm == 0x15) {
                    const auto found = pe.imports.find(pe.Dword(instruction.modrmAt + 1));
                    if (found != pe.imports.end()) {
                        const auto& item = found->second;
                        if (item.library == "ydwar3.dll" && item.name.starts_with("?jass_func@jass@")) nativeLookup = true;
                        if (item.library == "ydwar3.dll" && item.name.starts_with("?is_valid@func_value@jass@")) validated = true;
                        if (item.library == "lua53.dll" && item.name == "lua_pushcclosure") closure = true;
                    }
                }
                raw += instruction.size;
            }
            module.commonLookup = nativeLookup && validated && closure;
        }
    }
    std::sort(module.fields.begin(), module.fields.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    module.fields.erase(std::unique(module.fields.begin(), module.fields.end(), [](const auto& a, const auto& b) {
        return a.name == b.name;
    }), module.fields.end());
}
} // namespace

RuntimeApiInventory InspectRuntimeLuaEngine(const std::vector<std::byte>& bytes, std::stop_token cancel,
    std::chrono::steady_clock::time_point deadline) {
    RuntimeApiInventory result;
    try {
        CheckCancel(cancel, deadline);
        const Pe32 pe(bytes, cancel, deadline);
        const auto initialize = pe.exports.find("Initialize");
        Require(initialize != pe.exports.end() && pe.Executable(initialize->second), "引擎没有实际 Initialize 代码导出");
        std::set<std::uint32_t> helpers;
        const auto evidence = [&](std::string kind, std::uint32_t rva, std::uint32_t size) {
            const auto sha = Sha256(std::span(bytes).subspan(pe.Raw(rva, size), size));
            Require(sha.Succeeded(), "代码注册证据 SHA 计算失败");
            result.evidence.push_back({std::move(kind), rva, size, *sha.value});
        };
        for (const auto& section : pe.sections) {
            if (!(section.flags & 0x20000000u)) continue;
            for (std::size_t at = section.raw; at + 64 <= static_cast<std::uint64_t>(section.raw) + section.size; ++at) {
                if ((at & 0xfffu) == 0) CheckCancel(cancel, deadline);
                if (pe.Byte(at) == 0x55 && PreloadHelper(pe, at)) {
                    helpers.insert(pe.Rva(at)); evidence("lua-registry-_PRELOAD-C-helper", pe.Rva(at), 64);
                }
            }
        }
        Require(!helpers.empty(), "本机运行库的 _PRELOAD 注册 ABI 尚不能静态证明");
        std::set<std::string> modules;
        for (const auto& section : pe.sections) {
            if (!(section.flags & 0x20000000u)) continue;
            for (std::size_t at = section.raw; at + 20 <= static_cast<std::uint64_t>(section.raw) + section.size; ++at) {
                if ((at & 0xfffu) == 0) CheckCancel(cancel, deadline);
                if (pe.Byte(at) != 0xba && pe.Byte(at) != 0x68) continue;
                for (unsigned variant = 0; variant < 3; ++variant) {
                    auto module = Registration(pe, at, variant, helpers);
                    if (!module) continue;
                    Require(modules.insert(module->name).second, "运行库中模块注册关系有歧义");
                    evidence("preload-registration:" + module->name, module->registrationRva,
                        variant == 0 ? 18u : variant == 1 ? 19u : 17u);
                    try { Fields(pe, *module); } catch (const std::exception&) { /* 模块成立，字段保持部分。 */ }
                    result.modules.push_back(std::move(*module));
                }
            }
        }
        Require(!result.modules.empty(), "没有静态证明任何 jass 模块注册");
        std::sort(result.modules.begin(), result.modules.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        result.trusted = true;
        result.reason = "已核对所选本机运行库的注册代码与模块表关系；未验证 Initialize 可达性、模块实际运行或游戏加载；未枚举字段和模块仍按未知处理。";
    } catch (const std::exception& error) {
        result.trusted = false; result.modules.clear(); result.evidence.clear(); result.cancelled = cancel.stop_requested();
        result.reason = std::string("目前只检查语法：") + error.what();
    }
    return result;
}

std::shared_ptr<const RuntimeApiInventory> CaptureRuntimeLuaApiInventory(const std::filesystem::path& root,
    const std::filesystem::path& commonInput, std::stop_token cancel, std::chrono::steady_clock::time_point deadline) {
    auto result = std::make_shared<RuntimeApiInventory>();
    try {
        Require(!root.empty(), "未设置本机 YDWE 目录");
        static constexpr std::array<std::string_view, 6> paths{
            "bin/ydbase.dll", "bin/lua53.dll", "bin/ydwar3.dll", "plugin/warcraft3/yd_loader.dll",
            "plugin/warcraft3/yd_jass_api.dll", "plugin/warcraft3/yd_lua_engine.dll"};
        std::vector<std::vector<std::byte>> binaries;
        const ProjectSaveControl ioControl([&](ProjectSaveStage, std::uint64_t) { CheckCancel(cancel, deadline); });
        std::string identity = "audastra.lua-runtime-api.static.v1\n";
        for (const auto relative : paths) {
            CheckCancel(cancel, deadline);
            const auto path = root / std::filesystem::path(relative);
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(path, sizeError);
            Require(!sizeError && size && size <= 32u * 1024u * 1024u,
                "缺少本机运行库或文件超过 32 MiB 预算");
            const auto sha = Sha256File(path, &ioControl);
            Require(sha.Succeeded(), "缺少本机运行库或无法读取 SHA");
            auto file = ReadVerifiedFile(path, *sha.value, 32u * 1024u * 1024u, &ioControl);
            Require(file.Succeeded(), "本机运行库在读取期间变化或超过预算");
            result->sources.push_back({std::string(relative), *sha.value, file.value->size()});
            identity += std::string(relative) + "=" + *sha.value + "\n";
            binaries.push_back(std::move(*file.value));
        }
        CheckCancel(cancel, deadline);
        const auto common = commonInput.empty() ? root / "compiler/jass/24/common.j" : commonInput;
        std::error_code commonSizeError;
        const auto commonSize = std::filesystem::file_size(common, commonSizeError);
        Require(!commonSizeError && commonSize && commonSize <= 16u * 1024u * 1024u,
            "缺少实际 common.j 或超过 16 MiB 预算");
        const auto commonSha = Sha256File(common, &ioControl);
        Require(commonSha.Succeeded(), "缺少实际 common.j，不能绑定 native 签名");
        auto commonBytes = ReadVerifiedFile(common, *commonSha.value, 16u * 1024u * 1024u, &ioControl);
        Require(commonBytes.Succeeded(), "common.j 在读取期间变化或超过预算");
        result->sources.push_back({"common.j", *commonSha.value, commonBytes.value->size()});
        identity += "common.j=" + *commonSha.value + "\n";
        const auto identitySha = Sha256Text(identity);
        Require(identitySha.Succeeded(), "清单身份计算失败");
        result->binding = *identitySha.value;
        static std::mutex cacheMutex;
        static std::shared_ptr<const RuntimeApiInventory> cache;
        {
            const std::lock_guard lock(cacheMutex);
            if (cache && cache->trusted && cache->binding == result->binding) {
                CheckCancel(cancel, deadline); return cache;
            }
        }
        // 所有实际DLL解析export，交叉核引擎对这六库的imports，不加载代码。
        std::vector<Pe32> images;
        for (std::size_t i = 0; i < binaries.size(); ++i) {
            CheckCancel(cancel, deadline);
            try { images.emplace_back(binaries[i], cancel, deadline); }
            catch (const std::exception& error) {
                throw std::runtime_error(std::string(paths[i]) + "：" + error.what());
            }
        }
        for (const auto& image : images) for (const auto& [address, imported] : image.imports) {
            (void)address;
            for (std::size_t provider = 0; provider < paths.size(); ++provider) {
                const auto basename = std::filesystem::path(paths[provider]).filename().string();
                if (imported.library == basename) {
                    const auto supplied = images[provider].exports.find(imported.name);
                    Require(supplied != images[provider].exports.end(), "运行库 import 与本机 provider export 不相符");
                    const bool nativeBridge = imported.library == "ydwar3.dll" &&
                        (imported.name.starts_with("?jass_func@jass@") || imported.name.starts_with("?is_valid@func_value@jass@"));
                    const bool luaApi = imported.library == "lua53.dll" &&
                        (imported.name.starts_with("lua_") || imported.name.starts_with("luaL_"));
                    if (nativeBridge || luaApi) Require(images[provider].Executable(supplied->second),
                        "实际 Lua CAPI/native 查找 import 的 provider 不是有文件字节的代码导出");
                }
            }
        }
        const auto engine = InspectRuntimeLuaEngine(binaries.back(), cancel, deadline);
        Require(engine.trusted, engine.reason);
        result->modules = engine.modules;
        result->evidence = engine.evidence;
        const std::string text(reinterpret_cast<const char*>(commonBytes.value->data()), commonBytes.value->size());
        const Jass::JassSource source{"common.j", text};
        Jass::JassCheckOptions declarationOptions;
        declarationOptions.cancel = cancel;
        declarationOptions.deadline = deadline;
        const auto declarations = Jass::IndexJassDeclarations(std::span(&source, 1), declarationOptions);
        Require(declarations.completed, "实际 common.j 声明检查不完整");
        auto commonModule = std::find_if(result->modules.begin(), result->modules.end(), [](const auto& module) {
            return module.name == "jass.common";
        });
        Require(commonModule != result->modules.end() && commonModule->commonLookup,
            "jass.common 的 C __index 到本机 jass_func bridge 未静态证明");
        for (const auto& symbol : declarations.symbols) {
            if (symbol.kind == Jass::DeclarationKind::Native)
                commonModule->fields.push_back({symbol.name, static_cast<std::uint32_t>(symbol.parameters.size()),
                    true, commonModule->registrationRva, 0});
            else if (symbol.kind == Jass::DeclarationKind::Global) result->commonGlobals.push_back(symbol.name);
        }
        CheckCancel(cancel, deadline);
        result->trusted = true;
        result->reason = "已核对所选本机运行库的注册代码与模块表关系及 common.j native 声明契约；未验证 Initialize 可达性、模块实际运行或游戏加载；未枚举模块/惰性字段仍未知；仅供提示，不授予整图保存或运行资格。";
        {
            const std::lock_guard lock(cacheMutex); cache = result;
        }
    } catch (const std::exception& error) {
        result->trusted = false; result->modules.clear(); result->evidence.clear(); result->commonGlobals.clear();
        result->cancelled = cancel.stop_requested();
        result->reason = std::string("目前只检查语法：") + error.what();
    }
    return result;
}
} // namespace War3Map::Runtime
