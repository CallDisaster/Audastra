#pragma once

// 在NativeDestructablePathingTests.cpp的Fixture/BridgeOriginalFixture/CustomBridge后包含。
// 字段ID全部原创；用实际逻辑Name绑定验证保留，不依赖原版bnam字面值。
Fixture BridgeNameFixture(const std::string& field = "Nm90") {
    auto fixture = BridgeOriginalFixture();
    fixture.metadata.push_back({{"ID", field}, {"field", "Name"}, {"slk", "DestructableData"},
        {"type", "string"}, {"index", "0"}});
    fixture.Tables();
    return fixture;
}

NativeObjectDocument BridgeNameDocument(bool custom, const std::string& field, const std::string& name) {
    auto document = custom ? CustomBridge() : Empty();
    if (!custom) Check(document.AddOriginalObject("S001"));
    Modification(document, custom ? "N001" : "S001", field, NativeValueType::String, name);
    return document;
}

void BridgeNamesKeepTheActualStaticContribution() {
    for (const bool custom : {false, true}) for (const std::string field : {"Nm90", "Dsp8"})
        for (const std::string name : {std::string{}, std::string("bridge;\"literal\""), std::string("桥的名称")}) {
            auto fixture = BridgeNameFixture(field);
            const std::string placed = custom ? "N001" : "S001";
            auto document = BridgeNameDocument(custom, field, name);
            if (custom) Check(document.AddOriginalObject("S001"));
            fixture.SetNative(document);
            const auto source = fixture.sources;
            const auto actual = Take(fixture.Inspect({placed, placed}));
            Require(actual.footprints.size() == 1 && actual.specialFootprints.empty(),
                "Original及一层Custom的Name只保留一个实际桥描述，不制造普通或特殊占地");
            const auto& bridge = actual.footprints.front();
            Require(bridge.rawcode == placed && bridge.pathTexture == "PathTextures\\SyntheticBridge.tga"
                && bridge.fixedRotationDegrees == -1.f && bridge.gridPolicy == MapData::TerrainDoodadGridPolicy::TwoCells
                && bridge.contribution == MapData::TerrainNativeFootprintContribution::WalkableTerrainSuppression,
                "Name的空串、中文及字面引号不改变实际pathTex/fixedRot/walkable和新ID网格");
            for (std::size_t i = 0; i < source.size(); ++i)
                Require(source[i].name == fixture.sources[i].name && source[i].present == fixture.sources[i].present
                    && source[i].bytes == fixture.sources[i].bytes, "Name分类保持完整来源及原生记录原字");
            Require(Take(fixture.Inspect({placed, placed}, actual.workBytes)).workBytes == actual.workBytes,
                "Name及完整metadata继续计入同一精确解析预算");
            Rejected(fixture.Inspect({placed, placed}, actual.workBytes - 1), "TERRAIN_NATIVE_OBJECT_BUDGET",
                "Name不能豁免少一字节的累计预算");
        }
    for (const bool custom : {false, true}) {
        auto fixture = BridgeNameFixture(); const std::string placed = custom ? "N001" : "S001";
        fixture.destructables[0]["walkable"] = "0";
        fixture.destructables[0]["pathTex"] = "_";
        fixture.destructables[0]["fixedRot"] = "invalid fallback"; fixture.Tables();
        auto document = BridgeNameDocument(custom, "Nm90", "display");
        Modification(document, placed, "X001", NativeValueType::Int32, std::int32_t{1});
        Modification(document, placed, "Pth0", NativeValueType::String, std::string("PathTextures\\effective.tga"));
        Modification(document, placed, "Rot0", NativeValueType::Unreal, 0.f); fixture.SetNative(document);
        const auto actual = Take(fixture.Inspect({placed}));
        Require(actual.footprints.size() == 1 && actual.footprints.front().pathTexture == "PathTextures\\effective.tga"
            && actual.footprints.front().fixedRotationDegrees == 0.f,
            "Name可以与三项实际覆盖共存，不能将矛盾标准值当作真正通行来源");
    }
    auto category = BridgeNameFixture(); auto custom = CustomBridge("NC01");
    Modification(custom, "NC01", "Nm90", NativeValueType::String, std::string("non-C display")); category.SetNative(custom);
    Require(Take(category.Inspect({"NC01"})).footprints.front().gridPolicy == MapData::TerrainDoodadGridPolicy::StartupCategoryC,
        "Name不会把新ID的大写C网格请求改成显示文字或父ID网格");
}

