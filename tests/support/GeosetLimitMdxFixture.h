#pragma once

// 原创合成 MDX v800：每个几何体的顶点数与三角形索引数可调，只用于“几何体超出游戏上限”判定测试；不来自任何游戏文件。
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace GeosetLimitFixture {

struct Geoset {
    std::uint32_t vertices = 3;
    std::uint32_t indices = 3; // 须为 3 的倍数；第 k 个索引取 k % vertices
};

class Writer {
public:
    std::vector<std::byte> bytes;
    void Raw(const void* data, std::size_t size) {
        const auto* begin = static_cast<const std::byte*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
    }
    void Tag(const char* tag) { Raw(tag, 4); }
    void U32(std::uint32_t value) { Raw(&value, 4); }
    void F32(float value) { Raw(&value, 4); }
    void U16(std::uint16_t value) { Raw(&value, 2); }
    void Fixed(const std::string& text, std::size_t length) {
        std::vector<char> buffer(length, 0);
        std::memcpy(buffer.data(), text.data(), text.size() < length ? text.size() : length);
        Raw(buffer.data(), length);
    }
    std::size_t Begin() { const auto at = bytes.size(); U32(0); return at; }
    void EndInclusive(std::size_t at) { const auto size = static_cast<std::uint32_t>(bytes.size() - at); std::memcpy(bytes.data() + at, &size, 4); }
    void EndChunk(std::size_t at) { const auto size = static_cast<std::uint32_t>(bytes.size() - at - 4); std::memcpy(bytes.data() + at, &size, 4); }
};

inline std::vector<std::byte> BuildMdx(const std::vector<Geoset>& geosets) {
    Writer w;
    w.Tag("MDLX");
    w.Tag("VERS"); w.U32(4); w.U32(800);
    w.Tag("MODL"); { const auto at = w.Begin(); w.Fixed("Slab", 80); w.Fixed("", 260); w.F32(100);
        w.F32(0); w.F32(0); w.F32(0); w.F32(128); w.F32(256); w.F32(1); w.U32(150); w.EndChunk(at); }
    w.Tag("SEQS"); { const auto at = w.Begin();
        w.Fixed("Stand", 80); w.U32(0); w.U32(1000); w.F32(0); w.U32(0); w.F32(0); w.U32(0); w.F32(0);
        for (int i = 0; i < 6; ++i) w.F32(0);
        w.EndChunk(at); }
    w.Tag("TEXS"); { const auto at = w.Begin(); w.U32(0); w.Fixed("Textures\\Slab.blp", 260); w.U32(0); w.EndChunk(at); }
    w.Tag("MTLS"); { const auto at = w.Begin();
        const auto material = w.Begin(); w.U32(0); w.U32(0); w.Tag("LAYS"); w.U32(1);
        { const auto layer = w.Begin(); w.U32(0); w.U32(0); w.U32(0); w.U32(0xFFFFFFFF); w.U32(0); w.F32(1); w.EndInclusive(layer); }
        w.EndInclusive(material);
        w.EndChunk(at); }
    w.Tag("GEOS"); { const auto at = w.Begin();
        for (const auto& g : geosets) {
            const auto geoset = w.Begin();
            w.Tag("VRTX"); w.U32(g.vertices);
            for (std::uint32_t v = 0; v < g.vertices; ++v) { w.F32(static_cast<float>(v % 128)); w.F32(static_cast<float>(v / 128)); w.F32(0); }
            w.Tag("NRMS"); w.U32(g.vertices); for (std::uint32_t v = 0; v < g.vertices; ++v) { w.F32(0); w.F32(0); w.F32(1); }
            w.Tag("PTYP"); w.U32(1); w.U32(4);
            w.Tag("PCNT"); w.U32(1); w.U32(g.indices);
            w.Tag("PVTX"); w.U32(g.indices);
            for (std::uint32_t k = 0; k < g.indices; ++k) w.U16(static_cast<std::uint16_t>(k % g.vertices));
            w.Tag("GNDX"); w.U32(g.vertices); for (std::uint32_t v = 0; v < g.vertices; ++v) w.Raw("\0", 1);
            w.Tag("MTGC"); w.U32(1); w.U32(1);
            w.Tag("MATS"); w.U32(1); w.U32(0);
            w.U32(0); w.U32(0); w.U32(0);
            w.F32(100); w.F32(0); w.F32(0); w.F32(0); w.F32(128); w.F32(256); w.F32(1);
            w.U32(1); w.F32(100); w.F32(0); w.F32(0); w.F32(0); w.F32(128); w.F32(256); w.F32(1);
            w.Tag("UVAS"); w.U32(1);
            w.Tag("UVBS"); w.U32(g.vertices); for (std::uint32_t v = 0; v < g.vertices; ++v) { w.F32(0); w.F32(0); }
            w.EndInclusive(geoset);
        }
        w.EndChunk(at); }
    w.Tag("BONE"); { const auto at = w.Begin();
        const auto node = w.Begin(); w.Fixed("Root", 80); w.U32(0); w.U32(0xFFFFFFFFu); w.U32(0);
        w.EndInclusive(node); w.U32(0xFFFFFFFFu); w.U32(0xFFFFFFFFu);
        w.EndChunk(at); }
    w.Tag("PIVT"); { const auto at = w.Begin(); w.F32(0); w.F32(0); w.F32(0); w.EndChunk(at); }
    return std::move(w.bytes);
}

} // namespace GeosetLimitFixture
