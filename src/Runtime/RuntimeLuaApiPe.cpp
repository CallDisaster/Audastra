#include "RuntimeLuaApiPe.h"

#include <algorithm>
#include <limits>

namespace War3Map::Runtime::detail {
namespace {
void Require(bool condition, std::string_view reason) {
    if (!condition) throw std::runtime_error(std::string(reason));
}
}
std::uint8_t Pe32::Byte(std::size_t at) const {
    Require(at < bytes.size(), "PE 文件截断");
    return std::to_integer<std::uint8_t>(bytes[at]);
}
std::uint16_t Pe32::Word(std::size_t at) const {
    return static_cast<std::uint16_t>(Byte(at) | (Byte(at + 1) << 8));
}
std::uint32_t Pe32::Dword(std::size_t at) const {
    return Word(at) | (static_cast<std::uint32_t>(Word(at + 2)) << 16);
}
std::size_t Pe32::Raw(std::uint32_t rva, std::size_t count) const {
    std::optional<std::size_t> result;
    for (const auto& section : sections) {
        if (rva >= section.rva && static_cast<std::uint64_t>(rva) + count <=
            static_cast<std::uint64_t>(section.rva) + section.size) {
            Require(!result, "PE RVA 映射存在歧义");
            result = section.raw + static_cast<std::size_t>(rva - section.rva);
        }
    }
    if (!result) throw std::runtime_error("PE RVA 不在实际文件成员中（RVA=" + std::to_string(rva)
        + "，读取长度=" + std::to_string(count) + "）");
    return *result;
}
std::uint32_t Pe32::Rva(std::size_t raw) const {
    for (const auto& section : sections)
        if (raw >= section.raw && raw < static_cast<std::uint64_t>(section.raw) + section.size)
            return section.rva + static_cast<std::uint32_t>(raw - section.raw);
    throw std::runtime_error("PE 文件位置没有 RVA");
}
bool Pe32::Executable(std::uint32_t rva) const {
    (void)Raw(rva);
    for (const auto& section : sections)
        if (rva >= section.rva && static_cast<std::uint64_t>(rva) <
            static_cast<std::uint64_t>(section.rva) + section.size) return (section.flags & 0x20000000u) != 0;
    return false;
}
bool Pe32::ImageMapped(std::uint32_t rva, std::size_t count) const {
    if (static_cast<std::uint64_t>(rva) + count > imageSize) return false;
    unsigned matches = 0;
    for (const auto& section : sections) {
        const auto extent = (std::max)(section.size, section.virtualSize);
        if (rva >= section.rva && static_cast<std::uint64_t>(rva) + count <=
            static_cast<std::uint64_t>(section.rva) + extent) ++matches;
    }
    return matches == 1;
}
std::string Pe32::String(std::uint32_t va, std::size_t limit) const {
    Poll();
    Require(va >= imageBase, "PE 字符串指针超界");
    const auto rva = va - imageBase;
    const auto raw = Raw(rva);
    std::string result;
    for (std::size_t i = 0; i < limit; ++i) {
        (void)Raw(rva, i + 1);
        const auto ch = Byte(raw + i);
        if (!ch) {
            metadataBytes_ += result.size() + 1;
            Require(metadataBytes_ <= 4u * 1024u * 1024u, "PE 元数据名称超过 4 MiB 预算");
            return result;
        }
        Require(ch >= 32 && ch < 127, "PE 名称不是 ASCII 元数据");
        result += static_cast<char>(ch);
    }
    throw std::runtime_error("PE 名称没有有界结束符");
}
bool Pe32::IsImport(std::uint32_t va, std::string_view library, std::string_view name) const {
    const auto found = imports.find(va);
    return found != imports.end() && found->second.library == library && found->second.name == name;
}
bool Pe32::HasRelocation(std::size_t raw) const { return relocations.contains(Rva(raw)); }

void Pe32::Poll() const {
    Require(!cancel_.stop_requested(), "接口清单解析已取消");
    Require(std::chrono::steady_clock::now() < deadline_, "接口清单解析超过本轮时间预算");
}
Pe32::Pe32(const std::vector<std::byte>& data, std::stop_token cancel,
    std::chrono::steady_clock::time_point deadline) : bytes(data), cancel_(cancel), deadline_(deadline) {
    Poll();
    Require(bytes.size() <= 32u * 1024u * 1024u && bytes.size() >= 256, "PE 文件读取预算或最小大小无效");
    Require(Word(0) == 0x5a4d, "不是 MZ 文件");
    const auto pe = Dword(0x3c);
    Require(Dword(pe) == 0x4550 && Word(pe + 4) == 0x14c, "要求 i386 PE 文件");
    const auto optional = static_cast<std::size_t>(pe) + 24;
    const auto optionalSize = Word(pe + 20);
    Require(optionalSize >= 224 && Word(optional) == 0x10b && Dword(optional + 92) >= 16, "要求完整 PE32 数据目录");
    imageBase = Dword(optional + 28);
    imageSize = Dword(optional + 56);
    Require(imageSize && imageSize <= 256u * 1024u * 1024u, "PE 虚拟映像大小超出预算");
    const auto count = Word(pe + 6);
    Require(count && count <= 96, "PE 节区数超出预算");
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = optional + optionalSize + i * 40;
        const PeSection section{Dword(at + 12), Dword(at + 16), Dword(at + 20), Dword(at + 36), Dword(at + 8)};
        Require(static_cast<std::uint64_t>(section.raw) + section.size <= bytes.size(), "PE 节区截断");
        const auto extent = (std::max)(section.size, section.virtualSize);
        Require(static_cast<std::uint64_t>(section.rva) + extent <= imageSize, "PE 节区超过实际映像大小");
        for (const auto& prior : sections) {
            const auto priorExtent = (std::max)(prior.size, prior.virtualSize);
            Require(static_cast<std::uint64_t>(section.rva) + extent <= prior.rva ||
                static_cast<std::uint64_t>(prior.rva) + priorExtent <= section.rva, "PE RVA 节区重叠");
            Require(static_cast<std::uint64_t>(section.raw) + section.size <= prior.raw ||
                static_cast<std::uint64_t>(prior.raw) + prior.size <= section.raw, "PE 文件节区重叠");
        }
        sections.push_back(section);
    }
    const auto importRva = Dword(optional + 104), importSize = Dword(optional + 108);
    if (importRva) {
        Require(importSize >= 20 && importSize <= 128u * 20u, "PE import 描述超出预算");
        bool terminated = false;
        for (std::size_t i = 0; i < importSize / 20; ++i) {
            Poll();
            const auto at = Raw(importRva + static_cast<std::uint32_t>(i * 20), 20);
            const auto original = Dword(at), name = Dword(at + 12), target = Dword(at + 16);
            if (!original && !name && !target) { terminated = true; break; }
            auto library = String(imageBase + name);
            std::transform(library.begin(), library.end(), library.begin(), [](char ch) {
                return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
            });
            bool tableEnded = false;
            for (std::size_t j = 0; j < 4096; ++j) {
                if ((j & 63u) == 0) Poll();
                const auto thunkRva = (original ? original : target) + static_cast<std::uint32_t>(j * 4);
                const auto value = Dword(Raw(thunkRva, 4));
                (void)Raw(target + static_cast<std::uint32_t>(j * 4), 4);
                if (!value) { tableEnded = true; break; }
                if (value & 0x80000000u) continue;
                const auto address = imageBase + target + static_cast<std::uint32_t>(j * 4);
                Require(imports.emplace(address, PeImport{library, String(imageBase + value + 2)}).second,
                    "PE IAT 地址重复");
                Require(imports.size() <= 16384, "PE import 名称总数超过预算");
            }
            Require(tableEnded, "PE import thunk 没有有界结束符");
        }
        Require(terminated, "PE import 描述没有结束符");
    }
    const auto exportRva = Dword(optional + 96), exportSize = Dword(optional + 100);
    if (exportRva) {
        const auto at = Raw(exportRva, 40);
        const auto functions = Dword(at + 20), names = Dword(at + 24);
        Require(names <= 4096 && functions <= 4096, "PE export 数超出预算");
        for (std::size_t i = 0; i < names; ++i) {
            if ((i & 63u) == 0) Poll();
            const auto nameRva = Dword(Raw(Dword(at + 32) + static_cast<std::uint32_t>(i * 4), 4));
            const auto ordinal = Word(Raw(Dword(at + 36) + static_cast<std::uint32_t>(i * 2), 2));
            Require(ordinal < functions, "PE export ordinal 无效");
            const auto function = Dword(Raw(Dword(at + 28) + ordinal * 4, 4));
            Require(function && !(function >= exportRva && static_cast<std::uint64_t>(function) <
                static_cast<std::uint64_t>(exportRva) + exportSize), "PE forward export 尚不支持");
            // DATA exports 可合法指向加载器清零的虚拟尾部（没有文件字节）。
            // 仅承认它是映像内导出，不读取/捏造虚拟内容；代码和名称仍必须来自 raw。
            Require(ImageMapped(function), "PE export 目标不在实际映像节区中");
            for (const auto& section : sections) {
                if (function >= section.rva && static_cast<std::uint64_t>(function) <
                    static_cast<std::uint64_t>(section.rva) + (std::max)(section.size, section.virtualSize)
                    && (section.flags & 0x20000000u)) (void)Raw(function);
            }
            Require(exports.emplace(String(imageBase + nameRva), function).second, "PE export 名称重复");
        }
    }
    const auto relocationRva = Dword(optional + 136), relocationSize = Dword(optional + 140);
    Require(relocationRva && relocationSize <= 4u * 1024u * 1024u, "PE 缺少有界重定位表");
    std::size_t used = 0;
    while (used < relocationSize) {
        Poll();
        const auto at = Raw(relocationRva + static_cast<std::uint32_t>(used), 8);
        const auto page = Dword(at), size = Dword(at + 4);
        Require(size >= 8 && size % 2 == 0 && size <= relocationSize - used, "PE relocation 块无效");
        (void)Raw(relocationRva + static_cast<std::uint32_t>(used), size);
        for (std::size_t i = 8; i < size; i += 2) {
            if ((i & 4095u) == 0) Poll();
            const auto value = Word(at + i);
            const auto type = value >> 12;
            if (!type) continue;
            Require(type == 3, "PE relocation 非 HIGHLOW");
            const auto target = page + (value & 0xfffu);
            (void)Raw(target, 4);
            Require(relocations.insert(target).second, "PE relocation 重复");
        }
        used += size;
    }
}

