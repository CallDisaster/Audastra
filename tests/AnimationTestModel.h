#pragma once
// 在既有原创合成 MDX 上增加真实的第二动作区间；不是游戏资源，也不改参考测试生成器。
#include "MdxTestModel.h"
#include <stdexcept>
#include <string_view>

namespace AnimationTest {
// 只适用于共享原创夹具的已知常量轨迹，不用于产品或任意模型。
// 旧骨架、静态保留和KGAC测试不应因无关KMTF要求额外后端能力；专门材质用例保留真实轨迹。
inline std::vector<std::byte> WithoutSyntheticConstantKmtf(std::vector<std::byte> bytes) {
    const auto read = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (unsigned k = 0; k < 4; ++k) value |= std::to_integer<std::uint32_t>(bytes.at(at + k)) << (k * 8);
        return value;
    };
    const auto write = [&](std::size_t at, std::uint32_t value) {
        for (unsigned k = 0; k < 4; ++k) bytes.at(at + k) = static_cast<std::byte>((value >> (k * 8)) & 255u);
    };
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        const auto size = read(at + 4);
        const std::string_view tag(reinterpret_cast<const char*>(bytes.data() + at), 4);
        if (tag == "MTLS") {
            if (size != 100 || size > bytes.size() - at - 8)
                throw std::runtime_error("synthetic constant KMTF material size changed");
            const auto material=at+8,firstLayer=material+20,secondLayer=firstLayer+read(firstLayer),track=secondLayer+28;
            if (read(material)!=size || read(material+16)!=2 || read(firstLayer)!=28 || read(secondLayer)!=52 || track+24!=at+8+size ||
                std::memcmp(bytes.data()+track,"KMTF",4) || read(track+4)!=1 || read(track+8)!=0 ||
                read(track+12)!=UINT32_MAX || read(track+16)!=0 || read(track+20)!=0)
                throw std::runtime_error("synthetic constant KMTF fixture changed");
            write(secondLayer,28); write(material,read(material)-24); write(at+4,size-24);
            bytes.erase(bytes.begin()+static_cast<std::ptrdiff_t>(track),bytes.begin()+static_cast<std::ptrdiff_t>(track+24));
            return bytes; // 静态TEXS0、层alpha及其他chunk逐字节保持。
        }
        if (size > bytes.size() - at - 8) throw std::runtime_error("synthetic fixture chunk truncated");
        at += 8 + size;
    }
    throw std::runtime_error("synthetic constant KMTF material missing");
}
inline std::vector<std::byte> Bytes() {
    MdxTest::Options options; options.animatedHelper = true;
    auto bytes = WithoutSyntheticConstantKmtf(MdxTest::BuildTestMdx(options));
    const auto read = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (unsigned k = 0; k < 4; ++k) value |= std::to_integer<std::uint32_t>(bytes.at(at + k)) << (k * 8);
        return value;
    };
    const auto write = [&](std::size_t at, std::uint32_t value) {
        for (unsigned k = 0; k < 4; ++k) bytes.at(at + k) = static_cast<std::byte>((value >> (k * 8)) & 255u);
    };
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        const auto size = read(at + 4);
        const std::string_view tag(reinterpret_cast<const char*>(bytes.data() + at), 4);
        if (tag == "SEQS") {
            // 共享夹具按时间顺序写 Stand [0,1000]、Death [2000,3000]（原版按文件顺序切键区间）；
            // 把 [2000,3000] 的条目改名为 Walk，动作序号为 Stand=0、Walk=1。
            bool renamed = false;
            for (std::size_t entry = at + 8; entry + 132 <= at + 8 + size; entry += 132)
                if (read(entry + 80) == 2000 && read(entry + 84) == 3000) {
                    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(entry), 80, std::byte{});
                    std::memcpy(bytes.data() + entry, "Walk", 4); renamed = true;
                }
            if (!renamed) throw std::runtime_error("synthetic [2000,3000] sequence missing");
        }
        if (tag == "HELP") {
            const std::string_view body(reinterpret_cast<const char*>(bytes.data() + at + 8), size);
            const auto found = body.find("KGTR");
            if (found == std::string_view::npos) throw std::runtime_error("synthetic KGTR missing");
            const auto track = at + 8 + found;
            write(track + 4, 4); // Stand 的 [0,1000] 与 Walk 的 [2000,3000] 各两个按键
            MdxTest::Writer keys;
            keys.U32(2000); keys.F32(20); keys.F32(0); keys.F32(0);
            keys.U32(3000); keys.F32(60); keys.F32(0); keys.F32(0);
            write(at + 4, size + static_cast<std::uint32_t>(keys.bytes.size()));
            write(at + 8, read(at + 8) + static_cast<std::uint32_t>(keys.bytes.size()));
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + size), keys.bytes.begin(), keys.bytes.end());
            return bytes;
        }
        at += 8 + size;
    }
    throw std::runtime_error("synthetic helper missing");
}
} // namespace AnimationTest
