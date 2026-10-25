#pragma once

#include "War3MapCore/Text/TextUtil.h"

// 包含于TerrainShadowReceiverResourcesTests.cpp的夹具/帮助函数之后；由集成方登记调用。
// 全部地图、模型和表均为原创。expected来自下述独立地形/像素字面值，
// 不用正式坡道编辑器、分类器、receiver或投影器生成期望。
namespace {
namespace RampReceiverCases {
constexpr std::array<std::string_view, 8> kRampShapes{
    "HLAB", "BALH", "LHBA", "ABHL", "ALHB", "LABH", "BHLA", "HBAL"};

std::string RampPath(std::string_view shape, std::string_view extension = ".mdx") {
    return "Doodads/Terrain/AuthoredRamp/AuthoredRamp" + std::string(shape) + "0" + std::string(extension);
}
Bytes Table(bool second = false) {
    auto value = std::string("ID;PWXL;N;E\nC;X1;Y1;K\"cliffID\"\nC;X2;Y1;K\"cliffModelDir\"\n"
        "C;X3;Y1;K\"rampModelDir\"\nC;X1;Y2;K\"C001\"\nC;X2;Y2;K\"AuthoredCliff\"\n"
        "C;X3;Y2;K\"AuthoredRamp\"\n");
    if (second) value += "C;X1;Y3;K\"C002\"\nC;X2;Y3;K\"AuthoredCliff\"\nC;X3;Y3;K\"OtherRamp\"\n";
    return Text(value + "E\n");
}
void SetTerrain(Fixture& fixture, int dx, int dy, bool corner = false, bool lastMiss = false,
    int centerX = 8, int centerY = 8) {
    auto& terrain = fixture.terrain;
    terrain.header.width = terrain.header.height = 17;
    terrain.vertices.assign(17 * 17, {});
    for (int y = 0; y < 17; ++y) for (int x = 0; x < 17; ++x) {
        auto& vertex = terrain.vertices[std::size_t(y) * 17 + x];
        vertex.groundHeightRaw = vertex.waterHeightAndFlagsRaw = 8192;
        const bool high = corner ? x <= centerX && y <= centerY : (x - centerX) * dx + (y - centerY) * dy <= 0;
        vertex.cliffAndLayerRaw = high ? 3 : 2;
    }
    if (corner) {
        for (int x = centerX; x <= centerX + 2; ++x) for (int y = centerY; y <= centerY + 2; ++y)
            terrain.vertices[std::size_t(y) * 17 + x].textureAndFlagsRaw = 0x10;
    } else {
        for (int step = 0; step <= 2; ++step)
            terrain.vertices[std::size_t(centerY + step * dy) * 17 + centerX + step * dx].textureAndFlagsRaw = 0x10;
    }
    RefreshTerrainDerived(terrain, 0, 0, 17, 17);
    fixture.Set("war3map.w3e", Take(SerializeMapTerrain(terrain)));
    fixture.Set("war3map.shd", Bytes(64 * 64, std::byte{0x7d}));
    fixture.Set("TerrainArt/CliffTypes.slk", Table());
    // 每个一级普通cliff字面形状都有原版目录成员，避免以低层假路径救场。
    for (const auto shape : {"AAAB", "AABA", "AABB", "ABAA", "ABAB", "ABBA", "ABBB",
        "BAAA", "BAAB", "BABA", "BABB", "BBAA", "BBAB", "BBBA"})
        fixture.Set("Doodads/Terrain/AuthoredCliff/AuthoredCliff" + std::string(shape) + "0.mdx", Model(128));
    for (const auto shape : kRampShapes) fixture.Set(RampPath(shape), Model(64, lastMiss));
    fixture.Open();
}

void FourDirectionsAndAnchorIdentity() {
    struct Case { int dx, dy, modelX, modelY; std::string_view first, second; };
    constexpr std::array<Case, 4> cases{{
        {1,0,8,7,"HLAB","BALH"}, {-1,0,6,7,"LHBA","ABHL"},
        {0,1,7,8,"ALHB","LABH"}, {0,-1,7,6,"BHLA","HBAL"}
    }};
    for (const auto& item : cases) {
        Fixture fixture(true); SetTerrain(fixture, item.dx, item.dy);
        const auto owner = Take(fixture.Capture());
        Require(owner->RecordCount() == 16, "16格台阶仅以两个anchor替代原cliff，不为alternate重复建record");
        std::vector<std::string> actualModels;
        for (const auto& dependency : owner->Dependencies())
            if (dependency.source == TerrainShadowCasterSource::Map
                && dependency.name.starts_with("doodads/terrain/authoredramp/")
                && dependency.name.ends_with(".mdx")) actualModels.push_back(dependency.name);
        std::vector<std::string> expected{Text::LowerAscii(RampPath(item.first)), Text::LowerAscii(RampPath(item.second))};
        std::sort(expected.begin(), expected.end());
        Require(actualModels == expected, "实际anchor方向、R改L、ramp字段和重复目录必须精确，不能改用cliff或alternate");
        const auto grid = Take(owner->Sample());
        const auto sample = std::size_t(item.modelY * 4) * 64 + item.modelX * 4;
        // 大于768的整数world坐标使本夹具的-90度F32残差落回整数。
        // 原创平面64、竖线原交距2496，独立结果F32(2560-2496+0.1)。
        RequireF32At(grid.receiverHeights[sample], 0x42803333u, sample,
            "正负方向必须用原表平移，首anchor闭区间覆盖实际ramp模型格");
        Check(owner->VerifySources(*fixture.source));
    }
}

void AlternateFallbackAndGroundSHeight() {
    Fixture fixture(true); SetTerrain(fixture, 1, 0);
    const auto grid = Take(Take(fixture.Capture())->Sample());
    const auto at = [](unsigned x, unsigned y) { return std::size_t(y) * 64 + x; };
    // RLAA/AALR不是anchor，但仍走首包含record。模型仅覆盖第一格，
    // alternate射线miss后最近R顶点为64，最近L顶点为0。
    RequireF32At(grid.receiverHeights[at(36,30)], 0x42803333u, at(36,30), "alternate必须选前一格anchor并按R高度回退");
    RequireF32At(grid.receiverHeights[at(39,30)], 0x3dcccccdu, at(39,30), "alternate最近L顶点回退为0，不能整格套64");
    SetTerrain(fixture, 1, 0, false, true);
    const auto missed = Take(Take(fixture.Capture())->Sample());
    RequireF32At(missed.receiverHeights[at(32,28)], 0x4300199au, at(32,28),
        "最后未知组false必须回退高端128，不能把前一个组命中当作any-hit");

    Fixture corner(true); SetTerrain(corner, 1, 1, true);
    const auto cornerGrid = Take(Take(corner.Capture())->Sample());
    // 中心格NW/NE/SE/SW=R/S/R/H，不匹配模型模式，必须走完整block ground。
    // 独立高度SW=128、SE=64、NW=64、NE=32，SE-NW对角线不交换。
    // (u,v)=(1/8,1/8)为112；(7/8,7/8)为40。
    RequireF32At(cornerGrid.receiverHeights[at(32,32)], 0x42e03333u, at(32,32), "含S地格走ground且保留R偏移");
    RequireF32At(cornerGrid.receiverHeights[at(35,35)], 0x42206666u, at(35,35), "真实S32必须进入基础网格，不能套显示高度或64常量");
}

void BlockOwnershipAndHighestLayerSlot() {
    struct Boundary { int dx, dy, centerX, centerY, alternateX, alternateY; };
    constexpr std::array<Boundary, 4> cases{{
        {1,0,7,8,8,7}, {-1,0,9,8,7,7}, {0,1,8,7,7,8}, {0,-1,8,9,7,7}
    }};
    for (const auto& item : cases) {
        Fixture fixture(true); SetTerrain(fixture, item.dx, item.dy, false, false, item.centerX, item.centerY);
        const auto owner = Take(fixture.Capture());
        Require(owner->RecordCount() == 16, "跨4格block不能复制anchor record或改变原建表顺序");
        const auto grid = Take(owner->Sample());
        const auto index = std::size_t(item.alternateY * 4) * 64 + item.alternateX * 4;
        // 原版只查query block；邻block没有自己的包含record时返回0，
        // 即使原anchor的有符号闭区间覆盖此alternate，也不跨block借用或回退。
        RequireF32At(grid.receiverHeights[index], 0x3dcccccdu, index,
            "跨block的alternate无候选返回0.1，不能复制record或跨block找最近R高度");
    }
    Fixture highest(true); SetTerrain(highest, -1, 0);
    highest.terrain.header.cliffTilesets.push_back("C002");
    // anchor(7,7)的SW是低层槽0、SE是高层槽1，严格最高层选择必须取SE。
    highest.terrain.vertices[7 * 17 + 8].cliffAndLayerRaw |= 0x10;
    highest.Set("war3map.w3e", Take(SerializeMapTerrain(highest.terrain)));
    highest.Set("TerrainArt/CliffTypes.slk", Table(true));
    const std::string selected = "Doodads/Terrain/OtherRamp/OtherRampLHBA0.mdx";
    highest.Set(selected, Model(80)); highest.Open();
    const auto owner = Take(highest.Capture());
    Require(std::any_of(owner->Dependencies().begin(), owner->Dependencies().end(), [&](const auto& dependency) {
        return dependency.name == Text::LowerAscii(selected) && dependency.source == TerrainShadowCasterSource::Map;
    }), "ramp槽不能取首个非15顶点；须取实际五格最高层首顶点的槽");
    const auto grid = Take(owner->Sample());
    RequireF32At(grid.receiverHeights[28 * 64 + 24], 0x42a03333u, 28 * 64 + 24,
        "实际被选的第二ramp目录80高模型必须参与采样");
}

void SourcesAndProductionSave() {
    Fixture fixture(true); SetTerrain(fixture, 1, 0);
    const auto owner = Take(fixture.Capture());
    MapSaveTask task; Check(task.FreezeArchive(*fixture.source));
    task.target = fixture.root / "authored-ramp-shadow.w3x";
    task.prepareFinalDerived = [&](MapSaveTask& current) {
        Require(current.InFinalDerivedStage(), "坡道阴影必须经过正式最终派生阶段");
        std::shared_ptr<const TerrainSavePlan> plan;
        const auto status = PrepareTerrainShadowBake(current, fixture.game, nullptr, plan);
        Require(!plan, "只重算坡道阴影不能伪造地形编辑计划");
        return status;
    };
    MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && task.terrainShadow, "真实显式阴影保存应生成完整结果");
    Check(task.terrainShadow->VerifyReadback(*task.nextSession));
    Check(owner->VerifyReadback(*task.nextSession));
    for (const auto& member : fixture.members) if (member.name != "war3map.shd")
        Require(Take(task.nextSession->ReadMember(member.name)) == member.data, "计算坡道SHD不改其它原创成员");
    const std::array<MemberReplacement, 1> changed{{{RampPath("HLAB"), Model(65)}}};
    Rejected(owner->VerifySources(*fixture.source, changed), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    const std::array<MemberReplacement, 1> mdl{{{RampPath("HLAB", ".mdl"), Text("authored preferred mdl")}}};
    Rejected(owner->VerifySources(*fixture.source, mdl), "TERRAIN_SHADOW_RECEIVER_DEPENDENCY_STALE");
    Rejected(fixture.Capture(mdl), "TERRAIN_SHADOW_RECEIVER_MDL_UNSUPPORTED");
    // 固定variation0，不探测或消费variation1；改变无关成员不得变更该owner。
    const std::array<MemberReplacement, 1> unused{{{"Doodads/Terrain/AuthoredRamp/AuthoredRampHLAB1.mdx", Model(200)}}};
    Check(owner->VerifySources(*fixture.source, unused));
    fixture.Erase(RampPath("HLAB"));
    fixture.Set("Doodads/Terrain/AuthoredRampHLAB0.mdx", Model(64)); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_RECEIVER_MODEL_MISSING");
    Require(Take(fixture.source->ReadMember("war3map.shd")) == Bytes(64 * 64, std::byte{0x7d}),
        "缺实际目录时不写输出，也不能消费旧假路径");
}

void LimitsAndIncompleteR() {
    Fixture fixture(true); SetTerrain(fixture, 1, 0);
    auto limits = TerrainShadowReceiverLimits{}; limits.maximumWork = 100;
    Rejected(fixture.Capture({}, limits), "TERRAIN_SHADOW_RECEIVER_BUDGET");
    limits = {}; limits.maximumRecords = 1;
    Rejected(fixture.Capture({}, limits), "TERRAIN_SHADOW_RECEIVER_BUDGET");
    fixture.terrain.vertices[8 * 17 + 9].textureAndFlagsRaw = 0;
    fixture.Set("war3map.w3e", Take(SerializeMapTerrain(fixture.terrain))); fixture.Open();
    Rejected(fixture.Capture(), "TERRAIN_SHADOW_RAMP_GEOMETRY");
}
} // namespace RampReceiverCases
void RunTerrainShadowRampReceiverCases() {
    RampReceiverCases::FourDirectionsAndAnchorIdentity();
    RampReceiverCases::AlternateFallbackAndGroundSHeight();
    RampReceiverCases::BlockOwnershipAndHighestLayerSlot();
    RampReceiverCases::SourcesAndProductionSave();
    RampReceiverCases::LimitsAndIncompleteR();
}
} // namespace