void BridgeNamesRequireTheFullUniqueScalarMetadata() {
    for (const bool custom : {false, true}) {
        const std::string placed = custom ? "N001" : "S001";
        for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
            {"slk", "Profile"}, {"slk", "DoodadData"}, {"slk", ""},
            {"type", "int"}, {"type", "unreal"}, {"type", "pathingTexture"},
            {"index", "1"}, {"index", "-1"}, {"index", "broken"}, {"index", "\"0\""},
            {"repeat", "1"}, {"data", "1"}, {"appendindex", "1"}, {"effectindex", "1"},
            {"field", "\"Name\""}}) {
            auto fixture = BridgeNameFixture(); fixture.metadata.back()[key] = value; fixture.Tables();
            fixture.SetNative(BridgeNameDocument(custom, "Nm90", "display"));
            Rejected(fixture.Inspect({placed}), "TERRAIN_NATIVE_OBJECT_METADATA",
                "Name只接受实际DestructableData/index0/String，不吞掉表、索引、类型或二次解码错误");
        }
        auto missingIndex = BridgeNameFixture(); missingIndex.metadata.back().erase("index"); missingIndex.Tables();
        missingIndex.SetNative(BridgeNameDocument(custom, "Nm90", "display"));
        Rejected(missingIndex.Inspect({placed}), "TERRAIN_NATIVE_OBJECT_METADATA", "缺失Name索引不能猜成0");
        for (const auto& [key, value] : std::array{
            std::pair{"useSpecific", "S001"}, std::pair{"notSpecific", "N001"}}) {
            auto selector = BridgeNameFixture(); selector.metadata.back()[key] = value; selector.Tables();
            selector.SetNative(BridgeNameDocument(custom, "Nm90", "display"));
            Rejected(selector.Inspect({placed}), "TERRAIN_NATIVE_OBJECT_METADATA",
                "新Name域对Original和Custom均不推断非空对象选择器");
        }
        for (const unsigned alias : {0u, 1u, 2u, 3u, 4u}) {
            auto fixture = BridgeNameFixture();
            auto row = fixture.metadata.back(); row["ID"] = "Als9";
            if (alias == 1 || alias == 3) row["slk"] = "";
            if (alias == 2 || alias == 3 || alias == 4) { row["field"] = "nam"; row["data"] = alias == 4 ? "broken" : "5"; }
            if (alias == 0) row["useSpecific"] = "Z999";
            fixture.metadata.push_back(std::move(row)); fixture.Tables();
            fixture.SetNative(BridgeNameDocument(custom, "Nm90", "display"));
            Rejected(fixture.Inspect({placed}), "TERRAIN_NATIVE_OBJECT_METADATA",
                "逻辑同名、条件候选、data拼接及被schema丢弃的原始行都不能制造假的唯一Name");
        }
        auto missingSource = BridgeNameFixture(); missingSource.SetNative(BridgeNameDocument(custom, "Nm90", "display"));
        missingSource.sources[kMetadata] = {"Units/DestructableMetaData.slk", false, {}};
        Rejected(missingSource.Inspect({placed}), "TERRAIN_NATIVE_OBJECT_TABLE_MISSING", "Name仍需完整实际metadata来源");
    }
    auto unused = BridgeNameFixture(); unused.metadata.back()["index"] = "1";
    unused.metadata.back()["useSpecific"] = "Z999"; unused.Tables(); unused.SetNative(CustomBridge());
    Require(Take(unused.Inspect({"N001"})).footprints.size() == 1,
        "未覆盖Name时不向旧桥域新增未使用名称字段的资格要求");
}

