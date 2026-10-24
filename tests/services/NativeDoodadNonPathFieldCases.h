// 在NativeDestructablePathingTests.cpp的原创Fixture定义之后、匿名命名空间内包含。
// 只读颜色变体不开放物编写回；输入从现有经典v2 codec构造。
Fixture DoodadColorFixture(bool footprint = true) {
    Fixture fixture;
    fixture.doodads[0]["pathTex"] = footprint ? "PathTextures\\SyntheticColor.tga" : "_";
    fixture.doodads[0]["category"] = "E";
    fixture.doodads[0]["fixedRot"] = "180";
    fixture.doodadMetadata.push_back({{"ID", "ClrR"}, {"field", "vertR"}, {"slk", "DoodadData"},
        {"type", "int"}, {"repeat", "10"}, {"index", "0"}});
    fixture.doodadMetadata.push_back({{"ID", "ClrG"}, {"field", "vertG"}, {"slk", "DoodadData"},
        {"type", "int"}, {"repeat", "10"}, {"index", "0"}});
    fixture.doodadMetadata.push_back({{"ID", "ClrB"}, {"field", "vertB"}, {"slk", "DoodadData"},
        {"type", "int"}, {"repeat", "10"}, {"index", "0"}});
    fixture.doodadMetadata.push_back({{"ID", "Cat0"}, {"field", "category"}, {"slk", "DoodadData"},
        {"type", "doodadCategory"}, {"repeat", "0"}, {"index", "0"}});
    fixture.doodadMetadata.push_back({{"ID", "Rot0"}, {"field", "fixedRot"}, {"slk", "DoodadData"},
        {"type", "unreal"}, {"repeat", "0"}, {"index", "0"}});
    fixture.Tables(); fixture.AddDoodadMetadata();
    return fixture;
}

void DoodadColorModification(NativeObjectDocument& document, const std::string& rawcode,
    const std::string& field, std::uint32_t variation, std::uint32_t data = 0,
    NativeValueType type = NativeValueType::Int32, Value value = std::int32_t{127}) {
    ModificationKey key{field}; key.variation = variation; key.dataIndex = data;
    Check(document.AddModification({ObjectType::Doodad, rawcode}, key, type, std::move(value)));
}

void DoodadRepeatedColorsDoNotChangeFootprints() {
    for (const bool custom : {false, true}) for (const bool footprint : {false, true}) {
        auto fixture = DoodadColorFixture(footprint);
        auto document = Empty(ObjectType::Doodad);
        const std::string id = custom ? "XC01" : "D001";
        if (custom) Check(document.AddCustomObject(id, "D001"));
        else Check(document.AddOriginalObject(id));
        for (const std::string field : {"ClrR", "ClrG", "ClrB"})
            for (std::uint32_t variation = 1; variation <= 10; ++variation)
                DoodadColorModification(document, id, field, variation, 0, NativeValueType::Int32,
                    std::int32_t(variation * 17));
        // 同一对象上的标量仍参与有效占地，不因先命中无关颜色而跳过。
        Modification(document, id, "Cat0", NativeValueType::String, std::string("B"));
        Modification(document, id, "Rot0", NativeValueType::Unreal, 90.f);
        Modification(document, id, "Nm00", NativeValueType::String, std::string("color fixture"));
        fixture.SetNative(document);
        const auto before = fixture.sources;
        const auto inspected = Take(fixture.Inspect({id}));
        Require(inspected.footprints.size() == (footprint ? 1u : 0u),
            "Original/Custom的30条颜色变体不产生或删除占地");
        if (footprint) {
            const auto& type = inspected.footprints.front();
            Require(type.rawcode == id && type.pathTexture == "PathTextures\\SyntheticColor.tga"
                && type.fixedRotationDegrees == 90.f && type.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells,
                "有效pathTex/category/fixedRot不被无关变体取代");
        }
        Require(Take(fixture.Inspect({id}, inspected.workBytes)).workBytes == inspected.workBytes,
            "颜色记录沿用实际输入的精确累计工作预算");
        Rejected(fixture.Inspect({id}, inspected.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET",
            "颜色记录预算少一字节不能返回部分结果");
        for (std::size_t i = 0; i < before.size(); ++i)
            Require(fixture.sources[i].name == before[i].name && fixture.sources[i].present == before[i].present
                && fixture.sources[i].bytes == before[i].bytes, "只读颜色捕获保留全部来源原字节");
    }
    auto fixture = DoodadColorFixture();
    auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    DoodadColorModification(document, "D001", "ClrR", 1);
    Modification(document, "D001", "Dt00", NativeValueType::String, std::string{});
    fixture.SetNative(document);
    Require(Take(fixture.Inspect()).footprints.empty(), "颜色与显式空pathTex共存时保持清空语义");
    Modification(document, "D001", "ZZZ0", NativeValueType::Int32, std::int32_t{0});
    fixture.SetNative(document);
    Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "合法颜色后仍拒绝未知字段，不能整段continue放行");
}

