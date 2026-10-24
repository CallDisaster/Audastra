#pragma once

// 由TerrainWaterBrushTests.cpp的私有namespace包含；仅复用原创样例和独立字节预期。

TerrainWaterSpecialDoodadShape DisabledSpecialShape(const DoodadPlacements& doodads, std::uint32_t source) {
    const auto& row = doodads.specialDoodads[source];
    return {source, row.rawcode, row.variation, row.position, 0, 0, 0, 0, {}, false};
}

void SpecialRuntimeMappingPreservesDisabledRows() {
    const TerrainStrokePoint point{1024, 1024};
    for (const auto kind : {TerrainWaterBrushKind::Shallow, TerrainWaterBrushKind::Deep}) {
        for (const std::uint32_t removed : {1u, 3u, 4u}) {
            auto grid = Flat(); const auto brush = Brush(grid, kind);
            auto source = SpecialFixture({{"E000", 37, {8, 8}}, {"S001", 1, {24, 24}},
                {"E002", -91, {8, 8}}, {"S003", 3, {26, 26}},
                {"S004", 4, {28, 28}}, {"E005", 122, {8, 8}}});
            source.specialDoodads[removed].position = {8, 8};
            auto parsed = ParseDoodadPlacements(DoodadBytes(source));
            Require(parsed.Succeeded(), "mixed active/disabled fixture starts from parsed complete DOO bytes");
            source = std::move(*parsed.value);
            // 形状入参反序；运行时资格绑定原始sourceRecordIndex，而不是形状数组下标。
            const std::array shapes{DisabledSpecialShape(source, 5), FlatSpecialShape(source, 4),
                FlatSpecialShape(source, 3), DisabledSpecialShape(source, 2), FlatSpecialShape(source, 1),
                DisabledSpecialShape(source, 0)};
            const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(source);
            const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes);
            Require(result.Succeeded() && result.value
                && result.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{removed},
                "equal-XY disabled rows never consume a runtime deletion or become the swap-last replacement");
            // runtime [1,3,4] 独立手写预期；三个空项在active前、中、后，额外DWORD必须保留。
            if (removed == 1) SpecialOrder(result.value->doodads, source, {0, 4, 2, 3, 5},
                "first active deletion maps runtime [4,3] back around untouched disabled records");
            if (removed == 3) SpecialOrder(result.value->doodads, source, {0, 1, 2, 4, 5},
                "middle active deletion maps runtime [1,4] without moving or dropping disabled rows");
            if (removed == 4) SpecialOrder(result.value->doodads, source, {0, 1, 2, 3, 5},
                "last active deletion removes its slot while preserving the trailing disabled record");
            Require(At(result.value->terrain, 8, 8).CliffLevel() == (kind == TerrainWaterBrushKind::Shallow ? 1 : 0),
                "mixed runtime output is coupled to the real water terrain edit");
            SourceUnchanged(grid, terrainBefore, source, doodadsBefore);
        }
    }
}

void SpecialRuntimeDuplicateXyAndRecaptureUseActiveOrder() {
    auto grid = Flat(); const auto brush = Brush(grid); const TerrainStrokePoint point{1024, 1024};
    const auto source = SpecialFixture({{"E000", 71, {8, 8}}, {"S001", 1, {8, 8}},
        {"E002", 72, {8, 8}}, {"S003", 3, {8, 8}}, {"S004", 4, {28, 28}},
        {"S005", 5, {8, 8}}, {"E006", 73, {8, 8}}});
    const std::array shapes{DisabledSpecialShape(source, 6), FlatSpecialShape(source, 5),
        FlatSpecialShape(source, 4), FlatSpecialShape(source, 3), DisabledSpecialShape(source, 2),
        FlatSpecialShape(source, 1), DisabledSpecialShape(source, 0)};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(source);
    const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes);
    Require(result.Succeeded() && result.value
        && result.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{1, 5, 3},
        "equal-XY requests consume current first active rows [1,5,3] after each runtime swap-last");
    SpecialOrder(result.value->doodads, source, {0, 4, 2, 6},
        "three runtime deletions preserve every disabled row and the sole unrelated active row");
    SourceUnchanged(grid, terrainBefore, source, doodadsBefore);

    auto reloaded = ParseDoodadPlacements(DoodadBytes(result.value->doodads));
    Require(reloaded.Succeeded(), "complete mixed output can be parsed as the next saved source");
    auto nextGrid = result.value->terrain;
    const auto nextBrush = Brush(nextGrid, TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 28, 28);
    const TerrainStrokePoint nextPoint{3584, 3584};
    const std::array nextShapes{DisabledSpecialShape(*reloaded.value, 3), DisabledSpecialShape(*reloaded.value, 2),
        FlatSpecialShape(*reloaded.value, 1), DisabledSpecialShape(*reloaded.value, 0)};
    const auto next = PrepareTerrainWaterStroke(nextGrid, Rules(), nextBrush, std::span(&nextPoint, 1),
        *reloaded.value, nextShapes);
    Require(next.Succeeded() && next.value
        && next.value->result.removedSpecialSourceRecords == std::vector<std::uint32_t>{1},
        "a subsequent captured stroke binds the surviving active row to its new raw-source index");
    SpecialOrder(next.value->doodads, *reloaded.value, {0, 2, 3},
        "deleting the last active runtime entry leaves all disabled records byte-for-byte intact");
}

