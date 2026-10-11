#pragma once
#include "AnimationTestModel.h"

namespace AnimationPreviewFixture {
inline std::vector<std::byte> InverseKeys() {
    auto bytes = AnimationTest::Bytes();
    const auto read = [&](std::size_t at) {
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes.at(at + i)) << (8 * i);
        return value;
    };
    const auto write = [&](std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<std::byte>((value >> (8 * i)) & 255);
    };
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        const auto size = read(at + 4);
        if (size > bytes.size() - at - 8) throw std::runtime_error("original preview fixture truncated");
        if (std::memcmp(bytes.data() + at, "SEQS", 4) == 0) {
            for (auto sequence = at + 8; sequence + 132 <= at + 8 + size; sequence += 132)
                if (read(sequence + 80) == 2000) write(sequence + 92, 1); // 原创 Walk 不循环。
        }
        if (std::memcmp(bytes.data() + at, "HELP", 4) == 0) {
            const auto body = std::string_view(reinterpret_cast<const char*>(bytes.data() + at + 8), size);
            const auto relative = body.find("KGTR");
            if (relative == std::string_view::npos) throw std::runtime_error("original preview helper missing");
            const auto track = at + 8 + relative;
            if (read(track + 4) != 4 || read(track + 8) != 1 || track + 80 != at + 8 + size)
                throw std::runtime_error("original preview track layout changed");
            MdxTest::Writer replacement;
            replacement.Tag("KGTR"); replacement.U32(6); replacement.U32(1); replacement.U32(UINT32_MAX);
            for (const auto frame : {0u, 600u, 400u, 1000u, 2000u, 3000u}) {
                replacement.U32(frame); replacement.F32(static_cast<float>(frame) / 100.0f);
                replacement.F32(0); replacement.F32(0);
            }
            write(at + 4, size + 32); write(at + 8, read(at + 8) + 32);
            bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(track),
                bytes.begin() + static_cast<std::ptrdiff_t>(track + 80));
            bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(track), replacement.bytes.begin(), replacement.bytes.end());
            return bytes;
        }
        at += 8 + size;
    }
    throw std::runtime_error("original preview model has no helper");
}
}