void DoodadRepeatedColorsKeepExactAddressAndWireTypes() {
    for (const std::uint32_t variation : {0u, 11u, 2147483647u}) {
        auto fixture = DoodadColorFixture();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        DoodadColorModification(document, "D001", "ClrR", variation); fixture.SetNative(document);
        Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "颜色只接受实际1..10变体域");
    }
    for (const auto type : {NativeValueType::Real, NativeValueType::Unreal, NativeValueType::String}) {
        auto fixture = DoodadColorFixture();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        Value value = type == NativeValueType::String ? Value{std::string("127")} : Value{127.f};
        DoodadColorModification(document, "D001", "ClrR", 1, 0, type, std::move(value)); fixture.SetNative(document);
        Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "颜色保持实际Int32，不能借文本或F32转换");
    }
    auto fixture = DoodadColorFixture();
    auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    DoodadColorModification(document, "D001", "ClrR", 1, 1); fixture.SetNative(document);
    Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_DOODAD_TYPE", "颜色第二索引必须保持实际零值");
    document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    DoodadColorModification(document, "D001", "clrr", 1); fixture.SetNative(document);
    Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "颜色字段ID仍区分大小写");
}

void DoodadRepeatedColorsRequireActualUniqueMetadata() {
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"repeat", "0"}, {"repeat", "1"}, {"repeat", "11"}, {"repeat", "10garbage"},
        {"index", "1"}, {"index", ""}, {"data", "1"}, {"data", "bad"},
        {"appendindex", "1"}, {"effectindex", "1"}, {"effecttype", "x"},
        {"type", "bool"}, {"type", "real"}, {"type", "string"},
        {"slk", "Profile"}, {"slk", "Doodads"}, {"field", "otherColor"},
        {"useSpecific", "D001"}, {"notSpecific", "ZZZZ"}, {"useSpecific", "_"}}) {
        auto fixture = DoodadColorFixture(); fixture.doodadMetadata[2][key] = value; fixture.Tables();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        DoodadColorModification(document, "D001", "ClrR", 1); fixture.SetNative(document);
        Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "变体只有实际已证地址；不猜其它重复字段或选择器");
    }
    for (const std::string key : {"repeat", "index"}) {
        auto fixture = DoodadColorFixture(); fixture.doodadMetadata[2].erase(key); fixture.Tables();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        DoodadColorModification(document, "D001", "ClrR", 1); fixture.SetNative(document);
        Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "颜色repeat/index不能由loader默认值补证");
    }
    for (const std::string table : {"DoodadData", ""}) for (const std::string source : {"vertR", "vertR01", "vertR1", "vert"}) {
        auto fixture = DoodadColorFixture();
        Row alias{{"ID", "Als0"}, {"field", source}, {"slk", table}, {"type", "int"}, {"repeat", "0"}, {"index", "0"}};
        if (source == "vert") alias["data"] = "18";
        fixture.doodadMetadata.push_back(alias); fixture.Tables();
        auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
        DoodadColorModification(document, "D001", "ClrR", 1); fixture.SetNative(document);
        Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_METADATA", "重复原始字段、变体列或data拼接别名包括隐藏行仍拒绝");
    }
}

void DoodadRepeatedColorsStillRequireCompleteSourcesAndRecords() {
    auto fixture = DoodadColorFixture();
    auto document = Empty(ObjectType::Doodad); Check(document.AddOriginalObject("D001"));
    DoodadColorModification(document, "D001", "ClrR", 1);
    DoodadColorModification(document, "D001", "ClrR", 2); fixture.SetNative(document);
    const auto valid = fixture.sources[kW3d].bytes;
    // 两个Int32记录分别从20、44开始；wire变体分别位于28、52。
    Put32(fixture.sources[kW3d].bytes, 52, 1);
    Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "重复字段/变体/零data地址仍拒绝完整对象文档");
    fixture.sources[kW3d].bytes = valid; fixture.sources[kW3d].bytes.pop_back();
    Rejected(fixture.Inspect(), {}, "已知颜色不能掩盖截断W3D");
    fixture.sources[kW3d].bytes = valid; fixture.sources[kW3d].bytes.push_back(std::byte{0});
    Rejected(fixture.Inspect(), {}, "已知颜色不能掩盖未知尾载荷");
    fixture.sources[kW3d].bytes = valid;
    fixture.sources.pop_back();
    Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_DOODAD_METADATA_REQUIRED", "相关颜色记录仍需要实际第六项元数据来源");
    fixture.AddDoodadMetadata(); fixture.sources.back().present = false; fixture.sources.back().bytes.clear();
    Rejected(fixture.Inspect(), "TERRAIN_NATIVE_OBJECT_TABLE_MISSING", "实际元数据Missing不能借旧表或默认值放行");
}

void DoodadRepeatedColorFieldsPreservePathing() {
    DoodadRepeatedColorsDoNotChangeFootprints();
    DoodadRepeatedColorsKeepExactAddressAndWireTypes();
    DoodadRepeatedColorsRequireActualUniqueMetadata();
    DoodadRepeatedColorsStillRequireCompleteSourcesAndRecords();
}
