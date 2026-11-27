#pragma once

// 原创父子对象与真实地址形态，不包含私有地图记录。
void DoodadParentDisplayFieldsRemainTypedAndBounded() {
    for (const bool emptyPath : {false, true}) {
        Fixture fixture;
        fixture.doodads[0]["pathTex"] = emptyPath ? "_" : "PathTextures\\display-parent.tga";
        fixture.doodads[0]["category"] = "E"; fixture.doodads[0]["fixedRot"] = "90";
        fixture.doodadMetadata.push_back({{"ID", "Cp00"}, {"field", "canPlaceRandScale"},
            {"slk", "DoodadData"}, {"type", "bool"}, {"repeat", "0"}, {"index", "0"}});
        fixture.doodadMetadata.push_back({{"ID", "Sc00"}, {"field", "defScale"},
            {"slk", "DoodadData"}, {"type", "unreal"}, {"repeat", "0"}, {"index", "0"}});
        fixture.doodadMetadata.push_back({{"ID", "Ca00"}, {"field", "category"},
            {"slk", "DoodadData"}, {"type", "doodadCategory"}, {"repeat", "0"}, {"index", "0"}});
        fixture.doodadMetadata.push_back({{"ID", "Rt00"}, {"field", "fixedRot"},
            {"slk", "DoodadData"}, {"type", "unreal"}, {"repeat", "0"}, {"index", "0"}});
        fixture.Tables(); fixture.AddDoodadMetadata();
        auto document = Empty(ObjectType::Doodad);
        Check(document.AddOriginalObject("D001"));
        Modification(document, "D001", "Cp00", NativeValueType::Int32, std::int32_t{0});
        Modification(document, "D001", "Sc00", NativeValueType::Unreal, 2.f);
        for (const auto* child : {"C001", "C002"}) {
            Check(document.AddCustomObject(child, "D001"));
            Modification(document, child, "Cp00", NativeValueType::Int32, std::int32_t{1});
            Modification(document, child, "Nm00", NativeValueType::String, std::string("child display"));
        }
        fixture.SetNative(document); const auto source = fixture.sources[kW3d].bytes;
        const auto actual = Take(fixture.Inspect({"C001", "C002", "D001"}));
        Require(actual.footprints.size() == (emptyPath ? 0u : 3u),
            "同父两个Custom和直接摆放的Original均完整分类");
        for (const auto& type : actual.footprints)
            Require(type.pathTexture == "PathTextures\\display-parent.tga" && type.fixedRotationDegrees == 90.f
                && type.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells,
                "父显示缩放不替代实例尺寸、标准旋转、类别或占地");
        const std::array<std::string_view, 1> special{"C001"};
        const auto specialResult = Take(InspectNativeTerrainObjects({}, special, fixture.sources, kWorkingBytes));
        Require(specialResult.specialFootprints.size() == 1
            && specialResult.specialFootprints.front().pathTexture == (emptyPath ? "" : "PathTextures\\display-parent.tga"),
            "特殊模型共用完整父字段验证，不改变自己的占地来源");
        Require(Take(fixture.Inspect({"C001", "C002", "D001"}, actual.workBytes)).workBytes == actual.workBytes,
            "共享父验证缓存计入精确累计预算");
        Rejected(fixture.Inspect({"C001", "C002", "D001"}, actual.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET",
            "父验证缓存不绕过预算少一字节的拒绝");
        Require(fixture.sources[kW3d].bytes == source, "父子所有原生字段逐字保留");

        // 与游戏一致：Custom 不继承地图对标准父的修改。父改了 pathTex、类别或旋转，Custom 仍取标准值；
        // 父的未知字段仍拒绝。
        const auto standardFootprint = [&](const NativeObjectDocument& changed, const char* label) {
            fixture.SetNative(changed);
            const auto inspected = Take(fixture.Inspect({"C001"}));
            Require(inspected.footprints.size() == (emptyPath ? 0u : 1u), label);
            for (const auto& type : inspected.footprints)
                Require(type.pathTexture == "PathTextures\\display-parent.tga" && type.fixedRotationDegrees == 90.f
                    && type.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells, label);
        };
        auto parentPath = document;
        Modification(parentPath, "D001", "Dt00", NativeValueType::String,
            std::string(emptyPath ? "PathTextures\\parent-override.tga" : "_"));
        standardFootprint(parentPath, "父的pathTex地图修改不继承：Custom仍取标准占地");
        auto unknown = document;
        Modification(unknown, "D001", "ZZZ0", NativeValueType::String, std::string("parent"));
        fixture.SetNative(unknown);
        Rejected(fixture.Inspect({"C001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "父的未知字段仍拒绝");
        for (const bool rotation : {false, true}) {
            auto changed = document;
            Modification(changed, "D001", rotation ? "Rt00" : "Ca00",
                rotation ? NativeValueType::Unreal : NativeValueType::String,
                rotation ? Value{45.f} : Value{std::string("C")});
            standardFootprint(changed, "父的类别和旋转地图修改不继承：Custom仍取标准类别与旋转");
        }
        auto wrongType = document;
        const auto* parent = wrongType.FindObject({ObjectType::Doodad, "D001"});
        Require(parent, "坏型夹具有完整Original父");
        const auto scale = std::find_if(parent->modifications.begin(), parent->modifications.end(),
            [](const auto& modification) { return modification.key.fieldId == "Sc00"; });
        Require(scale != parent->modifications.end(), "坏型夹具定位实际正规化字段身份");
        const auto scaleKey = scale->key;
        Check(wrongType.RemoveModification({ObjectType::Doodad, "D001"}, scaleKey));
        Modification(wrongType, "D001", "Sc00", NativeValueType::String, std::string("2"));
        fixture.SetNative(wrongType);
        Rejected(fixture.Inspect({"C001"}), "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "父展示字段也要求真实wire类型");
        auto wrongIndex = document;
        Check(wrongIndex.RemoveModification({ObjectType::Doodad, "D001"}, scaleKey));
        auto variedScale = scaleKey; variedScale.variation = 1;
        Check(wrongIndex.AddModification({ObjectType::Doodad, "D001"}, variedScale, NativeValueType::Unreal, 2.f));
        fixture.SetNative(wrongIndex);
        Rejected(fixture.Inspect({"C001"}), "TERRAIN_NATIVE_OBJECT_METADATA", "父显示标量不能借用非零变体");
    }
}
