#pragma once

// 在NativeDestructablePathingTests.cpp的原创Fixture和CustomBridge之后包含。
// 仅兼容同类别、完整classic v2且零字段的Original父记录，不推测有字段继承。

void EmptyOriginalParentsKeepDoodadFields() {
    for (const bool nonempty : {false, true}) for (const unsigned childOverride : {0u, 1u, 2u}) {
        Fixture fixture;
        fixture.doodads[0]["pathTex"] = nonempty ? "PathTextures\\parent.tga" : "_";
        fixture.doodads[0]["category"] = "E";
        fixture.doodads[0]["fixedRot"] = "90";
        fixture.Tables(); fixture.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad);
        Check(document.AddOriginalObject("D001"));
        Check(document.AddCustomObject("C001", "D001"));
        if (childOverride != 0) Modification(document, "C001", "Dt00", NativeValueType::String,
            childOverride == 1 ? std::string{} : std::string("PathTextures\\child.tga"));
        fixture.SetNative(document);
        const auto source = fixture.sources;
        const auto actual = Take(fixture.Inspect({"C001"}));
        const bool active = childOverride == 2 || (childOverride == 0 && nonempty);
        Require(actual.footprints.size() == (active ? 1u : 0u),
            "空Original父不改变Custom实际缺省或显式空pathTex");
        if (active) {
            const auto& type = actual.footprints.front();
            Require(type.rawcode == "C001" && type.pathTexture == (childOverride == 2
                    ? "PathTextures\\child.tga" : "PathTextures\\parent.tga")
                && type.fixedRotationDegrees == 90.f
                && type.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells
                && type.contribution == MapData::TerrainNativeFootprintContribution::StaticRgbAttributes,
                "Custom保持新ID及自己的覆盖，未覆盖类别和角度仍取唯一标准父");
        }
        const auto directParent = Take(fixture.Inspect({"D001"}));
        Require(directParent.footprints.size() == (nonempty ? 1u : 0u),
            "同一个空Original标准父也可以直接摆放，不能套用子对象覆盖");
        if (nonempty) Require(directParent.footprints.front().rawcode == "D001"
            && directParent.footprints.front().pathTexture == "PathTextures\\parent.tga",
            "直接摆放的标准父保持自己的标准占地");
        const std::array<std::string_view, 1> special{"C001"};
        const auto specialResult = Take(InspectNativeTerrainObjects({}, special, fixture.sources, kWorkingBytes));
        Require(specialResult.footprints.empty() && specialResult.specialFootprints.size() == 1
            && specialResult.specialFootprints.front().rawcode == "C001"
            && specialResult.specialFootprints.front().pathTexture == (active ? (childOverride == 2
                ? "PathTextures\\child.tga" : "PathTextures\\parent.tga") : ""),
            "特殊模型仍按自己的新ID读取pathTex，空父不增加普通占地或覆盖禁用值");
        Require(Take(fixture.Inspect({"C001"}, actual.workBytes)).workBytes == actual.workBytes,
            "空父兼容不豁免完整来源和输出的精确预算");
        Rejected(fixture.Inspect({"C001"}, actual.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET",
            "空父兼容预算少一字节仍拒绝");
        for (std::size_t i = 0; i < source.size(); ++i)
            Require(source[i].name == fixture.sources[i].name && source[i].present == fixture.sources[i].present
                && source[i].bytes == fixture.sources[i].bytes, "分类保留全部父子记录和来源原字");
    }
}

