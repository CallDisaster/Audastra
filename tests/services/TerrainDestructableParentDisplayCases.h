#pragma once

void DestructableParentDisplayFieldsKeepPathingAndIdentity() {
    for (const bool bridge : {false, true}) {
        auto fixture = bridge ? BridgeOriginalFixture() : Fixture{};
        const std::string parent = bridge ? "S001" : "S002";
        const std::array<std::array<const char*, 3>, 7> fields{{
            {"Nm00", "Name", "string"}, {"Lo00", "minScale", "unreal"}, {"Hi00", "maxScale", "unreal"},
            {"Ts00", "tilesets", "tilesetList"}, {"Cr00", "colorR", "int"},
            {"Cg00", "colorG", "int"}, {"Cb00", "colorB", "int"}}};
        for (const auto& field : fields) fixture.metadata.push_back({{"ID", field[0]}, {"field", field[1]},
            {"type", field[2]}, {"slk", "DestructableData"}, {"index", "0"}});
        fixture.Tables();
        auto document = Empty();
        Check(document.AddOriginalObject(parent));
        Modification(document, parent, "Nm00", NativeValueType::String, std::string("synthetic parent"));
        Modification(document, parent, "Lo00", NativeValueType::Unreal, .8f);
        Modification(document, parent, "Hi00", NativeValueType::Unreal, 1.2f);
        Modification(document, parent, "Ts00", NativeValueType::String, std::string("A,B"));
        for (const auto* field : {"Cr00", "Cg00", "Cb00"})
            Modification(document, parent, field, NativeValueType::Int32, std::int32_t{175});
        Check(document.AddCustomObject("C001", parent));
        Check(document.AddCustomObject("C002", parent));
        fixture.SetNative(document);
        const auto wire = fixture.sources[kW3b].bytes;
        const auto actual = Take(fixture.Inspect({"C001", "C002"}));
        Require(fixture.sources[kW3b].bytes == wire, "父展示字段验证只读，不规范化或合并原生记录");
        Require(actual.footprints.size() == (bridge ? 2u : 0u), "展示覆盖不改变零贡献/桥的通行分类");
        for (const auto& type : actual.footprints) {
            Require(type.pathTexture == "PathTextures\\SyntheticBridge.tga" && type.fixedRotationDegrees == -1.f
                && type.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells
                && type.contribution == MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression,
                "子对象保留其wire类别、标准父占地和旋转，不使用展示比例代替实例比例");
        }
        Rejected(fixture.Inspect({"C001", "C002"}, actual.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET",
            "父字段验证和缓存计入剩余预算，不借重复引用逃逸");
    }
    for (int mode = 0; mode < 7; ++mode) {
        Fixture fixture;
        fixture.metadata.push_back({{"ID", "Cr00"}, {"field", "colorR"}, {"type", "int"},
            {"slk", "DestructableData"}, {"index", "0"}});
        auto document = Empty(); Check(document.AddOriginalObject("S002"));
        if (mode == 0) Modification(document, "S002", "X001", NativeValueType::Int32, std::int32_t{0});
        else if (mode == 1) Modification(document, "S002", "Nope", NativeValueType::Int32, std::int32_t{0});
        else if (mode == 2) Modification(document, "S002", "Cr00", NativeValueType::String, std::string("175"));
        else {
            Modification(document, "S002", "Cr00", NativeValueType::Int32, std::int32_t{mode == 3 ? 256 : 175});
            if (mode == 4) fixture.metadata.back()["index"] = "1";
            if (mode == 5) fixture.metadata.back()["useSpecific"] = "S002";
            if (mode == 6) { auto alias = fixture.metadata.back(); alias["ID"] = "Cr01"; fixture.metadata.push_back(alias); }
        }
        Check(document.AddCustomObject("C001", "S002")); fixture.Tables(); fixture.SetNative(document);
        Rejected(fixture.Inspect({"C001"}), mode == 0 || mode == 1 ? "TERRAIN_NATIVE_OBJECT_INHERITANCE"
            : (mode == 2 || mode == 3) ? "TERRAIN_NATIVE_OBJECT_PARENT_TYPE" : "TERRAIN_NATIVE_OBJECT_METADATA",
            "父相关字段、未知、错误wire、越界颜色、索引、selector及别名仍明确拒绝");
    }
}
