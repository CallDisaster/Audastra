// 本文件只在TerrainMinimapMarkersTests.cpp的匿名命名空间中包含，复用原创归档夹具。
Bytes ExpectedObjectMarkers(const std::vector<std::array<std::int32_t, 3>>& ordinary) {
    Bytes bytes; Put32(bytes, 0); Put32(bytes, static_cast<std::uint32_t>(ordinary.size() + 2));
    for (const auto& record : ordinary) {
        for (const auto value : record) Put32(bytes, static_cast<std::uint32_t>(value));
        Put32(bytes, 0xffffffffu);
    }
    for (const auto position : {std::array<std::int32_t, 2>{32, 160}, {528, 240}}) {
        Put32(bytes, 2); Put32(bytes, position[0]); Put32(bytes, position[1]);
        for (const auto color : {45, 35, 25, 255}) bytes.push_back(static_cast<std::byte>(color));
    }
    return bytes;
}
Bytes ItemIdentityWire(const std::vector<std::string>& original,
    const std::vector<std::array<std::string, 2>>& custom) {
    Bytes bytes; Put32(bytes, 2); Put32(bytes, static_cast<std::uint32_t>(original.size()));
    for (const auto& id : original) {
        const auto raw = Text(id); bytes.insert(bytes.end(), raw.begin(), raw.end());
        Put32(bytes, 0); Put32(bytes, 0);
    }
    Put32(bytes, static_cast<std::uint32_t>(custom.size()));
    for (const auto& object : custom) {
        for (const auto& id : {object[1], object[0]}) {
            const auto raw = Text(id); bytes.insert(bytes.end(), raw.begin(), raw.end());
        }
        Put32(bytes, 0);
    }
    return bytes;
}
Bytes ItemOverrides(std::string label) {
    auto items = Take(NativeObjectDocument::CreateEmpty(ObjectType::Item));
    Check(items.AddOriginalObject("It01")); Check(items.AddCustomObject("Ci01", "It01"));
    // MMP只使用经过完整原生codec验证的物品身份，不解释物品的展示字段。
    Check(items.AddModification({ObjectType::Item, "It01"}, {"inam"}, NativeValueType::String, std::string{"parent"}));
    Check(items.AddModification({ObjectType::Item, "Ci01"}, {"inam"}, NativeValueType::String, std::move(label)));
    return Take(NativeObjectCodec(ObjectType::Item).Serialize(items));
}
void CustomItemsAndParentOriginalsSaveWithoutUnitIcons() {
    Fixture fixture;
    fixture.members.push_back({"war3map.wpm", Pathing(64, 32)});
    fixture.members.push_back({"Fixture/opaque.bin", {std::byte{0}, std::byte{0xff}, std::byte{7}}});
    fixture.members.push_back({"war3map.w3t", ItemOverrides("old label")});
    fixture.units.units.push_back(Unit("Ci01", 15, 0, 0, 9)); fixture.UpdateUnits();
    const auto expected = ExpectedObjectMarkers({{1, 128, 128}, {0, 0, 192}});
    Require(fixture.Capture()->Bytes() == expected, "自定义物品及其标准父Original不会变成单位建筑图标");
    MapSaveTask task; fixture.Freeze(task);
    const auto itemBytes = ItemOverrides("final label");
    const auto unitBytes = UnitOverride(NativeValueType::Int32, std::int32_t{1});
    task.replacements = {{"war3map.w3t", itemBytes}, {"war3map.w3u", unitBytes}};
    fixture.PrepareOrdinary(task); MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && task.minimapMarkers, "同次单位和物品修改正式保存并生成独立MMP");
    const auto saved = Take(MapArchiveSession::Open(task.target));
    Require(Take(saved.ReadMember("war3map.mmp")) == expected, "最终MMP完整字节与独立手写预期一致");
    Require(Take(saved.ReadMember("war3map.w3t")) == itemBytes
        && Take(saved.ReadMember("war3map.w3u")) == unitBytes, "两份实际物编覆盖完整落盘");
    for (const auto* name : {"war3map.w3e", "war3map.wpm", "war3map.shd", "war3map.j",
        "war3mapMap.tga", "Fixture/opaque.bin"})
        Require(Take(saved.ReadMember(name)) == fixture.Member(name), "MMP身份补齐不改WPM、SHD、地形、脚本和其它成员");
    Check(task.minimapMarkers->VerifyReadback(saved));
}
void ItemIdentityAmbiguityAndUnknownParentsReject() {
    Fixture fixture; fixture.units.units.push_back(Unit("Ci01", 15, 0, 0, 9)); fixture.UpdateUnits();
    const auto reject = [&](Bytes wire, std::string_view code) {
        const std::vector<MemberReplacement> output{{"war3map.w3t", std::move(wire)}};
        Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output), code);
    };
    reject(ItemIdentityWire({}, {{"Ci01", "No??"}}), "TERRAIN_MMP_OBJECT_UNKNOWN");
    reject(ItemIdentityWire({}, {{"Ci01", "Ci02"}, {"Ci02", "It01"}}), "TERRAIN_MMP_INHERITANCE");
    reject(ItemIdentityWire({}, {{"Ci01", "Ci01"}}), "TERRAIN_MMP_INHERITANCE");
    reject(ItemIdentityWire({"It01", "It01"}, {{"Ci01", "It01"}}), "TERRAIN_MMP_OBJECT_LAYOUT");
    reject(ItemIdentityWire({"Ci01"}, {{"Ci01", "It01"}}), "TERRAIN_MMP_OBJECT_LAYOUT");
    reject(ItemIdentityWire({}, {{"Ci01", "It01"}, {"Ci01", "It01"}}), "TERRAIN_MMP_OBJECT_LAYOUT");
    reject(ItemIdentityWire({"Ci01"}, {}), "TERRAIN_MMP_OBJECT_UNKNOWN");
    reject(ItemIdentityWire({}, {{"Ci01", "S001"}}), "TERRAIN_MMP_OBJECT_TYPE");
    fixture.units.units.back().rawcode = "S001"; fixture.UpdateUnits();
    reject(ItemIdentityWire({}, {{"S001", "It01"}}), "TERRAIN_MMP_OBJECT_TYPE");
    fixture.units.units.back().rawcode = "It01"; fixture.UpdateUnits();
    reject(ItemIdentityWire({}, {{"It01", "It01"}}), "TERRAIN_MMP_INHERITANCE");
    auto units = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
    Check(units.AddCustomObject("It01", "S001"));
    const std::vector<MemberReplacement> cross{{"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(units))}};
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, cross), "TERRAIN_MMP_OBJECT_TYPE");
}
void ItemSourcesBindEvenWhenMarkerBytesDoNotChange() {
    Fixture fixture;
    const auto missing = fixture.Capture();
    Require(std::any_of(missing->Dependencies().begin(), missing->Dependencies().end(), [](const auto& dependency) {
        return dependency.name == "WAR3MAP.W3T" && dependency.source == TerrainMinimapMarkerSource::Missing;
    }), "普通单位也必须绑定物品表确实缺失，以排除后来出现的跨类型身份");
    std::vector<MemberReplacement> output{{"war3map.mmp", missing->Bytes()}, {"war3map.w3t", ItemOverrides("one")}};
    Rejected(missing->VerifyFrozenOutput(*fixture.source, output), "TERRAIN_MMP_DEPENDENCY_STALE");
    const auto present = fixture.Capture(output);
    Require(present->Bytes() == missing->Bytes(), "物品展示字段不影响图标正文但仍是实际来源");
    Check(present->VerifyFrozenOutput(*fixture.source, output));
    output.back().data = ItemOverrides("two");
    Rejected(present->VerifyFrozenOutput(*fixture.source, output), "TERRAIN_MMP_DEPENDENCY_STALE");
    const auto forged = Take(fixture.source->SaveCandidate(fixture.root / "changed-item-source.w3x", output));
    Rejected(present->VerifyReadback(Take(MapArchiveSession::Open(forged.path))), "TERRAIN_MMP_READBACK_STALE");
    fixture.members.push_back({"war3map.w3t", ItemOverrides("one")}); fixture.Open();
    const auto authored = fixture.Capture();
    const std::vector<MemberReplacement> installed{{"war3map.mmp", authored->Bytes()}};
    const std::vector<std::string> removed{"war3map.w3t"};
    Rejected(authored->VerifyFrozenOutput(*fixture.source, installed, removed), "TERRAIN_MMP_DEPENDENCY_STALE");
}
void FinalItemDependencyChangesCannotPublishOrBecomeWarnings() {
    for (unsigned mode = 0; mode < 3; ++mode) {
        Fixture fixture;
        if (mode) { fixture.members.push_back({"war3map.w3t", ItemOverrides("captured")}); fixture.Open(); }
        MapSaveTask task; fixture.Freeze(task); fixture.PrepareOrdinary(task);
        task.beforePublish = [&]() -> Status {
            Require(task.minimapMarkers != nullptr, "实际owner必须先在最终派生阶段捕获物品表或其缺失");
            if (mode == 2) task.removals.push_back("war3map.w3t");
            else task.replacements.push_back({"war3map.w3t", ItemOverrides("changed before publish")});
            return Status::Ok();
        };
        MapSaveService::Run(task);
        Require(task.executed && !task.succeeded && !task.nextSession && task.written.empty()
            && !std::filesystem::exists(task.target), "最终w3t新增、修改或删除均须在写目标之前拒绝");
        Code(task.diagnostics, "TERRAIN_MMP_DEPENDENCY_STALE");
        Require(std::none_of(task.diagnostics.begin(), task.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "TERRAIN_MMP_PRESERVED_UNSUPPORTED";
        }), "已捕获的w3t来源变化不得降级为保留图标warning");
    }
}
void UnknownItemParentPreservesMarkersAndUnrelatedSave() {
    Fixture fixture; const auto original = ExpectedObjectMarkers({{1, 128, 128}, {0, 0, 192}});
    fixture.members.push_back({"war3map.mmp", original});
    const auto items = ItemIdentityWire({}, {{"Ci01", "No??"}});
    fixture.members.push_back({"war3map.w3t", items});
    fixture.units.units.push_back(Unit("Ci01", 15, 0, 0, 9)); fixture.UpdateUnits();
    MapSaveTask task; fixture.Freeze(task);
    const auto units = UnitOverride(NativeValueType::Int32, std::int32_t{0});
    task.replacements.push_back({"war3map.w3u", units}); fixture.PrepareOrdinary(task); MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession && !task.minimapMarkers, "未知物品父保留原MMP且允许其它正常编辑保存");
    Code(task.diagnostics, "TERRAIN_MMP_PRESERVED_UNSUPPORTED");
    const auto saved = Take(MapArchiveSession::Open(task.target));
    Require(Take(saved.ReadMember("war3map.mmp")) == original
        && Take(saved.ReadMember("war3map.w3t")) == items && Take(saved.ReadMember("war3map.w3u")) == units,
        "能力不足不重写未知物品或旧图标，也不丢本次物编修改");
}
Bytes ParentMetadata(bool duplicate = false) {
    std::vector<std::vector<std::string>> rows{
        {"p001", "isbldg", "UnitBalance", "bool", "0", "0", "-1"},
        {"p002", "race", "UnitData", "unitRace", "0", "0", "-1"},
        {"p003", "nbmmIcon", "UnitUI", "bool", "0", "0", "-1"},
        {"p004", "abilList", "UnitAbilities", "abilityList", "0", "0", "-1"},
        {"p005", "HP", "UnitBalance", "int", "0", "0", "-1"},
        {"p007", "Name", "Profile", "string", "0", "0", "0", "S001"}};
    if (duplicate) rows.push_back({"p006", "HP", "UnitBalance", "int", "0", "0", "-1"});
    return Slk({"ID", "field", "slk", "type", "repeat", "data", "index", "useSpecific"}, rows);
}
NativeObjectDocument ParentUnitDocument() {
    auto units = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
    Check(units.AddOriginalObject("S001"));
    Check(units.AddModification({ObjectType::Unit, "S001"}, {"p001"}, NativeValueType::Int32, std::int32_t{0}));
    Check(units.AddModification({ObjectType::Unit, "S001"}, {"p002"}, NativeValueType::String, std::string{"human"}));
    Check(units.AddModification({ObjectType::Unit, "S001"}, {"p003"}, NativeValueType::Int32, std::int32_t{0}));
    Check(units.AddModification({ObjectType::Unit, "S001"}, {"p004"}, NativeValueType::String, std::string{"Agld"}));
    Check(units.AddModification({ObjectType::Unit, "S001"}, {"p005"}, NativeValueType::Int32, std::int32_t{1234}));
    Check(units.AddCustomObject("New1", "S001")); Check(units.AddCustomObject("New2", "S001"));
    return units;
}
void StandardParentOriginalDoesNotPolluteCustomFallback() {
    Fixture fixture; fixture.Member("Units/UnitMetaData.slk") = ParentMetadata();
    fixture.units.units[1].rawcode = "New1";
    fixture.units.units.push_back(Unit("New2", 15, 128, 512, 9));
    fixture.units.units.push_back(Unit("S001", 15, 0, 0, 10)); fixture.UpdateUnits();
    auto units = ParentUnitDocument();
    std::vector<MemberReplacement> output{{"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(units))}};
    const auto expected = ExpectedObjectMarkers({{1, 128, 128}, {0, 0, 192}, {1, 32, 160}});
    Require(fixture.Capture(output)->Bytes() == expected,
        "两个child未覆字段回标准SLK，不继承父Original的关闭建筑/人类/关闭图标/矿能力；直接摆父仍用Original");
    Check(units.AddModification({ObjectType::Unit, "New1"}, {"p001"}, NativeValueType::Int32, std::int32_t{0}));
    output.front().data = Take(NativeObjectCodec(ObjectType::Unit).Serialize(units));
    Require(fixture.Capture(output)->Bytes() == ExpectedObjectMarkers({{0, 0, 192}, {1, 32, 160}}),
        "child自己的覆盖优先于标准父SLK，不能被忽略的parent覆盖替代");
    auto limits = TerrainMinimapMarkerLimits{}; std::size_t low = 1, high = limits.maximumWorkingBytes;
    while (low < high) {
        limits.maximumWorkingBytes = low + (high - low) / 2;
        if (TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output, {}, limits).Succeeded()) high = limits.maximumWorkingBytes;
        else low = limits.maximumWorkingBytes + 1;
    }
    limits.maximumWorkingBytes = low;
    Require(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output, {}, limits).Succeeded(), "完整物编来源和实际读取字段计入精确工作预算");
    --limits.maximumWorkingBytes;
    Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output, {}, limits));
}
void IgnoredParentFieldsKeepActualChildAndParentGates() {
    Fixture fixture; fixture.Member("Units/UnitMetaData.slk") = ParentMetadata();
    // 两个直接父摆放都改成child；owner0也会读取isbldg，不能用它证明父字段未访问。
    fixture.units.units[1].rawcode = "New1"; fixture.units.units[4].rawcode = "New2"; fixture.UpdateUnits();
    const auto expected = ExpectedObjectMarkers({{1, 128, 128}, {0, 0, 192}});
    const auto requireIgnored = [&](const NativeObjectDocument& units) {
        const std::vector<MemberReplacement> output{{"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(units))}};
        Require(fixture.Capture(output)->Bytes() == expected, "不被child读取的父Profile、selector或元数据未知但字节边界明确的字段不能阻断MMP");
    };
    const auto reject = [&](NativeObjectDocument units, std::string_view code) {
        const std::vector<MemberReplacement> output{{"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(units))}};
        Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output), code);
    };
    auto unknown = ParentUnitDocument();
    Check(unknown.AddModification({ObjectType::Unit, "S001"}, {"what"}, NativeValueType::Int32, std::int32_t{1}));
    Check(unknown.AddModification({ObjectType::Unit, "S001"}, {"p007"}, NativeValueType::String, std::string{"ignored profile"}));
    requireIgnored(unknown);
    auto wrongParentWire = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
    Check(wrongParentWire.AddOriginalObject("S001"));
    Check(wrongParentWire.AddCustomObject("New1", "S001")); Check(wrongParentWire.AddCustomObject("New2", "S001"));
    Check(wrongParentWire.AddModification({ObjectType::Unit, "S001"}, {"p001"}, NativeValueType::Real, 1.0f));
    requireIgnored(wrongParentWire); // wire边界已知，但这个父值没有被getter访问。
    auto wrongChildWire = ParentUnitDocument();
    Check(wrongChildWire.AddModification({ObjectType::Unit, "New1"}, {"p001"}, NativeValueType::Real, 1.0f));
    reject(std::move(wrongChildWire), "TERRAIN_MMP_OBJECT_TYPE");
    fixture.units.units[1].rawcode = "S001"; fixture.UpdateUnits();
    reject(wrongParentWire, "TERRAIN_MMP_OBJECT_TYPE");
    fixture.units.units[1].rawcode = "New1"; fixture.UpdateUnits();
    fixture.Member("Units/UnitMetaData.slk") = ParentMetadata(true); fixture.Open();
    requireIgnored(unknown); // 父HP的重复metadata地址没有成为child实际字段。
    fixture.Member("Units/UnitMetaData.slk") = Slk({"ID", "field", "slk", "type", "repeat", "data", "index"},
        {{"p001", "isbldg", "UnitBalance", "bool", "0", "0", "-1"},
         {"p008", "isbldg", "UnitBalance", "bool", "0", "0", "-1"}}); fixture.Open();
    reject(ParentUnitDocument(), "TERRAIN_MMP_METADATA");
    fixture.Member("Units/UnitMetaData.slk") = ParentMetadata(); fixture.Open();
    auto chain = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
    Check(chain.AddCustomObject("New1", "New2")); Check(chain.AddCustomObject("New2", "S001"));
    reject(std::move(chain), "TERRAIN_MMP_INHERITANCE");
    const auto rejectWire = [&](Bytes wire, std::string_view code) {
        const std::vector<MemberReplacement> output{{"war3map.w3u", std::move(wire)}};
        Rejected(TerrainMinimapMarkers::Capture(*fixture.source, fixture.game, output), code);
    };
    // 单位/物品非extended记录具有相同身份头；手写wire覆盖codec不允许创建的歧义源。
    rejectWire(ItemIdentityWire({}, {{"New1", "No??"}}), "TERRAIN_MMP_OBJECT_UNKNOWN");
    rejectWire(ItemIdentityWire({}, {{"New1", "New1"}}), "TERRAIN_MMP_INHERITANCE");
    rejectWire(ItemIdentityWire({"New1"}, {{"New1", "S001"}}), "TERRAIN_MMP_OBJECT_LAYOUT");
    auto unknownWire = Take(NativeObjectCodec(ObjectType::Unit).Serialize(ParentUnitDocument()));
    // v2头8字节、首个Original头12字节；首字段ID之后是实际wire类型。
    unknownWire[24] = std::byte{0x7f};
    rejectWire(std::move(unknownWire), "native-value-type-unsupported");
    auto duplicateField = Take(NativeObjectCodec(ObjectType::Unit).Serialize(ParentUnitDocument()));
    const Bytes firstField(duplicateField.begin() + 20, duplicateField.begin() + 36);
    duplicateField.insert(duplicateField.begin() + 36, firstField.begin(), firstField.end());
    duplicateField[16] = std::byte{6}; // 五条父覆盖加一条完全重复的Int32字段。
    rejectWire(std::move(duplicateField), "TERRAIN_MMP_OBJECT_LAYOUT");
}
void MinimapMarkerObjectCoverageCases() {
    CustomItemsAndParentOriginalsSaveWithoutUnitIcons(); ItemIdentityAmbiguityAndUnknownParentsReject();
    ItemSourcesBindEvenWhenMarkerBytesDoNotChange(); FinalItemDependencyChangesCannotPublishOrBecomeWarnings();
    UnknownItemParentPreservesMarkersAndUnrelatedSave(); StandardParentOriginalDoesNotPolluteCustomFallback();
    IgnoredParentFieldsKeepActualChildAndParentGates();
}