Instruction Decode32(const Pe32& pe, std::size_t at) {
    Instruction out; out.raw = at;
    auto cursor = at;
    bool operand16 = false;
    for (unsigned i = 0; i < 8; ++i) {
        const auto byte = pe.Byte(cursor);
        if (byte == 0x66) { operand16 = true; ++cursor; continue; }
        if (byte == 0x2e || byte == 0x3e || byte == 0x36 || byte == 0x26 || byte == 0x64 || byte == 0x65 ||
            byte == 0xf0 || byte == 0xf2 || byte == 0xf3) { ++cursor; continue; }
        break;
    }
    const auto operand = operand16 ? 2u : 4u;
    out.opcode = pe.Byte(cursor++);
    bool modrm = false;
    std::size_t immediate = 0;
    const auto op = out.opcode;
    if (op == 0x0f) {
        out.twoByte = true; out.second = pe.Byte(cursor++);
        const auto second = out.second;
        if (second >= 0x80 && second <= 0x8f) immediate = operand;
        else if ((second >= 0xc8 && second <= 0xcf) || second == 0x31 || second == 0xa0 || second == 0xa1 ||
            second == 0xa8 || second == 0xa9) {}
        else if ((second >= 0x10 && second <= 0x2f) || (second >= 0x40 && second <= 0x76) ||
            (second >= 0x90 && second <= 0x9f) || (second >= 0xb0 && second <= 0xbf) || second == 0xa3 ||
            second == 0xab || second == 0xaf || second == 0xc0 || second == 0xc1 || second == 0xc7) modrm = true;
        else if (second == 0xba || second == 0xa4 || second == 0xac || second == 0xc2 || second == 0xc4 ||
            second == 0xc5 || second == 0xc6) { modrm = true; immediate = 1; }
        else throw std::runtime_error("x86 指令不在静态解析档位内");
    } else if (op <= 0x3f && (op & 7u) <= 3u) modrm = true;
    else if (op <= 0x3f && (op & 7u) == 4u) immediate = 1;
    else if (op <= 0x3f && (op & 7u) == 5u) immediate = operand;
    else if ((op >= 0x40 && op <= 0x61) || (op >= 0x90 && op <= 0x9f) ||
        (op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf) || op == 0xc3 || op == 0xc9 || op == 0xcc ||
        op == 0xf4 || op == 0xf5 || (op >= 0xf8 && op <= 0xfd)) {}
    else if (op == 0x68 || (op >= 0xb8 && op <= 0xbf) || op == 0xe8 || op == 0xe9 || op == 0xa9) immediate = operand;
    else if (op == 0x6a || (op >= 0xb0 && op <= 0xb7) || (op >= 0x70 && op <= 0x7f) ||
        (op >= 0xe0 && op <= 0xe7) || op == 0xeb || op == 0xa8 || op == 0xcd) immediate = 1;
    else if (op >= 0xa0 && op <= 0xa3) immediate = 4;
    else if (op == 0xc2 || op == 0xca) immediate = 2;
    else if (op == 0x69 || op == 0xc7 || op == 0x81) { modrm = true; immediate = operand; }
    else if (op == 0x6b || op == 0xc0 || op == 0xc1 || op == 0xc6 || op == 0x80 || op == 0x82 || op == 0x83) {
        modrm = true; immediate = 1;
    } else if ((op >= 0x84 && op <= 0x8f) || (op >= 0xd0 && op <= 0xd3) || (op >= 0xd8 && op <= 0xdf) ||
        op == 0xf6 || op == 0xf7 || op == 0xfe || op == 0xff || op == 0x62 || op == 0x63) modrm = true;
    else throw std::runtime_error("x86 指令不在静态解析档位内");
    if (modrm) {
        out.modrmAt = cursor; out.modrm = pe.Byte(cursor++);
        const auto mode = out.modrm >> 6, rm = out.modrm & 7;
        if (mode != 3 && rm == 4) {
            const auto sib = pe.Byte(cursor++);
            if (mode == 0 && (sib & 7u) == 5u) cursor += 4;
        }
        if (mode == 0 && rm == 5) cursor += 4;
        else if (mode == 1) cursor += 1;
        else if (mode == 2) cursor += 4;
        if ((op == 0xf6 || op == 0xf7) && ((out.modrm >> 3) & 7u) <= 1u) immediate = op == 0xf6 ? 1u : operand;
    }
    out.immediateAt = cursor; out.immediateSize = immediate; cursor += immediate;
    Require(cursor - at <= 15, "x86 指令超过 15 字节");
    (void)pe.Raw(pe.Rva(at), cursor - at);
    out.size = cursor - at;
    return out;
}
} // namespace War3Map::Runtime::detail
