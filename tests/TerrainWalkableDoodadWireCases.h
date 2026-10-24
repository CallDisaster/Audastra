#pragma once

// 由 MapPlacementsTests.cpp 的私有 namespace 包含。复用手写经典 wire 夹具，
// 核单条长度来自唯一 writer；不以待测 serializer 制造预期记录长度。
void CheckClassicDoodadRecordWireMeasurement() {
    const auto fixture = Doodads();
    const auto parsed = ParseDoodadPlacements(fixture.bytes);
    Require(parsed.Succeeded() && parsed.value && parsed.value->doodads.size() == 1,
        "handwritten classic DOO fixture parses for record measurement");
    const auto& row = parsed.value->doodads.front();
    const auto projection = MeasureClassicDoodadRecordWire(row, 8, 11);
    Require(projection.Succeeded() && projection.value && projection.value->byteLength == 82
        && projection.value->placementCount == 1 && projection.value->nestedRecordCount == 5,
        "complete TFT record measures fixed fields plus two sets and three item choices");
    Require(fixture.specialOffset == 16 + projection.value->byteLength,
        "single-record projection ends at the independently handwritten special boundary");
    PlacementReadLimits limits; limits.maxBytes = 82; limits.maxPlacements = 1; limits.maxNestedRecords = 5;
    Require(MeasureClassicDoodadRecordWire(row, 8, 11, limits).Succeeded(),
        "exact record byte, placement and cumulative nested budgets succeed");
    limits.maxBytes = 81;
    RequireWriteFailure(MeasureClassicDoodadRecordWire(row, 8, 11, limits), "MAP_WRITE_BUDGET");
    limits.maxBytes = 82; limits.maxNestedRecords = 4;
    RequireWriteFailure(MeasureClassicDoodadRecordWire(row, 8, 11, limits), "PLACEMENT_RECORD_LIMIT");
    limits.maxNestedRecords = 5; limits.maxPlacements = 0;
    RequireWriteFailure(MeasureClassicDoodadRecordWire(row, 8, 11, limits), "PLACEMENT_RECORD_LIMIT");
    RequireWriteFailure(MeasureClassicDoodadRecordWire(row, 8, 9), "PLACEMENT_VERSION_UNSUPPORTED");
    RequireWriteFailure(MeasureClassicDoodadRecordWire(row, 7, 9), "PLACEMENT_ROC_FIELD");
    auto roc = row; roc.droppedItemTable = -1; roc.itemSets.clear();
    const auto classicRoc = MeasureClassicDoodadRecordWire(roc, 7, 9);
    Require(classicRoc.Succeeded() && classicRoc.value && classicRoc.value->byteLength == 42
        && classicRoc.value->placementCount == 1 && classicRoc.value->nestedRecordCount == 0,
        "ROC single record excludes all TFT drop fields");
    auto invalid = row; invalid.skinId = "Skin";
    RequireWriteFailure(MeasureClassicDoodadRecordWire(invalid, 8, 11), "PLACEMENT_SKIN_WRITE_UNSUPPORTED");
    invalid = row; invalid.position[2] = std::numeric_limits<float>::infinity();
    RequireWriteFailure(MeasureClassicDoodadRecordWire(invalid, 8, 11), "MAP_WRITE_NONFINITE");
    invalid = row; invalid.flags = 0x20;
    RequireWriteFailure(MeasureClassicDoodadRecordWire(invalid, 8, 11), "PLACEMENT_LAYOUT_UNSUPPORTED");
}
