#include "War3MapCore/MapData/TerrainRowScan.h"
#include "War3MapCore/Render/ClassicMinimapTerrainRules.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Render;
using Bytes = std::vector<std::byte>;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) {
        std::string message;
        for (const auto& d : result.diagnostics) message += d.code + ":" + d.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Put16(Bytes& bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::byte>(value & 255); bytes[at + 1] = static_cast<std::byte>(value >> 8);
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned c = 0; c < 4; ++c) bytes[at + c] = static_cast<std::byte>((value >> (c * 8)) & 255);
}
Bytes Terrain(std::uint32_t width, std::uint32_t height) {
    Bytes bytes(45 + static_cast<std::size_t>(width) * height * 7);
    const auto raw = [&](std::size_t at, std::string_view text) {
        for (std::size_t c = 0; c < text.size(); ++c) bytes[at + c] = static_cast<std::byte>(text[c]);
    };
    raw(0, "W3E!"); Put32(bytes, 4, 11); bytes[8] = std::byte{'L'};
    Put32(bytes, 13, 1); raw(17, "Ldrt"); Put32(bytes, 21, 1); raw(25, "CLdi");
    Put32(bytes, 29, width); Put32(bytes, 33, height);
    for (std::size_t at = 45; at < bytes.size(); at += 7) {
        Put16(bytes, at, 8192); Put16(bytes, at + 2, 8192); bytes[at + 6] = std::byte{0xf2};
    }
    return bytes;
}
std::size_t Record(std::uint32_t width, std::uint32_t x, std::uint32_t y) {
    return 45 + (static_cast<std::size_t>(y) * width + x) * 7;
}
void Rejected(const auto& result, std::string_view code) {
    Require(!result.Succeeded() && !result.value, "失败不能返回部分行扫描结果");
    Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& d) { return d.code == code; }),
        "行扫描缺少预期的准确失败原因");
}

template<class T> void SameResult(const Result<T>& actual, const Result<T>& expected) {
    Require(actual.Succeeded() == expected.Succeeded(), "窗口与完整网格规则的成功/拒绝状态不同");
    if (expected.Succeeded()) Require(actual.value == expected.value, "窗口改变了完整网格规则结果");
    else Require(!actual.diagnostics.empty() && !expected.diagnostics.empty()
        && actual.diagnostics.front().code == expected.diagnostics.front().code, "窗口改变了完整网格规则的拒绝原因");
}

void CompareRules(const Bytes& bytes) {
    const auto full = Take(ParseMapTerrain(bytes)).Snapshot();
    const auto source = Take(OpenTerrainRowSource(bytes));
    std::uint32_t row = 0;
    const auto facts = Take(ScanTerrainRows(source, [&](const TerrainGridSnapshot& window, std::uint32_t first,
        std::uint32_t y, TerrainRowScanBudget& budget) {
        Require(y == row++, "窗口回调必须保持原地图逐行顺序");
        Require(window.tiles.empty() && window.vertices.size() == static_cast<std::size_t>(window.header.width) * window.header.height,
            "窗口不应暗中生成完整tile数组，且必须保留准确行长");
        auto charged = budget.ChargeConsumerWork(static_cast<std::size_t>(full->header.width - 1) * 1024);
        if (!charged.Succeeded()) return charged;
        for (std::uint32_t x = 0; x + 1 < full->header.width; ++x) {
            const auto local = y - first;
            const auto& actual = window.vertices[static_cast<std::size_t>(local) * window.header.width + x];
            const auto& expected = full->vertices[static_cast<std::size_t>(y) * full->header.width + x];
            Require(actual.position == expected.position && actual.groundHeightRaw == expected.groundHeightRaw
                && actual.waterHeightAndFlagsRaw == expected.waterHeightAndFlagsRaw
                && actual.cliffAndLayerRaw == expected.cliffAndLayerRaw, "共享vertex解码必须逐字段等价");
            SameResult(ClassicMinimapCliffTextureAt(window, x, local), ClassicMinimapCliffTextureAt(*full, x, y));
            SameResult(ClassicMinimapGroundHeightAt(window, x, local), ClassicMinimapGroundHeightAt(*full, x, y));
        }
        return Status::Ok();
    }));
    Require(row + 1 == full->header.height && facts.vertexDecodes == full->vertices.size()
        && facts.tileAnchors == full->tiles.size() && facts.maximumResidentRows <= 7,
        "完整输入与末行/列必须实际解码，窗口驻留不能随图高增长");
}

