#include "War3MapCore/MapData/PlacementElevationInvariant.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace War3Map::MapData;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// 独立构造 wire 数据；测试不会先用待核对的 writer 生成预期结果。
struct Fixture {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> records;
    std::vector<std::size_t> heights;
    std::size_t recordsEnd = 0;
    bool units = false;

    void U32(std::uint32_t value) {
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void F32(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void U8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void Raw(std::string_view value) {
        Require(value.size() == 4, "rawcode length");
        for (const auto c : value) U8(static_cast<std::uint8_t>(c));
    }
    void Patch(std::size_t offset, std::uint32_t value) {
        Require(offset + 4 <= bytes.size(), "patch bounds");
        for (unsigned i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
    }
    void Height(std::size_t row, float value) { Patch(heights.at(row), std::bit_cast<std::uint32_t>(value)); }

    War3Map::Status Verify(const Fixture& after, std::size_t budget = 64 * 1024 * 1024) const {
        return units ? VerifyUnitPlacementElevationOnly(bytes, after.bytes, budget)
            : VerifyDoodadPlacementElevationOnly(bytes, after.bytes, budget);
    }
};

Fixture MakeFixture(bool units, bool roc, std::int32_t randomMode) {
    Fixture f;
    f.units = units;
    f.Raw("W3do"); f.U32(roc ? 7 : 8); f.U32(roc ? 9 : 11); f.U32(2);
    for (std::int32_t row = 0; row < 2; ++row) {
        f.records.push_back(f.bytes.size());
        f.Raw(units ? (row ? "u002" : "u001") : (row ? "d002" : "d001"));
        f.I32(row + 1); f.F32(-128.0f + row); f.F32(256.0f + row);
        f.heights.push_back(f.bytes.size()); f.F32(16.0f + row);
        f.F32(0.25f); f.F32(1.0f); f.F32(1.25f); f.F32(0.75f);
        f.U8(2);
        if (units) {
            f.I32(1); f.U8(0x12); f.U8(0x34); f.I32(-1); f.I32(-1);
            if (!roc) f.I32(-1);
            f.U32(1); f.U32(1); f.Raw("I001"); f.I32(100);
            f.I32(0); f.F32(-1.0f); f.I32(1);
            if (!roc) { f.I32(10); f.I32(11); f.I32(12); }
            f.U32(1); f.I32(0); f.Raw("I002");
            f.U32(1); f.Raw("A001"); f.I32(1); f.I32(2);
            f.I32(randomMode);
            if (randomMode == 0) { f.U8(0xff); f.U8(0xff); f.U8(0xff); f.U8(2); }
            else if (randomMode == 1) { f.I32(1); f.I32(0); }
            else if (randomMode == 2) { f.U32(1); f.Raw("u003"); f.I32(100); }
            f.I32(-1); f.I32(-1); f.I32(100 + row);
        } else {
            f.U8(100);
            if (!roc) {
                f.I32(-1); f.U32(1); f.U32(1); f.Raw("I001"); f.I32(100);
            }
            f.I32(200 + row);
        }
    }
    f.recordsEnd = f.bytes.size();
    if (!units) {
        f.I32(0); f.U32(1);
        f.Raw("D001"); f.I32(3); f.I32(8); f.I32(12);
    }
    return f;
}

bool IsHeightByte(const Fixture& f, std::size_t offset) {
    return std::any_of(f.heights.begin(), f.heights.end(), [offset](const auto height) {
        return offset >= height && offset < height + 4;
    });
}

void CheckClassic(bool units, bool roc, std::int32_t randomMode) {
    const auto before = MakeFixture(units, roc, randomMode);
    auto after = before;
    after.Height(0, 128.5f); after.Height(1, -64.25f);
    const auto sourceBytes = before.bytes;
    const auto currentBytes = after.bytes;
    Require(before.Verify(after).Succeeded(), "multiple Z-only changes must pass");
    Require(before.bytes == sourceBytes && after.bytes == currentBytes, "verification must not mutate inputs");
    Require(before.Verify(after, before.bytes.size() + after.bytes.size()).Succeeded(), "exact cumulative budget");
    const auto budgetFailure = before.Verify(after, before.bytes.size() + after.bytes.size() - 1);
    Require(!budgetFailure.Succeeded() && budgetFailure.diagnostics.front().code == "TERRAIN_PLACEMENT_ELEVATION_BUDGET",
        "one byte above cumulative budget must reject");
    Require(!before.Verify(after, 64 * 1024 * 1024 + 1).Succeeded(), "caller cannot raise hard budget");
    Require(!before.Verify(after, 0).Succeeded(), "zero budget cannot accept nonempty inputs");
    Require(before.Verify(before).Succeeded(), "unchanged member");

    // 在合法 Z 改动上再修改每个非 Z 字节：覆盖表头、rawcode、顺序身份、XY、
    // 朝向、缩放、flags、owner、不透明字节、嵌套条目及完整 special tail。
    for (std::size_t offset = 0; offset < before.bytes.size(); ++offset) {
        if (IsHeightByte(before, offset)) continue;
        auto invalid = after;
        invalid.bytes[offset] ^= std::byte{1};
        Require(!before.Verify(invalid).Succeeded(), "non-Z mutation accepted at byte " + std::to_string(offset));
    }

    auto reordered = after;
    Require(before.recordsEnd - before.records[1] == before.records[1] - before.records[0], "fixture equal row sizes");
    std::swap_ranges(reordered.bytes.begin() + before.records[0], reordered.bytes.begin() + before.records[1],
        reordered.bytes.begin() + before.records[1]);
    Require(!before.Verify(reordered).Succeeded(), "row order must remain exact");
    auto appended = after; appended.U8(0);
    Require(!before.Verify(appended).Succeeded(), "trailing byte cannot be ignored");
    auto truncated = after; truncated.bytes.pop_back();
    Require(!before.Verify(truncated).Succeeded(), "truncation cannot be ignored");

    for (const auto height : {1'000'000.0f, -1'000'000.0f, 0.0f, -0.0f}) {
        auto edge = before; edge.Height(0, height);
        Require(before.Verify(edge).Succeeded(), "finite boundary Z must pass");
    }
    for (const auto height : {1'000'001.0f, -1'000'001.0f,
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()}) {
        auto invalid = before; invalid.Height(0, height);
        Require(!before.Verify(invalid).Succeeded(), "invalid changed Z must reject");
        if (!std::isfinite(height))
            Require(!invalid.Verify(before).Succeeded(), "cannot repair nonfinite source under Z-only rule");
    }

    auto unknown = before; unknown.Patch(4, 99);
    Require(unknown.Verify(unknown).Succeeded(), "unmodified unknown layout is opaque and preserved");
    auto changedUnknown = unknown; changedUnknown.Height(0, 64.0f);
    Require(!unknown.Verify(changedUnknown).Succeeded(), "changed unknown layout cannot gain edit permission");
    auto malformed = before; malformed.bytes.resize(13);
    Require(malformed.Verify(malformed).Succeeded(), "unchanged malformed member is outside this helper's authority");
    auto changedMalformed = malformed; changedMalformed.bytes[0] ^= std::byte{1};
    Require(!malformed.Verify(changedMalformed).Succeeded(), "malformed changed member rejected");
}

} // namespace

int main() {
    try {
        for (const bool roc : {false, true}) {
            CheckClassic(false, roc, -1);
            for (std::int32_t mode = -1; mode <= 2; ++mode) CheckClassic(true, roc, mode);
        }
        std::cout << "placement elevation invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
