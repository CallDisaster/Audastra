#include "War3MapCore/Render/MdxDeclaredExtents.h"
#include "War3MapCore/Render/MdxModel.h"
#include "support/GeosetLimitMdxFixture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// MDX 写明范围（MODL/SEQS/GEOS）：读取位置与数值、不安全布局拒绝、加宽只改范围字段且仍能完整解析。
namespace {
using namespace War3Map;
using namespace War3Map::Render;
namespace F = GeosetLimitFixture;

void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "unexpected failure";
        for (const auto& diagnostic : result.diagnostics) message += "\n" + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
template<class T> bool HasCode(const Result<T>& result, std::string_view code) {
    return !result.Succeeded() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
float FloatAt(const std::vector<std::byte>& bytes, std::size_t at) { float value = 0; std::memcpy(&value, bytes.data() + at, 4); return value; }
void PutFloat(std::vector<std::byte>& bytes, std::size_t at, float value) { std::memcpy(bytes.data() + at, &value, 4); }
std::size_t ChunkBody(const std::vector<std::byte>& bytes, const char* tag) {
    for (std::size_t at = 4; at + 8 <= bytes.size();) {
        std::uint32_t size = 0; std::memcpy(&size, bytes.data() + at + 4, 4);
        if (std::memcmp(bytes.data() + at, tag, 4) == 0) return at + 8;
        at += 8 + size;
    }
    throw std::runtime_error(std::string("fixture chunk missing: ") + tag);
}

F::Extents Distinct() {
    F::Extents extents;
    extents.model = {40, {-10, -20, 0}, {10, 20, 30}};
    extents.sequence = {41, {-11, -21, 0}, {11, 21, 31}};
    extents.geoset = {42, {-12, -22, 0}, {12, 22, 32}};
    extents.geosetSequence = {43, {-13, -23, 0}, {13, 23, 33}};
    extents.sequences = 2;
    return extents;
}

std::vector<const MdxDeclaredExtent*> All(const MdxDeclaredExtents& extents) {
    std::vector<const MdxDeclaredExtent*> all{&extents.model};
    for (const auto* list : {&extents.sequences, &extents.geosets, &extents.geosetSequences})
        for (const auto& extent : *list) all.push_back(&extent);
    return all;
}

void ReadsEveryDeclaredExtent() {
    const auto bytes = F::BuildMdx({{}, {}}, Distinct());
    const auto extents = Take(ReadMdxDeclaredExtents(bytes, MdxExtentScope::All));
    Require(extents.sequences.size() == 2 && extents.geosets.size() == 2 && extents.geosetSequences.size() == 4,
        "one model extent, one per sequence, one per geoset and one per geoset and sequence");
    Require(extents.model.radius == 40 && extents.model.minimum[1] == -20 && extents.model.maximum[2] == 30
        && extents.sequences[1].radius == 41 && extents.sequences[1].maximum[0] == 11
        && extents.geosets[1].minimum[0] == -12 && extents.geosetSequences[3].maximum[1] == 23, "values come from their own fields");
    for (const auto* extent : All(extents))
        Require(FloatAt(bytes, extent->offset) == extent->radius && FloatAt(bytes, extent->offset + 24) == extent->maximum[2]
            && MdxExtentUsable(*extent), "each offset points at its 28-byte radius/min/max field");
    Require(extents.model.offset == ChunkBody(bytes, "MODL") + 340, "MODL extent follows the 80-byte name and 260-byte path");
    const auto partial = Take(ReadMdxDeclaredExtents(bytes, MdxExtentScope::ModelAndSequences));
    Require(partial.geosets.empty() && partial.geosetSequences.empty() && partial.sequences.size() == 2,
        "the model-and-sequences scope does not walk GEOS");
}

void RejectsUnsafeLayouts() {
    const auto bytes = F::BuildMdx({{}}, Distinct());
    auto version = bytes;
    const std::uint32_t reforged = 900; std::memcpy(version.data() + ChunkBody(version, "VERS"), &reforged, 4);
    Require(HasCode(ReadMdxDeclaredExtents(version, MdxExtentScope::All), "MDX_EXTENT_VERSION"), "only classic version 800");
    auto geoset = bytes;
    const auto mats = std::search(geoset.begin(), geoset.end(), reinterpret_cast<const std::byte*>("MATS"),
        reinterpret_cast<const std::byte*>("MATS") + 4);
    Require(mats != geoset.end(), "fixture MATS tag");
    std::memcpy(&*mats, "MATX", 4);
    Require(HasCode(ReadMdxDeclaredExtents(geoset, MdxExtentScope::All), "MDX_EXTENT_LAYOUT"),
        "a geoset whose fields do not match the classic order is refused");
    Require(ReadMdxDeclaredExtents(geoset, MdxExtentScope::ModelAndSequences).Succeeded(),
        "the model-and-sequences scope does not depend on GEOS");
    auto twice = bytes;
    const auto modl = ChunkBody(bytes, "MODL") - 8;
    twice.insert(twice.end(), bytes.begin() + static_cast<std::ptrdiff_t>(modl), bytes.begin() + static_cast<std::ptrdiff_t>(modl + 8 + 372));
    Require(HasCode(ReadMdxDeclaredExtents(twice, MdxExtentScope::All), "MDX_EXTENT_LAYOUT"), "a second MODL is refused");
    auto cut = bytes; cut.resize(cut.size() - 3);
    Require(HasCode(ReadMdxDeclaredExtents(cut, MdxExtentScope::All), "MDX_EXTENT_LAYOUT"), "a truncated chunk is refused");
    const std::string text = "MDLX-synthetic";
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    Require(!ReadMdxDeclaredExtents(std::vector<std::byte>(begin, begin + text.size()), MdxExtentScope::All).Succeeded(),
        "a placeholder file is not read as a model");
}

void WidensOnlyExtentBytes() {
    const auto bytes = F::BuildMdx({{}, {}}, Distinct());
    const auto before = Take(ReadMdxDeclaredExtents(bytes, MdxExtentScope::All));
    const auto widened = Take(WidenMdxDeclaredExtents(bytes, {-5, -5, -1000}, {5, 5, -900}));
    Require(widened.bytes.size() == bytes.size() && widened.changed == 9, "all 9 extents widened, file length unchanged");
    std::vector<char> field(bytes.size(), 0);
    for (const auto* extent : All(before)) std::fill_n(field.begin() + static_cast<std::ptrdiff_t>(extent->offset), kMdxExtentBytes, 1);
    for (std::size_t at = 0; at < bytes.size(); ++at)
        Require(field[at] || widened.bytes[at] == bytes[at], "byte " + std::to_string(at) + " outside the extent fields changed");
    const auto after = Take(ReadMdxDeclaredExtents(widened.bytes, MdxExtentScope::All));
    Require(after.model.minimum[0] == -10 && after.model.maximum[1] == 20 && after.model.minimum[2] == -1000
        && after.model.maximum[2] == 30, "the union keeps the old box and adds the target");
    const double half = 0.5 * std::sqrt(20.0 * 20 + 40.0 * 40 + 1030.0 * 1030);
    Require(after.model.radius >= half && after.model.radius - half < 1e-3, "radius is half the new box diagonal");
    for (const auto* extent : All(after))
        Require(extent->minimum[2] <= -1000 && extent->maximum[2] >= -900 && extent->minimum[0] <= -5 && extent->maximum[0] >= 5,
            "every extent contains the target");
    const auto parsed = ParseMdx(widened.bytes);
    Require(parsed.Succeeded() && parsed.value && parsed.value->geosets.size() == 2 && parsed.value->sequences.size() == 2
        && parsed.value->minimum[2] == -1000 && parsed.value->boundsRadius == after.model.radius,
        "the widened model still parses and reports the new MODL extent");

    const auto inside = Take(WidenMdxDeclaredExtents(bytes, {-1, -1, 1}, {1, 1, 2}));
    Require(inside.changed == 0 && inside.bytes == bytes, "extents that already contain the target stay untouched");
    const auto outward = Take(WidenMdxDeclaredExtents(bytes, {0, 0, 0}, {0.1, 0.1, 100.1}));
    const auto rounded = Take(ReadMdxDeclaredExtents(outward.bytes, MdxExtentScope::All));
    Require(double(rounded.model.maximum[2]) >= 100.1, "single-precision rounding goes outward");

    auto invalid = Distinct(); invalid.geoset.minimum[0] = 50;
    Require(HasCode(WidenMdxDeclaredExtents(F::BuildMdx({{}}, invalid), {0, 0, 0}, {1, 1, 1}), "MDX_EXTENT_INVALID"),
        "an extent with minimum above maximum is not widened");
    auto nan = bytes; PutFloat(nan, before.sequences[0].offset, std::numeric_limits<float>::quiet_NaN());
    Require(HasCode(WidenMdxDeclaredExtents(nan, {0, 0, 0}, {1, 1, 1}), "MDX_EXTENT_INVALID"), "a NaN radius is not widened");
    Require(HasCode(WidenMdxDeclaredExtents(bytes, {0, 0, 0}, {1e39, 1, 1}), "MDX_EXTENT_RANGE"), "beyond single precision is refused");
    Require(HasCode(WidenMdxDeclaredExtents(bytes, {1, 0, 0}, {0, 1, 1}), "MDX_EXTENT_TARGET"), "an inverted target is refused");
}
} // namespace

int main() {
    try {
        ReadsEveryDeclaredExtent(); RejectsUnsafeLayouts(); WidensOnlyExtentBytes();
        std::cout << "MdxDeclaredExtents tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