void SpecialRuntimeNoDeletionKeepsRawSource() {
    const TerrainStrokePoint point{1024, 1024};
    for (const bool hasActive : {false, true}) {
        auto grid = Flat();
        At(grid, 8, 8).textureAndFlagsRaw |= 0x40;
        At(grid, 8, 8).waterHeightAndFlagsRaw = 8192;
        const auto brush = Brush(grid);
        const auto source = SpecialFixture({{"E000", 104, {-17, 9000}}, {"S001", 1, {28, 28}},
            {"E002", -13, {8, 8}}});
        const std::array shapes{DisabledSpecialShape(source, 2),
            hasActive ? FlatSpecialShape(source, 1) : DisabledSpecialShape(source, 1), DisabledSpecialShape(source, 0)};
        const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(source);
        const auto result = PrepareTerrainWaterStroke(grid, Rules(false), brush, std::span(&point, 1), source, shapes);
        Require(result.Succeeded() && result.value && result.value->result.removedSpecialSourceRecords.empty()
            && result.value->result.changed.Empty() && Bytes(result.value->terrain) == terrainBefore
            && DoodadBytes(result.value->doodads) == doodadsBefore,
            "structural no-op with all-disabled or mixed rows preserves W3E and every DOO field without filtering");
        SourceUnchanged(grid, terrainBefore, source, doodadsBefore);
    }
}

void SpecialRuntimeMappingKeepsBudgetsAndAtomicFailures() {
    auto grid = Flat(); const auto brush = Brush(grid); const TerrainStrokePoint point{1024, 1024};
    const auto source = SpecialFixture({{"E000", 37, {8, 8}}, {"S001", 1, {8, 8}},
        {"E002", 38, {8, 8}}, {"S003", 3, {28, 28}}, {"E004", 39, {8, 8}}});
    const std::array shapes{DisabledSpecialShape(source, 4), FlatSpecialShape(source, 3),
        DisabledSpecialShape(source, 2), FlatSpecialShape(source, 1), DisabledSpecialShape(source, 0)};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(source);
    const auto prepared = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes);
    Require(prepared.Succeeded() && prepared.value && prepared.value->result.work > source.specialDoodads.size(),
        "mixed source mapping succeeds under the unchanged hard work cap");
    TerrainWaterBrushLimits exact; exact.maximumWork = prepared.value->result.work;
    Require(PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes, exact).Succeeded(),
        "the complete reported work budget admits the same deterministic mixed transaction");
    --exact.maximumWork;
    const auto limited = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes, exact);
    Require(!limited.Succeeded() && !limited.value && std::any_of(limited.diagnostics.begin(), limited.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "TERRAIN_WATER_WORK_BUDGET"; }),
        "one less work unit rejects output mapping without publishing partial terrain or DOO");
    TerrainWaterBrushLimits memory; memory.maximumCandidateBytes = 1;
    const auto noMemory = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes, memory);
    Require(!noMemory.Succeeded() && !noMemory.value && std::any_of(noMemory.diagnostics.begin(), noMemory.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "TERRAIN_WATER_MEMORY_BUDGET"; }),
        "mixed source mapping does not relax candidate memory limits");
    Require(!ApplyTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes).Succeeded(),
        "terrain-only legacy Apply still refuses to discard the coupled mixed DOO deletion");
    SourceUnchanged(grid, terrainBefore, source, doodadsBefore);
}

void SpecialRuntimeDuplicateXyStillRejectsInvalidSurvivor() {
    auto grid = Flat(); const auto brush = Brush(grid); const TerrainStrokePoint point{1024, 1024};
    const auto source = SpecialFixture({{"E000", 37, {6, 6}}, {"S001", 1, {6, 6}},
        {"E002", 38, {6, 6}}, {"S003", 3, {6, 6}}});
    const std::array shapes{FlatSpecialShape(source, 3, 3, 3), DisabledSpecialShape(source, 2),
        FlatSpecialShape(source, 1), DisabledSpecialShape(source, 0)};
    const auto terrainBefore = Bytes(grid), doodadsBefore = DoodadBytes(source);
    // 宽形态3才失配，但原版首XY查找会先删窄形态1；不能追加删除阶段强行通过。
    const auto result = PrepareTerrainWaterStroke(grid, Rules(), brush, std::span(&point, 1), source, shapes);
    Require(!result.Succeeded() && !result.value && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "TERRAIN_WATER_SPECIAL_DOODAD_DELETE"; }),
        "excluding disabled rows never relaxes the final invalid-active-survivor rejection");
    SourceUnchanged(grid, terrainBefore, source, doodadsBefore);
}