void EmptyOriginalParentsKeepBridgeAndZeroContributionFields() {
    for (const std::string child : {"N001", "NC01"}) for (const bool overridden : {false, true}) {
        auto fixture = BridgeOriginalFixture();
        auto document = CustomBridge(child);
        Check(document.AddOriginalObject("S001"));
        if (overridden) {
            Modification(document, child, "Pth0", NativeValueType::String, std::string("PathTextures\\child-bridge.tga"));
            Modification(document, child, "Rot0", NativeValueType::Unreal, 0.f);
        }
        fixture.SetNative(document); const auto source = fixture.sources;
        const auto actual = Take(fixture.Inspect({child, "S001"}));
        Require(actual.footprints.size() == 2, "有空Original父的Custom桥和标准父均完整分类");
        const auto childType = std::find_if(actual.footprints.begin(), actual.footprints.end(),
            [&](const auto& type) { return type.rawcode == child; });
        const auto parentType = std::find_if(actual.footprints.begin(), actual.footprints.end(),
            [](const auto& type) { return type.rawcode == "S001"; });
        Require(childType != actual.footprints.end() && parentType != actual.footprints.end()
            && childType->pathTexture == (overridden ? "PathTextures\\child-bridge.tga" : "PathTextures\\SyntheticBridge.tga")
            && childType->fixedRotationDegrees == (overridden ? 0.f : -1.f)
            && childType->gridPolicy == (child == "NC01" ? MapData::TerrainDoodadGridPolicy::StartupCategoryC
                : MapData::TerrainDoodadGridPolicy::TwoCells)
            && childType->contribution == MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression
            && parentType->pathTexture == "PathTextures\\SyntheticBridge.tga"
            && parentType->fixedRotationDegrees == -1.f,
            "空父不混淆Custom三字段覆盖、新ID网格或标准父自己的值");
        Require(Take(fixture.Inspect({child, "S001"}, actual.workBytes)).workBytes == actual.workBytes,
            "桥空父继续预收所有实际源字节和类型结果");
        Rejected(fixture.Inspect({child, "S001"}, actual.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET",
            "桥空父不借工作量不足返回部分类型");
        for (std::size_t i = 0; i < source.size(); ++i)
            Require(source[i].bytes == fixture.sources[i].bytes, "桥分类不丢弃或重写空Original父记录");
    }
    for (const bool explicitZero : {false, true}) {
        auto fixture = BridgeOriginalFixture();
        auto document = CustomBridge("N001", explicitZero ? "S001" : "S002");
        Check(document.AddOriginalObject(explicitZero ? "S001" : "S002"));
        if (explicitZero) Modification(document, "N001", "X001", NativeValueType::Int32, std::int32_t{0});
        fixture.SetNative(document);
        Require(Take(fixture.Inspect({"N001"})).footprints.empty(),
            "空Original父也保留标准false和Custom显式false的零贡献语义");
        Accepted(fixture.Validate({"N001"}), "旧Validate入口共用同一个空父分类实现");
    }
}

void EmptyOriginalParentsStillRejectAnyRealParentField() {
    for (const unsigned field : {0u, 1u, 2u}) {
        Fixture fixture;
        fixture.doodads[0]["pathTex"] = "_"; fixture.Tables(); fixture.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad);
        Check(document.AddOriginalObject("D001")); Check(document.AddCustomObject("C001", "D001"));
        Modification(document, "D001", field == 0 ? "Dt00" : field == 1 ? "Nm00" : "ZZZ0",
            NativeValueType::String, field == 0 ? std::string("_") : std::string("parent"));
        fixture.SetNative(document);
        Rejected(fixture.Inspect({"C001"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE",
            "父相同占地值、纯显示字段和未知字段都不是空Original，不能借当前等价值放行");
    }
    for (const unsigned field : {0u, 1u, 2u, 3u}) {
        auto fixture = BridgeOriginalFixture(); auto document = CustomBridge();
        Check(document.AddOriginalObject("S001"));
        if (field == 0) Modification(document, "S001", "X001", NativeValueType::Int32, std::int32_t{1});
        if (field == 1) Modification(document, "S001", "Pth0", NativeValueType::String, std::string("PathTextures\\SyntheticBridge.tga"));
        if (field == 2) Modification(document, "S001", "Rot0", NativeValueType::Unreal, -1.f);
        if (field == 3) Modification(document, "S001", "Name", NativeValueType::String, std::string("display"));
        fixture.SetNative(document);
        Rejected(fixture.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE",
            "桥父任一实际字段仍拒绝，即使值等于标准或看似仅用于显示");
    }
    auto doodadChain = Fixture(); doodadChain.doodads[0]["pathTex"] = "_";
    doodadChain.Tables(); doodadChain.AddDoodadMetadata();
    auto doodads = Empty(ObjectType::Doodad); Check(doodads.AddOriginalObject("D001"));
    Check(doodads.AddCustomObject("C001", "D001")); Check(doodads.AddCustomObject("C002", "C001"));
    doodadChain.SetNative(doodads);
    Rejected(doodadChain.Inspect({"C002"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE",
        "空Custom父不能伪装成空Original并放开多层继承");
    auto bridgeChain = BridgeOriginalFixture(); auto bridges = CustomBridge();
    Check(bridges.AddOriginalObject("S001")); Check(bridges.AddCustomObject("N002", "N001"));
    bridgeChain.SetNative(bridges);
    Rejected(bridgeChain.Inspect({"N002"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "桥多层空Custom仍拒绝");
}

void EmptyOriginalParentsRequireUniqueCompleteDocuments() {
    for (const bool doodad : {false, true}) {
        auto fixture = BridgeOriginalFixture();
        fixture.doodads[0]["pathTex"] = "_"; fixture.Tables(); fixture.AddDoodadMetadata();
        const auto type = doodad ? ObjectType::Doodad : ObjectType::Destructable;
        const std::string parent = doodad ? "D001" : "S001", child = doodad ? "C001" : "N001";
        auto document = Empty(type); Check(document.AddOriginalObject(parent));
        Check(document.AddCustomObject(child, parent)); fixture.SetNative(document);
        const auto source = fixture.sources;
        const auto slot = doodad ? kW3d : kW3b;
        Take(fixture.Inspect({child}));
        fixture.sources[slot].bytes.pop_back();
        Rejected(fixture.Inspect({child}), {}, "空Original父不能掩盖完整成员截断");
        fixture.sources = source; fixture.sources[slot].bytes.push_back(std::byte{0});
        Rejected(fixture.Inspect({child}), {}, "空Original父不能掩盖未知尾载荷");
        fixture.sources = source; PutId(fixture.sources[slot].bytes, 12, "Bad0");
        Rejected(fixture.Inspect({child}), "native-original-table-new-id", "Original的newID必须真实为零");
        fixture.sources = source;
        Check(document.AddOriginalObject(doodad ? "D002" : "S002")); fixture.SetNative(document);
        // 两条空Original记录各12字节；将第二条原ID改为第一条，必须由完整codec拒绝。
        PutId(fixture.sources[slot].bytes, 20, parent);
        Rejected(fixture.Inspect({child}), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "重复空父也不能建立唯一来源身份");
        fixture.sources = source;
        auto crossClass = Empty(doodad ? ObjectType::Destructable : ObjectType::Doodad);
        Check(crossClass.AddOriginalObject(parent)); fixture.SetNative(crossClass);
        Rejected(fixture.Inspect({child}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "跨类别空父记录仍属类别冲突");
        fixture.sources = source;
        auto missing = Empty(type); Check(missing.AddOriginalObject("None"));
        Check(missing.AddCustomObject(child, "None")); fixture.SetNative(missing);
        Rejected(fixture.Inspect({child}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "空Original不能凭空产生标准父定义");
    }
}

void EmptyOriginalParentsKeepExactStandardInheritance() {
    EmptyOriginalParentsKeepDoodadFields();
    EmptyOriginalParentsKeepBridgeAndZeroContributionFields();
    EmptyOriginalParentsStillRejectAnyRealParentField();
    EmptyOriginalParentsRequireUniqueCompleteDocuments();
}