void SevenRowCliffAndSaddleNeighborhoods() {
    auto fallback = Terrain(11, 15);
    fallback[Record(11, 5, 7) + 6] = std::byte{0xf3};
    fallback[Record(11, 3, 5) + 6] = std::byte{0x72};
    const auto full = Take(ParseMapTerrain(fallback)).Snapshot();
    Require(Take(ClassicMinimapCliffTextureAt(*full, 5, 8)) == 7,
        "独立夹具唯一有效cliff palette必须位于查询点y-3");
    CompareRules(fallback);
    for (int shape = 0; shape < 3; ++shape) for (int rotation = 0; rotation < 4; ++rotation) {
        auto bytes = Terrain(7, 13);
        for (std::uint32_t y = 0; y < 13; ++y) for (std::uint32_t x = 0; x < 7; ++x) {
            int u = static_cast<int>(x), v = static_cast<int>(y) - 3;
            for (int turn = 0; turn < rotation; ++turn) { const auto old = u; u = 6 - v; v = old; }
            const bool high = shape == 0 ? u >= 4 || v >= 4 : shape == 1 ? u >= 4 && v >= 4
                : (u >= 4 && v >= 4) || (u <= 2 && v <= 2);
            bytes[Record(7, x, y) + 6] = static_cast<std::byte>(high ? 0xf3 : 0xf2);
            if (x >= 2 && x <= 4 && y >= 5 && y <= 7) bytes[Record(7, x, y) + 4] |= std::byte{0x10};
        }
        CompareRules(bytes);
    }
    CompareRules(Terrain(2, 2)); CompareRules(Terrain(3, 4));
}

void CumulativeAndResidentBudgets() {
    const auto bytes = Terrain(9, 17);
    const auto run = [&](const TerrainRowScanLimits& limits) {
        auto source = OpenTerrainRowSource(bytes, limits);
        if (!source.Succeeded()) return Result<TerrainRowScanFacts>{std::nullopt, std::move(source.diagnostics)};
        return ScanTerrainRows(*source.value, [](const auto&, auto, auto, TerrainRowScanBudget& budget) {
            return budget.ChargeConsumerWork(3);
        });
    };
    const auto facts = Take(run({}));
    const std::size_t moved = 10 * 6 * 9;
    Require(facts.inputBytes == bytes.size() && facts.vertexDecodes == 153 && facts.tileAnchors == 128
        && facts.rowVertexMoves == moved && facts.consumerWorkUnits == 16 * 3
        && facts.workUnits == 153 + 128 + moved + 16 * 3 && facts.maximumResidentRows == 7,
        "每个输入、窗口移位、地格与消费者访问必须记入同一累计账");
    TerrainRowScanLimits limits; limits.maxWorkUnits = facts.workUnits;
    Require(run(limits).Succeeded(), "完整累计工作精确边界应通过");
    --limits.maxWorkUnits; Rejected(run(limits), "TERRAIN_ROW_WORK_BUDGET");
    limits = {}; limits.maxResidentBytes = facts.maximumResidentBytes;
    Require(run(limits).Succeeded(), "窗口真实capacity精确边界应通过");
    --limits.maxResidentBytes; Rejected(run(limits), "TERRAIN_ROW_RESIDENT_BUDGET");
    limits = {}; limits.maxInputBytes = bytes.size(); Require(run(limits).Succeeded(), "完整原字恰好输入预算");
    --limits.maxInputBytes; Rejected(run(limits), "TERRAIN_BYTE_LIMIT");
    limits = {}; limits.maxVertexDecodes = 152; Rejected(run(limits), "TERRAIN_VERTEX_LIMIT");
    limits = {}; limits.maxTileAnchors = 127; Rejected(run(limits), "TERRAIN_TILE_LIMIT");
    limits = {}; ++limits.maxResidentBytes; Rejected(run(limits), "TERRAIN_ROW_LIMITS");
}

void WholeSourceValidationAfterCallbacks() {
    auto bytes = Terrain(9, 17);
    Put16(bytes, Record(9, 8, 16), 0x8000);
    const auto source = Take(OpenTerrainRowSource(bytes));
    std::size_t rows = 0;
    const auto scanned = ScanTerrainRows(source, [&](const auto&, auto, auto, auto&) { ++rows; return Status::Ok(); });
    Require(rows > 0, "负控必须在已处理多行后命中末顶点错误");
    Rejected(scanned, "TERRAIN_GROUND_HEIGHT_AMBIGUOUS");
    Rejected(ParseMapTerrain(bytes), "TERRAIN_GROUND_HEIGHT_AMBIGUOUS");
    bytes = Terrain(9, 17); bytes.push_back(std::byte{0});
    Rejected(OpenTerrainRowSource(bytes), "TERRAIN_TRAILING_DATA");
    bytes.pop_back(); bytes.pop_back(); Rejected(OpenTerrainRowSource(bytes), "TERRAIN_TRUNCATED");
}
}

int main() {
    try {
        SevenRowCliffAndSaddleNeighborhoods(); CumulativeAndResidentBudgets(); WholeSourceValidationAfterCallbacks();
        std::cout << "TerrainRowScanTests: all scenarios passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
