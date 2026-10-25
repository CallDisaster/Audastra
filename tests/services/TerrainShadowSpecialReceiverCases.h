#pragma once

// 包含在receiver测试夹具与RampReceiverCases之后，由集成方登记调用。
// 原创地图/表/TGA/MDX；完整receiver expected独立写像素矩形和F32常量。
// 正式SHD保存验证属于集成回读，不冒充原版保存或SHD算法oracle。
namespace {
namespace SpecialReceiverCases {
constexpr std::string_view kFootprint = "Fixture/Special.tga";
constexpr std::string_view kFirstModel = "Fixture/SpecialA.mdx", kSecondModel = "Fixture/SpecialB.mdx";

Bytes Table(std::string_view path = kFootprint, std::string_view firstFile = "Fixture/SpecialA",
    unsigned variants = 1, bool caster = false) {
    std::string text = "ID;PWXL;N;E\nC;X1;Y1;K\"doodID\"\nC;X2;Y1;K\"file\"\n"
        "C;X3;Y1;K\"numVar\"\nC;X4;Y1;K\"pathTex\"\n"
        "C;X5;Y1;K\"shadow\"\nC;X6;Y1;K\"defScale\"\nC;X7;Y1;K\"maxPitch\"\n"
        "C;X8;Y1;K\"maxRoll\"\nC;X9;Y1;K\"fixedRot\"\nC;X10;Y1;K\"floats\"\n";
    for (unsigned row = 2; row <= 3; ++row) {
        const auto y = std::to_string(row);
        text += "C;X1;Y" + y + ";K\"" + (row == 2 ? "S001" : "S002") + "\"\n"
            + "C;X2;Y" + y + ";K\"" + std::string(row == 2 ? firstFile : "Fixture/SpecialB") + "\"\n"
            + "C;X3;Y" + y + ";K" + std::to_string(variants) + "\n"
            + "C;X4;Y" + y + ";K\"" + std::string(path) + "\"\n"
            + "C;X5;Y" + y + ";K0\nC;X6;Y" + y + ";K1\nC;X7;Y" + y + ";K0\n"
            + "C;X8;Y" + y + ";K0\nC;X9;Y" + y + ";K-1\nC;X10;Y" + y + ";K0\n";
    }
    if (caster) text += "C;X1;Y4;K\"D001\"\nC;X2;Y4;K\"Fixture/Caster\"\nC;X3;Y4;K1\n"
        "C;X4;Y4;K\"_\"\nC;X5;Y4;K1\nC;X6;Y4;K1\nC;X7;Y4;K0\nC;X8;Y4;K0\n"
        "C;X9;Y4;K-1\nC;X10;Y4;K0\n";
    return Text(text + "E\n");
}
Bytes Footprint(unsigned width = 8, unsigned height = 4, bool bottomOrigin = false) {
    Bytes bytes(18 + std::size_t(width) * height * 4);
    bytes[2] = std::byte{2}; bytes[12] = std::byte(width); bytes[13] = std::byte(width >> 8);
    bytes[14] = std::byte(height); bytes[15] = std::byte(height >> 8);
    bytes[16] = std::byte{32}; bytes[17] = bottomOrigin ? std::byte{8} : std::byte{0x28};
    // BGRA全零，四角alpha均对应相对零层；原始层级全2，最低模型基准是0世界高。
    return bytes;
}
Bytes CenteredModel(float z, unsigned width = 8, unsigned height = 4, bool lastMiss = false, bool dynamic = false) {
    auto bytes = Model(z, lastMiss, width * 32.0f, dynamic);
    constexpr std::array<std::byte, 4> tag{std::byte{'V'}, std::byte{'R'}, std::byte{'T'}, std::byte{'X'}};
    const auto found = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
    Require(found != bytes.end(), "原创special模型包含独立可定位VRTX");
    const auto offset = static_cast<std::size_t>(found - bytes.begin()) + 8;
    Require(offset + 4 * 12 <= bytes.size(), "原创special四顶点布局完整");
    for (std::size_t i = 0; i < 4; ++i) {
        float x = 0, y = 0;
        std::memcpy(&x, bytes.data() + offset + i * 12, 4);
        std::memcpy(&y, bytes.data() + offset + i * 12 + 4, 4);
        x = x * (height / 4.0f) + height * 16.0f; y -= width * 16.0f;
        std::memcpy(bytes.data() + offset + i * 12, &x, 4);
        std::memcpy(bytes.data() + offset + i * 12 + 4, &y, 4);
    }
    return bytes;
}
void SetSpecials(Fixture& fixture, std::span<const SpecialDoodadPlacement> special) {
    DoodadPlacements placements; placements.specialDoodads.assign(special.begin(), special.end());
    fixture.Set("war3map.doo", Take(placements.Serialize()));
}
void SetFlat(Fixture& fixture, bool overlap = false, bool bottomOrigin = false) {
    fixture.Erase("war3map.w3b"); fixture.Erase("war3map.w3d");
    fixture.terrain.header.width = fixture.terrain.header.height = 17;
    fixture.terrain.vertices.assign(17 * 17, {});
    for (auto& vertex : fixture.terrain.vertices) {
        vertex.groundHeightRaw = vertex.waterHeightAndFlagsRaw = 8192; vertex.cliffAndLayerRaw = 2;
    }
    RefreshTerrainDerived(fixture.terrain, 0, 0, 17, 17);
    fixture.Set("war3map.w3e", Take(SerializeMapTerrain(fixture.terrain)));
    fixture.Set("war3map.shd", Bytes(64 * 64, std::byte{0x7d}));
    fixture.Set("Units/DestructableData.slk", Text("ID;PWXL;N;E\nC;X1;Y1;K\"DestructableID\"\nE\n"));
    fixture.Set("Doodads/Doodads.slk", Table());
    fixture.Set(std::string(kFootprint), Footprint(8, 4, bottomOrigin));
    fixture.Set(std::string(kFirstModel), CenteredModel(64));
    fixture.Set(std::string(kSecondModel), CenteredModel(192));
    const std::array<SpecialDoodadPlacement, 2> special{{{"S001", 0, {11,11}}, {"S002", 0, {11,11}}}};
    SetSpecials(fixture, std::span(special).first(overlap ? 2 : 1)); fixture.Open();
}
void RequireRectangle(const TerrainShadowReceiverGrid& grid, std::uint32_t heightBits) {
    Require(grid.width == 64 && grid.height == 64 && grid.receiverHeights.size() == 64 * 64,
        "special必须采完整SHD网格，不返回占地局部候选");
    // 实际8x4像素占地=两格宽一格高，锚点11跨到相邻x block。
    // 原模型先恢复中心偏移再旋转；锚点1408处F32半ULP足以收掉本矩形旋转残差。
    // footprint外平地0，内平面独立64/192；最终SHD receiver均加原版0.1。
    for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
        const bool covered = x >= 44 && x < 52 && y >= 44 && y < 48;
        RequireF32At(grid.receiverHeights[std::size_t(y) * 64 + x], covered ? heightBits : 0x3dcccccdu,
            std::size_t(y) * 64 + x, "独立完整special非方矩形、末格及跨block锚点采样");
    }
}
void GeometryOrderAndFallback() {
    for (const bool bottomOrigin : {false, true}) {
        Fixture fixture(true); SetFlat(fixture, false, bottomOrigin);
        const auto owner = Take(fixture.Capture());
        Require(owner->RecordCount() == 1, "平地special不得走ordinary早返回，也不能按覆盖格重复建record");
        RequireRectangle(Take(owner->Sample()), 0x42803333u); Check(owner->VerifySources(*fixture.source));
    }
    Fixture overlap(true); SetFlat(overlap, true);
    const auto first = Take(overlap.Capture()); Require(first->RecordCount() == 2, "重叠special仍按原序保留两条record");
    RequireRectangle(Take(first->Sample()), 0x42803333u);
    const std::array<SpecialDoodadPlacement, 2> reversed{{{"S002", 0, {11,11}}, {"S001", 0, {11,11}}}};
    SetSpecials(overlap, reversed); overlap.Open();
    RequireRectangle(Take(Take(overlap.Capture())->Sample()), 0x4340199au);
    overlap.Set(std::string(kSecondModel), CenteredModel(192, 8, 4, true)); overlap.Open();
    const auto fallback = Take(Take(overlap.Capture())->Sample());
    for (std::size_t i = 0; i < fallback.receiverHeights.size(); ++i)
        RequireF32At(fallback.receiverHeights[i], 0x3dcccccdu, i,
            "首special最后group false必须回退同图最近顶点，不借重叠第二模型或any-hit");
}
void OrdinaryRecordsStayBeforeSpecial() {
    Fixture fixture(true); RampReceiverCases::SetTerrain(fixture, 1, 0);
    fixture.Set("Units/DestructableData.slk", Text("ID;PWXL;N;E\nC;X1;Y1;K\"DestructableID\"\nE\n"));
    fixture.Set("Doodads/Doodads.slk", Table()); fixture.Set(std::string(kFootprint), Footprint(4,4));
    fixture.Set(std::string(kFirstModel), CenteredModel(192,4,4));
    const std::array<SpecialDoodadPlacement, 1> special{{{"S001", 0, {9,7}}}};
    SetSpecials(fixture, special); fixture.Open();
    const auto owner = Take(fixture.Capture()); const auto grid = Take(owner->Sample());
    // 普通ramp anchor(8,7)的record先于special，闭区间也包含special anchor(9,7)。
    // 原射线在alternate低端miss，最近顶点(9,7)层级2且非R=0；不能直接选special192。
    RequireF32At(grid.receiverHeights[28 * 64 + 36], 0x3dcccccdu, 28 * 64 + 36,
        "special锚点映射后仍须原序首包含，不能绕过在前普通坡道record");
}
void MinimumUsesFullFootprint() {
    Fixture fixture(true); SetFlat(fixture);
    fixture.terrain.vertices[11 * 17 + 11].cliffAndLayerRaw = 3;
    RefreshTerrainDerived(fixture.terrain, 0, 0, 17, 17);
    fixture.Set("war3map.w3e", Take(SerializeMapTerrain(fixture.terrain)));
    auto footprint = Footprint();
    // SW角alpha=128，其余顶点alpha=0；原版最低层在占地其它顶点，不是锚点3。
    footprint[18 + (3 * 8) * 4 + 3] = std::byte{128};
    fixture.Set(std::string(kFootprint), footprint);
    for (const auto shape : {"ABAA", "BAAA", "AABA"})
        fixture.Set("Doodads/Terrain/Fixture/Cliff/Fixture/Cliff" + std::string(shape) + "0.mdx", Model(0));
    fixture.Open();
    const auto owner = Take(fixture.Capture()); const auto grid = Take(owner->Sample());
    RequireF32At(grid.receiverHeights[44 * 64 + 44], 0x42803333u, 44 * 64 + 44,
        "special最低层须扫完整闭区间四角顶点，不能只取锚点或display高度");
}
void SourcesAndFormalSave() {
    Fixture fixture(true); SetFlat(fixture);
    const auto owner = Take(fixture.Capture()); const auto originalSha = Take(Sha256File(fixture.source->SourcePath()));
    const std::array<MemberReplacement, 1> changedModel{{{std::string(kFirstModel), CenteredModel(80)}}};
    Rejected(owner->VerifySources(*fixture.source, changedModel), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    const std::array<MemberReplacement, 1> changedTable{{{"Doodads/Doodads.slk", Table(kFootprint, "Fixture/Changed")}}};
    Rejected(owner->VerifySources(*fixture.source, changedTable), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    auto modifiedTga = Footprint(); modifiedTga[18] = std::byte{1};
    const std::array<MemberReplacement, 1> changedTga{{{std::string(kFootprint), modifiedTga}}};
    Rejected(owner->VerifySources(*fixture.source, changedTga), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    const std::array<MemberReplacement, 1> mdl{{{"Fixture/SpecialA.mdl", Text("authored preferred model")}}};
    Rejected(owner->VerifySources(*fixture.source, mdl), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    Rejected(fixture.Capture(mdl), "TERRAIN_SHADOW_RECEIVER_MDL_UNSUPPORTED");
    const std::array<std::string, 1> removed{std::string(kFirstModel)};
    Rejected(owner->VerifySources(*fixture.source, {}, removed), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    MapSaveTask normal; Check(normal.FreezeArchive(*fixture.source)); normal.target = fixture.root / "normal-special.w3x";
    MapSaveService::Run(normal); Check({normal.diagnostics});
    Require(normal.succeeded && normal.nextSession && !normal.terrainShadow
        && Take(normal.nextSession->ReadMember("war3map.shd")) == Bytes(64 * 64, std::byte{0x7d}),
        "普通保存保留原SHD，特殊接收面不得暗中触发显式重算");
    MapSaveTask task; Check(task.FreezeArchive(*fixture.source)); task.target = fixture.root / "rebuilt-special.w3x";
    task.prepareFinalDerived = [&](MapSaveTask& current) {
        Require(current.InFinalDerivedStage(), "special阴影只在正式最终派生站计算");
        std::shared_ptr<const TerrainSavePlan> plan;
        const auto status = PrepareTerrainShadowBake(current, fixture.game, nullptr, plan);
        Require(!plan, "单独计算special阴影不伪造地形编辑计划"); return status;
    };
    MapSaveService::Run(task); Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && task.terrainShadow, "special显式计算须完整写出并回读SHD");
    Check(owner->VerifyReadback(*task.nextSession)); Check(task.terrainShadow->VerifyReadback(*task.nextSession));
    const auto bytes = Take(task.nextSession->ReadMember("war3map.shd"));
    Require(bytes.size() == 64 * 64 && bytes != Bytes(64 * 64, std::byte{0x7d}), "显式special保存替换完整旧SHD");
    for (const auto& member : fixture.members) if (member.name != "war3map.shd")
        Require(Take(task.nextSession->ReadMember(member.name)) == member.data, "显式special保存原样保留其它所有成员");
    Require(Take(Sha256File(fixture.source->SourcePath())) == originalSha, "special真实归档源只读");
    // 同一原创来源移到自建game MPQ，核实际winner；不把声明的game根当来源证明。
    std::vector<MapArchiveMember> gameMembers;
    for (const auto& member : fixture.members) if (member.name == "Doodads/Doodads.slk"
        || member.name == "Units/DestructableData.slk" || member.name == kFootprint || member.name == kFirstModel)
        gameMembers.push_back(member);
    for (const auto& member : gameMembers) fixture.Erase(member.name);
    fixture.ReplaceBaseGameArchive(gameMembers); fixture.Open();
    const auto gameOwner = Take(fixture.Capture()); RequireRectangle(Take(gameOwner->Sample()), 0x42803333u);
    Require(std::any_of(gameOwner->Dependencies().begin(), gameOwner->Dependencies().end(), [](const auto& dependency) {
        return dependency.name == "FIXTURE\\SPECIALA.MDX" && dependency.source == TerrainShadowCasterSource::Game
            && !dependency.gameContainer.empty() && !dependency.sha256.empty();
    }), "special实际模型依赖要报告game winner容器与SHA");
    Check(gameOwner->VerifySources(*fixture.source));
}
void Guards() {
    Fixture fixture(true); SetFlat(fixture, true);
    auto limits = TerrainShadowReceiverLimits{}; limits.maximumRecords = 1;
    Rejected(fixture.Capture({}, limits), "TERRAIN_SHADOW_RECEIVER_BUDGET");
    limits = {}; limits.maximumWork = 100; Rejected(fixture.Capture({}, limits), "TERRAIN_SHADOW_RECEIVER_BUDGET");
    const auto measured = Take(fixture.Capture());
    limits = {}; limits.maximumWork = measured->Work();
    Require(Take(fixture.Capture({}, limits))->Work() == measured->Work(), "special重复捕获的精确work门须稳定");
    --limits.maximumWork; Rejected(fixture.Capture({}, limits), "TERRAIN_SHADOW_RECEIVER_BUDGET");
    const std::array<SpecialDoodadPlacement, 1> nonzero{{{"S001", 1, {11,11}}}};
    SetSpecials(fixture, nonzero); fixture.Open(); Rejected(fixture.Capture(), "TERRAIN_SHADOW_SPECIAL_VARIATION");
    SetFlat(fixture); fixture.Set(std::string(kFootprint), Footprint(6,4)); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SPECIAL_LAYOUT");
    SetFlat(fixture); fixture.Set("Doodads/Doodads.slk", Table("Fixture/Special.blp")); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_SPECIAL_PATH");
    SetFlat(fixture); fixture.Set("Doodads/Doodads.slk", Table(kFootprint, "Fixture/SpecialA.mdx")); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_SPECIAL_MODEL_PATH");
    SetFlat(fixture); fixture.Set(std::string(kFirstModel), CenteredModel(64,8,4,false,true)); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_RECEIVER_CONTROLLER");
    SetFlat(fixture); fixture.Erase(kFirstModel); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_SPECIAL_MODEL_MISSING");
    SetFlat(fixture); Writer native; native.U32(2); native.U32(1); native.Tag("S001"); native.U32(0); native.U32(0); native.U32(0);
    fixture.Set("war3map.w3d", native.bytes); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_SPECIAL_NATIVE_UNSUPPORTED");
    SetFlat(fixture); fixture.Set("Doodads/Doodads.slk", Table(kFootprint, "Fixture/SpecialA", 1, true));
    fixture.Set("Fixture/Caster.mdx", Model(32));
    DoodadPlacements mixed;
    mixed.specialDoodads = {{"S001", 0, {11,11}}};
    DoodadPlacement caster; caster.rawcode = "D001"; caster.position = {128,256,0};
    caster.scale = {1,1,1}; caster.flags = 2; caster.lifePercent = 100; caster.creationId = 1;
    mixed.doodads.push_back(caster); fixture.Set("war3map.doo", Take(mixed.Serialize())); fixture.Open();
    // 真实caster owner先拒绝special/复杂面，不能伪造一个已通过的owner再测试后一道门。
    Rejected(TerrainShadowCasterResources::Capture(*fixture.source, fixture.game), "TERRAIN_SHADOW_SPECIAL_GEOMETRY");
    mixed.specialDoodads.clear(); fixture.terrain.vertices.back().cliffAndLayerRaw = 3;
    fixture.Set("war3map.w3e", Take(SerializeMapTerrain(fixture.terrain)));
    fixture.Set("war3map.doo", Take(mixed.Serialize())); fixture.Open();
    Rejected(TerrainShadowCasterResources::Capture(*fixture.source, fixture.game), "TERRAIN_SHADOW_CLIFF_GEOMETRY");
    SetFlat(fixture); const auto owner = Take(fixture.Capture());
    std::stop_source cancelled; cancelled.request_stop(); Rejected(owner->Sample({}, cancelled.get_token()), "TERRAIN_SHADOW_RECEIVER_CANCELLED");
    Require(Take(fixture.source->ReadMember("war3map.shd")) == Bytes(64 * 64, std::byte{0x7d}),
        "拒绝或取消不发布部分SHD，也不改源");
}
} // namespace SpecialReceiverCases
void RunTerrainShadowSpecialReceiverCases() {
    SpecialReceiverCases::GeometryOrderAndFallback();
    SpecialReceiverCases::OrdinaryRecordsStayBeforeSpecial();
    SpecialReceiverCases::MinimumUsesFullFootprint();
    SpecialReceiverCases::SourcesAndFormalSave();
    SpecialReceiverCases::Guards();
}
} // namespace