void BridgeNamesKeepNativeWireAndInheritanceGuards() {
    for (const bool custom : {false, true}) {
        const std::string placed = custom ? "N001" : "S001";
        for (const auto wire : {NativeValueType::Int32, NativeValueType::Real, NativeValueType::Unreal}) {
            auto fixture = BridgeNameFixture(); auto document = custom ? CustomBridge() : Empty();
            if (!custom) Check(document.AddOriginalObject(placed));
            Modification(document, placed, "Nm90", wire, wire == NativeValueType::Int32 ? Value{std::int32_t{0}} : Value{0.f});
            fixture.SetNative(document);
            Rejected(fixture.Inspect({placed}), "TERRAIN_NATIVE_OBJECT_BRIDGE_TYPE", "Name数值wire不能伪装成实际String");
        }
        auto unknown = BridgeNameFixture(); auto document = BridgeNameDocument(custom, "Nm90", "display");
        Modification(document, placed, "ZZZ9", NativeValueType::String, std::string("unknown")); unknown.SetNative(document);
        Rejected(unknown.Inspect({placed}), "TERRAIN_NATIVE_OBJECT_METADATA", "Name正例不能掩盖同一对象的未知字段");
        auto other = BridgeNameFixture(); other.metadata.push_back({{"ID", "Suf9"}, {"field", "EditorSuffix"},
            {"slk", "DestructableData"}, {"type", "string"}, {"index", "0"}}); other.Tables();
        auto modified = BridgeNameDocument(custom, "Nm90", "display");
        Modification(modified, placed, "Suf9", NativeValueType::String, std::string("suffix")); other.SetNative(modified);
        Rejected(other.Inspect({placed}), "TERRAIN_NATIVE_STATIC_OBJECT_UNSUPPORTED", "不把Name特例扩大为任意String字段");
    }
    auto duplicate = BridgeNameFixture(); duplicate.SetNative(BridgeNameDocument(false, "Nm90", "display"));
    auto& wire = duplicate.sources[kW3b].bytes;
    Require(wire.size() >= 36 && wire[20] == std::byte{'N'}, "单Original名称modifier的字面wire起点");
    const Bytes repeated(wire.begin() + 20, wire.end() - 4);
    wire.insert(wire.end() - 4, repeated.begin(), repeated.end()); Put32(wire, 16, 2);
    Rejected(duplicate.Inspect({"S001"}), "TERRAIN_NATIVE_OBJECT_DOCUMENT", "重复Name地址仍由完整classic codec拒绝");
    for (const bool emptyParent : {false, true}) {
        auto fixture = BridgeNameFixture(); auto document = BridgeNameDocument(true, "Nm90", "child display");
        Check(document.AddOriginalObject("S001"));
        if (!emptyParent) Modification(document, "S001", "Nm90", NativeValueType::String, std::string("parent display"));
        fixture.SetNative(document);
        if (emptyParent) Require(Take(fixture.Inspect({"N001"})).footprints.size() == 1, "合法空Original父保持已有兼容");
        else Rejected(fixture.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE",
            "父Name也是真实覆盖，不能因同为显示字段而猜测父覆盖继承");
    }
    auto chain = BridgeNameFixture(); auto document = BridgeNameDocument(true, "Nm90", "display");
    Check(document.AddCustomObject("N002", "N001")); chain.SetNative(document);
    Rejected(chain.Inspect({"N002"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "Name不放开多层Custom继承");
    auto crossClass = BridgeNameFixture(); crossClass.SetNative(BridgeNameDocument(true, "Nm90", "display"));
    auto parent = Empty(ObjectType::Doodad); Check(parent.AddOriginalObject("S001")); crossClass.SetNative(parent);
    Rejected(crossClass.Inspect({"N001"}), "TERRAIN_NATIVE_OBJECT_INHERITANCE", "Name不放开跨类别父记录");
}

void BridgeNameOverridesKeepExactPathing() {
    BridgeNamesKeepTheActualStaticContribution();
    BridgeNamesRequireTheFullUniqueScalarMetadata();
    BridgeNamesKeepNativeWireAndInheritanceGuards();
}
